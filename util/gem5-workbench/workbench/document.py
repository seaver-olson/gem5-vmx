"""Top-level aggregate joining simulation intent and editor layout."""

from dataclasses import dataclass, field

from workbench.layout import ComponentLayout, Position, ProjectLayout
from workbench.model import Component, ComponentId, Project
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
        component = Component(
            type_id=type_id,
            parameters=dict(parameters) if parameters is not None else {},
        )
        if component_id is not None:
            component.id = component_id
        self.project.add_component(component)
        self.layout.set_component(
            component.id, ComponentLayout(position=position)
        )
        return component

    def remove_component(self, component_id: ComponentId) -> Component:
        component = self.project.remove_component(component_id)
        self.layout.remove_component(component_id)
        return component
