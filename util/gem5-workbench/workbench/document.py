"""Top-level aggregate joining simulation intent and editor layout."""

from dataclasses import (
    dataclass,
    field,
)
from typing import Mapping

from workbench.layout import (
    ComponentLayout,
    Position,
    ProjectLayout,
)
from workbench.model import (
    Component,
    ComponentId,
    Project,
    ProjectMutationError,
)
from workbench.model.values import JsonValue

FORMAT_ID = "gem5-workbench"
SCHEMA_VERSION = 1


@dataclass(slots=True)
class ProjectDocument:
    project: Project = field(default_factory=Project)
    layout: ProjectLayout = field(default_factory=ProjectLayout)

    def add_component(
        self,
        type_id: str,
        position: Position,
        *,
        component_id: ComponentId | None = None,
        parameters: dict[str, JsonValue] | None = None,
    ) -> Component:
        try:
            if parameters is not None and not isinstance(parameters, Mapping):
                raise ValueError("component parameters must be a mapping")
            component = Component(
                type_id=type_id,
                **({"id": component_id} if component_id is not None else {}),
                parameters=(
                    dict(parameters) if parameters is not None else {}
                ),
            )
            component_layout = ComponentLayout(position=position)
        except ValueError as error:
            raise ProjectMutationError(str(error)) from error
        if component.id in self.layout.components:
            raise ProjectMutationError(
                f"layout already contains component id: {component.id}"
            )
        self.project.add_component(component)
        self.layout.set_component(component.id, component_layout)
        return component

    def remove_component(self, component_id: ComponentId) -> Component:
        component = self.project.remove_component(component_id)
        self.layout.remove_component(component_id)
        return component

    def repair_layout(self) -> list[ComponentId]:
        """Add deterministic layout entries for components which lack one."""

        return self.layout.repair_missing(self.project.components)
