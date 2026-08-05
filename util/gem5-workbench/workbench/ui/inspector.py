"""Contextual, schema-driven Inspector for the current project selection."""

from dataclasses import dataclass
from enum import Enum
from typing import Mapping

import pygame
from workbench.model import (
    Component,
    Connection,
    Endpoint,
)
from workbench.model.values import JsonValue
from workbench.registry import (
    ComponentDefinition,
    ComponentRegistry,
    ComponentSupport,
    ParameterDefinition,
    ParameterType,
    create_builtin_registry,
)
from workbench.state import WorkbenchState
from workbench.ui.base import Panel
from workbench.ui.inspector_descriptors import (
    DEFAULT_INSPECTOR_DESCRIPTORS,
    ComponentInspectorDescriptor,
    InspectorDescriptorProvider,
    PropertyFieldDescriptor,
)
from workbench.ui.theme import Theme
from workbench.ui.widgets import (
    TextInput,
    display_text,
    draw_panel,
    draw_text,
    ellipsize,
    get_font,
)
from workbench.validation import (
    Diagnostic,
    DiagnosticSeverity,
)

_DIAGNOSTIC_ORDER = {
    DiagnosticSeverity.ERROR: 0,
    DiagnosticSeverity.WARNING: 1,
    DiagnosticSeverity.INFO: 2,
}


class InspectorMode(Enum):
    PROJECT = "Project"
    TEMPLATE = "Template"
    COMPONENT = "Component"
    CONNECTION = "Connection"


@dataclass(slots=True)
class _ParameterEditor:
    """A rendered parameter field; attributes remain test-adapter friendly."""

    definition: ParameterDefinition
    rect: pygame.Rect
    label: str = ""
    section_id: str = ""
    text_input: TextInput | None = None
    diagnostics: tuple[str, ...] = ()
    diagnostic_severity: DiagnosticSeverity | None = None


@dataclass(slots=True)
class _ContentBlock:
    kind: str
    y: int
    height: int
    label: str = ""
    value: object = ""
    editor: _ParameterEditor | None = None
    severity: DiagnosticSeverity | None = None


class Inspector(Panel):
    """Inspect templates, instances, connections, and project-wide state."""

    PADDING = 16
    HEADER_HEIGHT = 50
    SECTION_HEIGHT = 34
    ROW_HEIGHT = 46
    PARAGRAPH_HEIGHT = 54
    PARAMETER_HEIGHT = 62
    DIAGNOSTIC_HEIGHT = 54

    def __init__(
        self,
        rect: pygame.Rect,
        theme: Theme,
        registry: ComponentRegistry | None = None,
        descriptors: InspectorDescriptorProvider | None = None,
    ) -> None:
        super().__init__(rect, theme)
        self.registry = registry or create_builtin_registry()
        self.descriptors = descriptors or DEFAULT_INSPECTOR_DESCRIPTORS
        self.mode = InspectorMode.PROJECT
        self.active_descriptor: ComponentInspectorDescriptor | None = None
        self._editors: list[_ParameterEditor] = []
        self._blocks: list[_ContentBlock] = []
        self._context_key: tuple[object, ...] | None = None
        self._context_identity: tuple[object, ...] | None = None
        self._registry_revision = 0
        self._content_height = 0
        self._cursor = 0
        self._scroll = 0

    @property
    def has_focus(self) -> bool:
        return any(
            editor.text_input is not None and editor.text_input.focused
            for editor in self._editors
        )

    def commit_focused(self) -> bool:
        """Commit the active property before a global save or run shortcut."""

        for editor in self._editors:
            text_input = editor.text_input
            if text_input is not None and text_input.focused:
                return text_input.commit()
        return True

    def cancel_focused(self) -> None:
        """Discard an active draft when its project context is replaced."""

        for editor in self._editors:
            text_input = editor.text_input
            if text_input is not None and text_input.focused:
                text_input.revert()
                return

    @property
    def section_titles(self) -> tuple[str, ...]:
        """Return visible section names for tests and accessibility adapters."""

        return tuple(
            block.label for block in self._blocks if block.kind == "section"
        )

    def set_registry(self, registry: ComponentRegistry) -> None:
        self.registry = registry
        self._registry_revision += 1
        self._invalidate_context()

    def set_rect(self, rect: pygame.Rect) -> None:
        super().set_rect(rect)
        self._scroll = max(0, min(self._scroll, self._max_scroll()))
        self._layout_editors()

    def _invalidate_context(self) -> None:
        self._context_key = None

    def _identity_for(self, state: WorkbenchState) -> tuple[object, ...]:
        mode = self._mode_for(state)
        project_identity = id(state.document.project)
        if mode is InspectorMode.COMPONENT:
            component = state.document.project.components[
                state.selected_component_id
            ]
            return (
                mode,
                project_identity,
                component.id,
                component.type_id,
            )
        if mode is InspectorMode.CONNECTION:
            return (mode, project_identity, state.selected_connection_id)
        if mode is InspectorMode.TEMPLATE:
            return (mode, project_identity, state.selected_type_id)
        return (mode, project_identity)

    def _focused_editor_snapshot(
        self,
    ) -> tuple[tuple[object, ...] | None, str, str, str] | None:
        for editor in self._editors:
            text_input = editor.text_input
            if text_input is not None and text_input.focused:
                return (
                    self._context_identity,
                    editor.definition.id,
                    text_input.text,
                    text_input._committed_text,
                )
        return None

    def _restore_focused_editor(
        self,
        snapshot: tuple[tuple[object, ...] | None, str, str, str] | None,
    ) -> None:
        if snapshot is None or snapshot[0] != self._context_identity:
            return
        _, parameter_id, text, committed_text = snapshot
        for editor in self._editors:
            text_input = editor.text_input
            if (
                editor.definition.id == parameter_id
                and text_input is not None
                and text_input.enabled
            ):
                if text_input.text != committed_text:
                    return
                text_input.text = text
                text_input._committed_text = committed_text
                text_input.focused = True
                return

    @staticmethod
    def _mode_for(state: WorkbenchState) -> InspectorMode:
        if state.selected_connection_id in state.document.project.connections:
            return InspectorMode.CONNECTION
        if state.selected_component_id in state.document.project.components:
            return InspectorMode.COMPONENT
        if state.selected_type_id is not None:
            return InspectorMode.TEMPLATE
        return InspectorMode.PROJECT

    @staticmethod
    def _diagnostic_signature(state: WorkbenchState) -> tuple[object, ...]:
        return tuple(
            (
                item.code,
                item.message,
                item.severity.value,
                item.layer.value,
                item.component_id,
                item.connection_id,
                item.field,
            )
            for item in state.diagnostics
        )

    def _make_context_key(self, state: WorkbenchState) -> tuple[object, ...]:
        mode = self._mode_for(state)
        base = (
            mode,
            self._registry_revision,
            len(self.registry.definitions()),
            id(state.document.project),
            state.document.project.revision,
            self._diagnostic_signature(state),
        )
        if mode is InspectorMode.CONNECTION:
            return base + (state.selected_connection_id,)
        if mode is InspectorMode.COMPONENT:
            return base + (state.selected_component_id,)
        if mode is InspectorMode.TEMPLATE:
            return base + (state.selected_type_id,)
        return base + (
            state.catalog_message,
            state.gem5_binary,
            state.job_kind,
            state.job_state,
            tuple(state.job_log_lines[-8:]),
            state.job_output_path,
            state.can_run,
            state.run_block_reason,
        )

    def _ensure_context(self, state: WorkbenchState) -> None:
        key = self._make_context_key(state)
        if key == self._context_key:
            return
        identity = self._identity_for(state)
        preserve_interaction = identity == self._context_identity
        previous_scroll = self._scroll if preserve_interaction else 0
        focused_editor = (
            self._focused_editor_snapshot() if preserve_interaction else None
        )
        self._context_key = key
        self.mode = self._mode_for(state)
        self._context_identity = identity
        self.active_descriptor = None
        self._editors.clear()
        self._blocks.clear()
        self._cursor = 0

        if self.mode is InspectorMode.CONNECTION:
            connection = state.document.project.connections[
                state.selected_connection_id
            ]
            self._build_connection(connection, state)
        elif self.mode is InspectorMode.COMPONENT:
            component = state.document.project.components[
                state.selected_component_id
            ]
            self._build_component(component, state)
        elif self.mode is InspectorMode.TEMPLATE:
            self._build_template(state.selected_type_id, state)
        else:
            self._build_project(state)

        self._content_height = self._cursor + self.PADDING
        self._scroll = min(max(0, previous_scroll), self._max_scroll())
        self._layout_editors()
        self._restore_focused_editor(focused_editor)

    def _add_block(
        self,
        kind: str,
        height: int,
        label: str = "",
        value: object = "",
        *,
        editor: _ParameterEditor | None = None,
        severity: DiagnosticSeverity | None = None,
    ) -> None:
        self._blocks.append(
            _ContentBlock(
                kind,
                self._cursor,
                height,
                label,
                value,
                editor,
                severity,
            )
        )
        self._cursor += height

    def _add_section(self, title: str) -> None:
        self._add_block("section", self.SECTION_HEIGHT, title)

    def _add_row(
        self, label: str, value: object, *, kind: str = "row"
    ) -> None:
        self._add_block(kind, self.ROW_HEIGHT, label, value)

    def _add_paragraph(self, value: object) -> None:
        self._add_block("paragraph", self.PARAGRAPH_HEIGHT, value=value)

    def _add_diagnostics(self, diagnostics: list[Diagnostic]) -> None:
        if not diagnostics:
            return
        self._add_section("Diagnostics")
        for diagnostic in sorted(
            diagnostics,
            key=lambda item: (
                _DIAGNOSTIC_ORDER[item.severity],
                item.code,
            ),
        ):
            self._add_block(
                "diagnostic",
                self.DIAGNOSTIC_HEIGHT,
                diagnostic.severity.value.title(),
                diagnostic.message,
                severity=diagnostic.severity,
            )

    @staticmethod
    def _definition_defaults(
        definition: ComponentDefinition,
    ) -> dict[str, JsonValue]:
        return {
            parameter.id: parameter.default
            for parameter in definition.parameters.values()
            if parameter.has_default
        }

    def _build_template(
        self, type_id: str | None, state: WorkbenchState
    ) -> None:
        definition = self.registry.get(type_id) if type_id else None
        self._add_section("Component template")
        if definition is None:
            self._add_row("Type ID", type_id or "Unknown")
            self._add_paragraph(
                "This component type is not present in the active catalog."
            )
            return

        self.active_descriptor = self.descriptors.descriptor_for(definition)
        self._add_row("Name", definition.display_name)
        self._add_row("Category", definition.category)
        self._add_row("Support", definition.support.value.title())
        if self.active_descriptor.generated:
            self._add_row("Property schema", "Generated from catalog metadata")
        if definition.description:
            self._add_paragraph(definition.description)
        if definition.support is not ComponentSupport.SUPPORTED:
            self._add_block(
                "diagnostic",
                self.DIAGNOSTIC_HEIGHT,
                "Catalog status",
                definition.support_reason
                or "No audited execution adapter is available for this type.",
                severity=(
                    DiagnosticSeverity.WARNING
                    if definition.support is ComponentSupport.EXPERIMENTAL
                    else DiagnosticSeverity.ERROR
                ),
            )
        if definition.source_reference:
            self._add_row("Source", definition.source_reference)

        defaults = self._definition_defaults(definition)
        self._add_property_preview(definition, defaults)
        self._add_ports_preview(definition)
        if definition.support is ComponentSupport.SUPPORTED:
            self._add_section("Placement")
            self._add_paragraph(
                f"{definition.display_name} selected — click the canvas to "
                "place it. Parameters start with the values shown above."
            )

    def _add_property_preview(
        self,
        definition: ComponentDefinition,
        parameters: dict[str, JsonValue],
    ) -> None:
        descriptor = self.active_descriptor
        if descriptor is None:
            return
        for section in descriptor.sections:
            visible = tuple(
                field
                for field in section.fields
                if field.is_visible(parameters)
            )
            if not visible:
                continue
            self._add_section(section.title)
            for field in visible:
                parameter = definition.parameters[field.parameter_id]
                if parameter.has_default:
                    value: object = parameter.default
                elif parameter.required:
                    value = "Required — no default"
                else:
                    value = "Not set"
                self._add_row(field.display_label, value)

    def _add_ports_preview(self, definition: ComponentDefinition) -> None:
        self._add_section("Ports")
        if not definition.ports:
            self._add_paragraph(
                "This component declares no connectable ports."
            )
            return
        for port in definition.ports.values():
            flags = [port.direction.value, port.interface]
            if port.vector:
                flags.append("vector")
            if port.required:
                flags.append("required")
            self._add_row(port.id, " · ".join(flags), kind="port")

    def _build_component(
        self, component: Component, state: WorkbenchState
    ) -> None:
        definition = self.registry.get(component.type_id)
        self._add_section("Component")
        self._add_row(
            "Type",
            (
                definition.display_name
                if definition is not None
                else f"Unknown ({component.type_id})"
            ),
        )
        self._add_row("Component ID", component.id)
        if definition is None:
            self._add_stored_parameters(component.parameters)
            self._add_diagnostics(
                [
                    item
                    for item in state.diagnostics
                    if item.component_id == component.id
                ]
            )
            return

        self.active_descriptor = self.descriptors.descriptor_for(definition)
        self._add_row(
            "Category · support",
            f"{definition.category} · {definition.support.value.title()}",
        )

        component_diagnostics = [
            item
            for item in state.diagnostics
            if item.component_id == component.id
        ]
        inline_diagnostics: set[Diagnostic] = set()
        for section in self.active_descriptor.sections:
            visible = tuple(
                field
                for field in section.fields
                if field.is_visible(component.parameters)
            )
            if not visible:
                continue
            self._add_section(section.title)
            for field in visible:
                field_diagnostics = tuple(
                    sorted(
                        (
                            item
                            for item in component_diagnostics
                            if item.field == f"parameters.{field.parameter_id}"
                        ),
                        key=lambda item: (
                            _DIAGNOSTIC_ORDER[item.severity],
                            item.code,
                        ),
                    )
                )
                if field_diagnostics:
                    inline_diagnostics.add(field_diagnostics[0])
                self._add_parameter_editor(
                    definition.parameters[field.parameter_id],
                    field,
                    section.id,
                    (
                        (field_diagnostics[0].message,)
                        if field_diagnostics
                        else ()
                    ),
                    (
                        field_diagnostics[0].severity
                        if field_diagnostics
                        else None
                    ),
                    state,
                )

        extension_parameters = {
            parameter_id: value
            for parameter_id, value in component.parameters.items()
            if parameter_id not in definition.parameters
        }
        if extension_parameters:
            self._add_section("Stored extension parameters")
            for parameter_id, value in extension_parameters.items():
                self._add_row(parameter_id.replace("_", " ").title(), value)

        if (
            definition.description
            or definition.source_reference
            or self.active_descriptor.generated
            or definition.support is not ComponentSupport.SUPPORTED
        ):
            self._add_section("Type information")
        if self.active_descriptor.generated:
            self._add_row("Property schema", "Generated from catalog metadata")
        if definition.description:
            self._add_paragraph(definition.description)
        if definition.support is not ComponentSupport.SUPPORTED:
            self._add_block(
                "diagnostic",
                self.DIAGNOSTIC_HEIGHT,
                "Catalog status",
                definition.support_reason
                or "No audited execution adapter is available for this type.",
                severity=(
                    DiagnosticSeverity.WARNING
                    if definition.support is ComponentSupport.EXPERIMENTAL
                    else DiagnosticSeverity.ERROR
                ),
            )
        if definition.source_reference:
            self._add_row("Source", definition.source_reference)

        self._add_component_ports(component, definition, state)
        self._add_diagnostics(
            [
                item
                for item in component_diagnostics
                if item not in inline_diagnostics
            ]
        )

    def _add_stored_parameters(self, parameters: Mapping[str, object]) -> None:
        self._add_section("Stored parameters")
        if not parameters:
            self._add_paragraph("This component has no stored parameters.")
            return
        for parameter_id, value in parameters.items():
            self._add_row(parameter_id.replace("_", " ").title(), value)

    def _add_parameter_editor(
        self,
        parameter: ParameterDefinition,
        field: PropertyFieldDescriptor,
        section_id: str,
        diagnostics: tuple[str, ...],
        diagnostic_severity: DiagnosticSeverity | None,
        state: WorkbenchState,
    ) -> None:
        editor = _ParameterEditor(
            parameter,
            pygame.Rect(0, 0, 0, 0),
            field.display_label,
            section_id,
            diagnostics=diagnostics,
            diagnostic_severity=diagnostic_severity,
        )
        if parameter.value_type not in (
            ParameterType.BOOLEAN,
            ParameterType.ENUM,
        ):
            component = state.document.project.components.get(
                state.selected_component_id
            )
            value = (
                component.parameters.get(parameter.id) if component else None
            )
            text_input = TextInput(pygame.Rect(0, 0, 0, 0))
            text_input.set_value(value)
            text_input.enabled = not parameter.read_only
            text_input.on_commit = (
                lambda text, item=parameter: self._commit_text(
                    item, text, state
                )
            )
            editor.text_input = text_input
        self._editors.append(editor)
        height = self.PARAMETER_HEIGHT + (22 if diagnostics else 0)
        self._add_block("editor", height, editor=editor)

    def _add_component_ports(
        self,
        component: Component,
        definition: ComponentDefinition,
        state: WorkbenchState,
    ) -> None:
        self._add_section("Ports & connections")
        if not definition.ports:
            self._add_paragraph(
                "This component declares no connectable ports."
            )
            return
        for port in definition.ports.values():
            connected = []
            for connection in state.document.project.connections.values():
                if (
                    connection.source.component_id == component.id
                    and connection.source.port_id == port.id
                ):
                    connected.append(connection.target)
                elif (
                    connection.target.component_id == component.id
                    and connection.target.port_id == port.id
                ):
                    connected.append(connection.source)
            if connected:
                value = ", ".join(
                    self._endpoint_label(endpoint, state)
                    for endpoint in connected
                )
            elif port.required:
                value = "Not connected — required"
            else:
                value = "Not connected"
            flags = port.direction.value
            if port.vector:
                flags += " · vector"
            self._add_row(f"{port.id} · {flags}", value, kind="port")

    def _build_connection(
        self, connection: Connection, state: WorkbenchState
    ) -> None:
        self._add_section("Connection")
        self._add_row("Connection ID", connection.id)
        self._add_row("Source", self._endpoint_label(connection.source, state))
        self._add_row("Target", self._endpoint_label(connection.target, state))
        source_interface = self._endpoint_interface(connection.source, state)
        target_interface = self._endpoint_interface(connection.target, state)
        if source_interface or target_interface:
            interface = source_interface or target_interface
            if (
                source_interface
                and target_interface
                and source_interface != target_interface
            ):
                interface = f"{source_interface} ↔ {target_interface}"
            self._add_row("Interface", interface)
        slots = []
        if connection.source.slot is not None:
            slots.append(f"source {connection.source.slot}")
        if connection.target.slot is not None:
            slots.append(f"target {connection.target.slot}")
        if slots:
            self._add_row("Vector slots", ", ".join(slots))
        if connection.parameters:
            self._add_section("Connection metadata")
            for key, value in connection.parameters.items():
                self._add_row(key.replace("_", " ").title(), value)
        self._add_section("Actions")
        self._add_paragraph(
            "Press Delete or Backspace to remove this explicit connection."
        )
        self._add_diagnostics(
            [
                item
                for item in state.diagnostics
                if item.connection_id == connection.id
            ]
        )

    def _endpoint_label(
        self, endpoint: Endpoint, state: WorkbenchState
    ) -> str:
        component = state.document.project.components.get(
            endpoint.component_id
        )
        if component is None:
            name = f"Missing component {endpoint.component_id}"
        else:
            definition = self.registry.get(component.type_id)
            name = definition.display_name if definition else component.type_id
        slot = f"[{endpoint.slot}]" if endpoint.slot is not None else ""
        identity = (
            f" ({endpoint.component_id})" if component is not None else ""
        )
        return f"{name}{identity} · {endpoint.port_id}{slot}"

    def _endpoint_interface(
        self, endpoint: Endpoint, state: WorkbenchState
    ) -> str | None:
        component = state.document.project.components.get(
            endpoint.component_id
        )
        definition = (
            self.registry.get(component.type_id) if component else None
        )
        port = definition.ports.get(endpoint.port_id) if definition else None
        return port.interface if port else None

    def _build_project(self, state: WorkbenchState) -> None:
        project = state.document.project
        name = project.metadata.get("name", "Untitled project")
        self._add_section("Project overview")
        self._add_row("Name", name)
        self._add_row("Components", len(project.components))
        self._add_row("Connections", len(project.connections))
        errors = sum(
            item.severity is DiagnosticSeverity.ERROR
            for item in state.diagnostics
        )
        warnings = sum(
            item.severity is DiagnosticSeverity.WARNING
            for item in state.diagnostics
        )
        self._add_row("Validation", f"{errors} errors · {warnings} warnings")

        self._add_section("gem5 environment")
        self._add_row("Binary", state.gem5_binary or "Not configured")
        self._add_row("Catalog", state.catalog_message)
        if state.can_run:
            readiness = "Ready to run"
        else:
            readiness = state.run_block_reason or "Not ready"
        self._add_row("Readiness", readiness)
        if state.job_state != "idle" or state.job_kind:
            self._add_row(
                "Current job",
                " · ".join(
                    item for item in (state.job_kind, state.job_state) if item
                ),
            )
        if state.job_output_path:
            self._add_row("Output", state.job_output_path)
        if state.job_log_lines:
            self._add_section("Recent job output")
            for line in state.job_log_lines[-6:]:
                self._add_paragraph(line)
        self._add_diagnostics(list(state.diagnostics))

    @staticmethod
    def _parse_text(parameter: ParameterDefinition, text: str) -> JsonValue:
        if text == "" and parameter.nullable:
            return None
        if parameter.required and not text:
            raise ValueError("value is required")
        if parameter.value_type is ParameterType.INTEGER:
            return int(text)
        if parameter.value_type is ParameterType.NUMBER:
            return float(text)
        return text

    def _set_parameter(
        self,
        parameter: ParameterDefinition,
        value: JsonValue,
        state: WorkbenchState,
    ) -> bool:
        parameter_label = display_text(parameter.id)
        component_id = state.selected_component_id
        component = state.document.project.components.get(component_id)
        if component is None or parameter.read_only:
            return False
        if (
            isinstance(value, (int, float))
            and not isinstance(value, bool)
            and parameter.minimum is not None
            and value < parameter.minimum
        ):
            state.status_message = (
                f"{parameter_label} must be at least {parameter.minimum}"
            )
            return False
        if (
            isinstance(value, (int, float))
            and not isinstance(value, bool)
            and parameter.maximum is not None
            and value > parameter.maximum
        ):
            state.status_message = (
                f"{parameter_label} must be at most {parameter.maximum}"
            )
            return False
        parameters = dict(component.parameters)
        parameters[parameter.id] = value
        try:
            state.document.project.update_parameters(component.id, parameters)
        except ValueError as error:
            state.status_message = (
                f"Invalid value for {parameter_label}: "
                f"{display_text(error)}"
            )
            return False
        state.diagnostics.clear()
        state.status_message = f"Updated {parameter_label}"
        self._context_key = None
        return True

    def _commit_text(
        self,
        parameter: ParameterDefinition,
        text: str,
        state: WorkbenchState,
    ) -> bool:
        try:
            value = self._parse_text(parameter, text)
        except ValueError:
            state.status_message = (
                f"Invalid value for {display_text(parameter.id)}"
            )
            return False
        return self._set_parameter(parameter, value, state)

    def _activate_choice(
        self, editor: _ParameterEditor, state: WorkbenchState
    ) -> bool:
        parameter = editor.definition
        if parameter.read_only:
            return True
        component = state.document.project.components.get(
            state.selected_component_id
        )
        if component is None:
            return False
        current = component.parameters.get(parameter.id)
        if parameter.value_type is ParameterType.BOOLEAN:
            return self._set_parameter(parameter, not bool(current), state)
        if parameter.choices:
            try:
                index = parameter.choices.index(current)
            except ValueError:
                index = -1
            value = parameter.choices[(index + 1) % len(parameter.choices)]
            return self._set_parameter(parameter, value, state)
        return False

    def _content_rect(self) -> pygame.Rect:
        return pygame.Rect(
            self.rect.x,
            self.rect.y + self.HEADER_HEIGHT,
            self.rect.width,
            max(0, self.rect.height - self.HEADER_HEIGHT),
        )

    def _max_scroll(self) -> int:
        return max(0, self._content_height - self._content_rect().height)

    def _layout_editors(self) -> None:
        width = max(0, self.rect.width - self.PADDING * 2)
        content_top = self.rect.y + self.HEADER_HEIGHT
        for block in self._blocks:
            if block.editor is None:
                continue
            editor = block.editor
            editor.rect = pygame.Rect(
                self.rect.x + self.PADDING,
                content_top + block.y + 21 - self._scroll,
                width,
                30,
            )
            if editor.text_input is not None:
                editor.text_input.rect = editor.rect.copy()

    def _scroll_thumb_rect(self) -> pygame.Rect | None:
        content_rect = self._content_rect()
        maximum = self._max_scroll()
        if (
            maximum <= 0
            or content_rect.height <= 12
            or content_rect.width <= 0
        ):
            return None
        track_height = content_rect.height - 12
        thumb_height = max(
            28,
            round(track_height * content_rect.height / self._content_height),
        )
        thumb_height = min(track_height, thumb_height)
        travel = max(0, track_height - thumb_height)
        thumb_y = content_rect.y + 6 + round(travel * self._scroll / maximum)
        thumb_width = min(3, content_rect.width)
        return pygame.Rect(
            max(content_rect.x, content_rect.right - thumb_width - 2),
            thumb_y,
            thumb_width,
            thumb_height,
        )

    def handle_event(
        self, event: pygame.event.Event, state: WorkbenchState
    ) -> bool:
        self._ensure_context(state)
        if event.type == pygame.MOUSEWHEEL and self.rect.collidepoint(
            pygame.mouse.get_pos()
        ):
            self._scroll = max(
                0, min(self._max_scroll(), self._scroll - event.y * 30)
            )
            self._layout_editors()
            return True
        focused = next(
            (
                editor.text_input
                for editor in self._editors
                if editor.text_input is not None and editor.text_input.focused
            ),
            None,
        )
        if focused is not None and focused.handle_event(event):
            return True
        if event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
            if not self.rect.collidepoint(event.pos):
                return False
            if (
                not self._content_rect()
                .clip(self.rect)
                .collidepoint(event.pos)
            ):
                return True
            for editor in self._editors:
                if not editor.rect.collidepoint(event.pos):
                    continue
                if editor.text_input is not None:
                    for other in self._editors:
                        if (
                            other.text_input is not None
                            and other.text_input is not editor.text_input
                            and other.text_input.focused
                        ):
                            other.text_input.commit()
                    editor.text_input.focus()
                    return True
                return self._activate_choice(editor, state)
            return True
        return False

    @staticmethod
    def _wrapped_lines(
        value: object,
        font: pygame.font.Font,
        width: int,
        maximum: int = 2,
    ) -> tuple[str, ...]:
        text = display_text(value).strip()
        if not text or width <= 0:
            return ("",)
        words = text.split()
        lines: list[str] = []
        current = ""
        for word in words:
            candidate = f"{current} {word}".strip()
            if current and font.size(candidate)[0] > width:
                lines.append(current)
                current = word
            else:
                current = candidate
            if len(lines) == maximum:
                break
        if len(lines) < maximum and current:
            lines.append(current)
        if len(lines) > maximum:
            lines = lines[:maximum]
        consumed = " ".join(lines)
        if consumed != text and lines:
            lines[-1] += "…"
        return tuple(ellipsize(line, font, width) for line in lines)

    def _draw_editor(
        self,
        surface: pygame.Surface,
        editor: _ParameterEditor,
        state: WorkbenchState,
    ) -> None:
        content_rect = self._content_rect()
        if not editor.rect.colliderect(content_rect):
            return
        parameter = editor.definition
        draw_text(
            surface,
            display_text(editor.label),
            (editor.rect.x, editor.rect.y - 18),
            color=self.theme.muted_text,
            size=12,
        )
        if editor.text_input is not None:
            editor.text_input.draw(surface, self.theme)
        else:
            component = state.document.project.components.get(
                state.selected_component_id
            )
            value = (
                component.parameters.get(parameter.id) if component else None
            )
            pygame.draw.rect(
                surface, self.theme.button, editor.rect, border_radius=4
            )
            pygame.draw.rect(
                surface,
                self.theme.panel_border,
                editor.rect,
                width=1,
                border_radius=4,
            )
            suffix = " (fixed)" if parameter.read_only else ""
            draw_text(
                surface,
                ellipsize(
                    display_text(f"{value}{suffix}"),
                    get_font(14),
                    max(0, editor.rect.width - 14),
                ),
                (editor.rect.x + 7, editor.rect.centery),
                color=(
                    self.theme.muted_text
                    if parameter.read_only
                    else self.theme.text
                ),
                size=14,
                anchor="midleft",
            )
        if editor.diagnostics:
            diagnostic_color = {
                DiagnosticSeverity.ERROR: (238, 112, 112),
                DiagnosticSeverity.WARNING: (232, 184, 92),
                DiagnosticSeverity.INFO: self.theme.accent,
            }.get(editor.diagnostic_severity, (238, 112, 112))
            draw_text(
                surface,
                ellipsize(
                    display_text(editor.diagnostics[0]),
                    get_font(11),
                    max(0, editor.rect.width),
                ),
                (editor.rect.x, editor.rect.bottom + 4),
                color=diagnostic_color,
                size=11,
            )

    def _draw_block(
        self,
        surface: pygame.Surface,
        block: _ContentBlock,
        state: WorkbenchState,
    ) -> None:
        if block.kind == "editor":
            if block.editor is not None:
                self._draw_editor(surface, block.editor, state)
            return
        content_rect = self._content_rect()
        y = content_rect.y + block.y - self._scroll
        if y + block.height < content_rect.top or y > content_rect.bottom:
            return
        x = self.rect.x + self.PADDING
        width = max(0, self.rect.width - self.PADDING * 2)
        if block.kind == "section":
            draw_text(
                surface,
                display_text(block.label),
                (x, y + 8),
                color=self.theme.text,
                size=13,
                bold=True,
            )
            pygame.draw.line(
                surface,
                self.theme.panel_border,
                (x, y + block.height - 3),
                (self.rect.right - self.PADDING, y + block.height - 3),
            )
            return
        if block.kind in ("row", "port"):
            draw_text(
                surface,
                ellipsize(display_text(block.label), get_font(11), width),
                (x, y + 2),
                color=self.theme.muted_text,
                size=11,
            )
            color = self.theme.text
            if block.kind == "port" and str(block.value).startswith(
                "Not connected"
            ):
                color = self.theme.muted_text
            draw_text(
                surface,
                ellipsize(display_text(block.value), get_font(13), width),
                (x, y + 20),
                color=color,
                size=13,
            )
            return
        if block.kind in ("paragraph", "diagnostic"):
            color = self.theme.muted_text
            offset = 3
            if block.kind == "diagnostic":
                color = {
                    DiagnosticSeverity.ERROR: (238, 112, 112),
                    DiagnosticSeverity.WARNING: (232, 184, 92),
                    DiagnosticSeverity.INFO: self.theme.accent,
                }.get(block.severity, self.theme.muted_text)
                draw_text(
                    surface,
                    display_text(block.label),
                    (x, y + 1),
                    color=color,
                    size=11,
                    bold=True,
                )
                offset = 18
            font = get_font(12)
            for index, line in enumerate(
                self._wrapped_lines(block.value, font, width, 2)
            ):
                draw_text(
                    surface,
                    line,
                    (x, y + offset + index * 17),
                    color=(
                        self.theme.text
                        if block.kind == "diagnostic"
                        else color
                    ),
                    size=12,
                )

    def draw(self, surface: pygame.Surface, state: WorkbenchState) -> None:
        self._ensure_context(state)
        draw_panel(surface, self.rect, self.theme)
        if self.rect.width < 1 or self.rect.height < 1:
            return
        previous_clip = surface.get_clip()
        surface.set_clip(self.rect)
        try:
            draw_text(
                surface,
                "Inspector",
                (self.rect.x + self.PADDING, self.rect.y + 15),
                color=self.theme.text,
                size=18,
                bold=True,
            )
            draw_text(
                surface,
                self.mode.value,
                (self.rect.right - self.PADDING, self.rect.y + 23),
                color=self.theme.muted_text,
                size=11,
                anchor="midright",
            )
            content_rect = self._content_rect()
            surface.set_clip(content_rect.clip(self.rect))
            for block in self._blocks:
                self._draw_block(surface, block, state)
            scroll_thumb = self._scroll_thumb_rect()
            if scroll_thumb is not None:
                pygame.draw.rect(
                    surface,
                    self.theme.accent,
                    scroll_thumb,
                    border_radius=2,
                )
        finally:
            surface.set_clip(previous_clip)


__all__ = ["Inspector", "InspectorMode"]
