"""Component definition registry."""

from workbench.registry.definitions import ComponentDefinition


class ComponentRegistry:
    def __init__(self) -> None:
        self._definitions: dict[str, ComponentDefinition] = {}

    def register(self, definition: ComponentDefinition) -> None:
        if definition.type_id in self._definitions:
            raise ValueError(f"duplicate component type: {definition.type_id}")
        self._definitions[definition.type_id] = definition

    def get(self, type_id: str) -> ComponentDefinition | None:
        return self._definitions.get(type_id)

    def definitions(self) -> tuple[ComponentDefinition, ...]:
        return tuple(self._definitions.values())
