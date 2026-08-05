"""UI-only schemas describing how component properties are grouped.

Registry definitions describe persisted values and validation.  These
descriptors only describe presentation, so neither the project model nor the
registry needs to know about Inspector sections, labels, or visibility rules.
"""

from dataclasses import dataclass
from typing import Mapping

from workbench.model.values import FrozenJsonValue
from workbench.registry import (
    DDR3_MEMORY_TYPE_ID,
    NO_CACHE_TYPE_ID,
    SE_WORKLOAD_TYPE_ID,
    SIMPLE_BOARD_TYPE_ID,
    SIMPLE_PROCESSOR_TYPE_ID,
    ComponentDefinition,
)


@dataclass(frozen=True, slots=True)
class FieldVisibility:
    """Show a property field when another parameter has the given value."""

    parameter_id: str
    equals: FrozenJsonValue

    def matches(self, parameters: Mapping[str, FrozenJsonValue]) -> bool:
        return parameters.get(self.parameter_id) == self.equals


@dataclass(frozen=True, slots=True)
class PropertyFieldDescriptor:
    parameter_id: str
    label: str | None = None
    description: str = ""
    visible_when: tuple[FieldVisibility, ...] = ()

    def is_visible(self, parameters: Mapping[str, FrozenJsonValue]) -> bool:
        return all(rule.matches(parameters) for rule in self.visible_when)

    @property
    def display_label(self) -> str:
        return self.label or self.parameter_id.replace("_", " ").title()


@dataclass(frozen=True, slots=True)
class PropertySectionDescriptor:
    id: str
    title: str
    fields: tuple[PropertyFieldDescriptor, ...]


@dataclass(frozen=True, slots=True)
class ComponentInspectorDescriptor:
    """Presentation schema selected for a component definition."""

    type_id: str
    sections: tuple[PropertySectionDescriptor, ...]
    generated: bool = False


def _field(
    parameter_id: str,
    label: str | None = None,
    description: str = "",
    *,
    when: tuple[FieldVisibility, ...] = (),
) -> PropertyFieldDescriptor:
    return PropertyFieldDescriptor(parameter_id, label, description, when)


_CATEGORY_SECTIONS = {
    "Boards": (
        PropertySectionDescriptor(
            "board_timing",
            "Board timing",
            (
                _field(
                    "clk_freq",
                    "Clock frequency",
                    "Frequency used by the board clock domain.",
                ),
            ),
        ),
    ),
    "Processors": (
        PropertySectionDescriptor(
            "processor_architecture",
            "Architecture",
            (_field("isa", "ISA"),),
        ),
    ),
    "Memory": (
        PropertySectionDescriptor(
            "memory_capacity",
            "Memory capacity",
            (_field("size", "Size"),),
        ),
    ),
    "Workloads": (
        PropertySectionDescriptor(
            "workload_source",
            "Workload source",
            (_field("source_kind", "Source kind"),),
        ),
    ),
}

_CATEGORY_FALLBACK_TITLES = {
    "boards": "Board properties",
    "processors": "Processor properties",
    "memory": "Memory properties",
    "cache hierarchies": "Cache properties",
    "workloads": "Workload properties",
}


_CURATED_DESCRIPTORS = (
    ComponentInspectorDescriptor(SIMPLE_BOARD_TYPE_ID, ()),
    ComponentInspectorDescriptor(
        SIMPLE_PROCESSOR_TYPE_ID,
        (
            PropertySectionDescriptor(
                "cpu_model",
                "CPU model",
                (_field("cpu_type", "CPU type"),),
            ),
            PropertySectionDescriptor(
                "topology",
                "Topology",
                (_field("num_cores", "Core count"),),
            ),
        ),
    ),
    ComponentInspectorDescriptor(DDR3_MEMORY_TYPE_ID, ()),
    ComponentInspectorDescriptor(NO_CACHE_TYPE_ID, ()),
    ComponentInspectorDescriptor(
        SE_WORKLOAD_TYPE_ID,
        (
            PropertySectionDescriptor(
                "local_binary",
                "Local binary",
                (
                    _field(
                        "local_path",
                        "Binary path",
                        when=(FieldVisibility("source_kind", "local"),),
                    ),
                    _field(
                        "path_base",
                        "Resolve relative to",
                        when=(FieldVisibility("source_kind", "local"),),
                    ),
                ),
            ),
            PropertySectionDescriptor(
                "resource",
                "gem5 resource",
                (
                    _field(
                        "resource_id",
                        "Resource ID",
                        when=(FieldVisibility("source_kind", "resource"),),
                    ),
                    _field(
                        "resource_version",
                        "Resource version",
                        when=(FieldVisibility("source_kind", "resource"),),
                    ),
                ),
            ),
        ),
    ),
)


class InspectorDescriptorProvider:
    """Resolve curated schemas and generate a safe fallback for new types."""

    def __init__(
        self,
        descriptors: tuple[ComponentInspectorDescriptor, ...] = (
            _CURATED_DESCRIPTORS
        ),
        category_sections: Mapping[
            str, tuple[PropertySectionDescriptor, ...]
        ] = _CATEGORY_SECTIONS,
    ) -> None:
        self._descriptors = {
            descriptor.type_id: descriptor for descriptor in descriptors
        }
        self._category_sections = {
            category.casefold(): sections
            for category, sections in category_sections.items()
        }

    def descriptor_for(
        self, definition: ComponentDefinition
    ) -> ComponentInspectorDescriptor:
        curated = self._descriptors.get(definition.type_id)
        known_parameters = set(definition.parameters)
        sections: list[PropertySectionDescriptor] = []
        claimed: set[str] = set()
        category_key = definition.category.casefold()
        layers = [self._category_sections.get(category_key, ())]
        if curated is not None:
            layers.append(curated.sections)
        for section in (section for layer in layers for section in layer):
            fields = tuple(
                field
                for field in section.fields
                if field.parameter_id in known_parameters
                and field.parameter_id not in claimed
            )
            if fields:
                sections.append(
                    PropertySectionDescriptor(
                        section.id, section.title, fields
                    )
                )
                claimed.update(field.parameter_id for field in fields)

        additional = tuple(
            _field(parameter_id)
            for parameter_id in definition.parameters
            if parameter_id not in claimed
        )
        if additional:
            title = "Additional properties"
            if curated is None and not claimed:
                title = _CATEGORY_FALLBACK_TITLES.get(
                    category_key, "Properties"
                )
            sections.append(
                PropertySectionDescriptor("additional", title, additional)
            )
        return ComponentInspectorDescriptor(
            definition.type_id,
            tuple(sections),
            generated=curated is None,
        )


DEFAULT_INSPECTOR_DESCRIPTORS = InspectorDescriptorProvider()


__all__ = [
    "ComponentInspectorDescriptor",
    "DEFAULT_INSPECTOR_DESCRIPTORS",
    "FieldVisibility",
    "InspectorDescriptorProvider",
    "PropertyFieldDescriptor",
    "PropertySectionDescriptor",
]
