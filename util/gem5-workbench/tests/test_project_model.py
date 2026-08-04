"""Tests for the domain, persistence, registry, and validation layers."""

import unittest

from workbench.document import ProjectDocument
from workbench.layout import Position
from workbench.model import Component, Connection, Endpoint
from workbench.model.identifiers import ComponentId, ConnectionId
from workbench.model.project import ProjectMutationError
from workbench.persistence import (
    ProjectFileError,
    document_from_data,
    document_to_data,
    dumps_project_document,
    loads_project_document,
)
from workbench.registry import (
    ComponentDefinition,
    ComponentRegistry,
    PortDefinition,
    PortDirection,
)
from workbench.validation import (
    DiagnosticLayer,
    validate_document_shape,
    validate_readiness,
    validate_registry,
    validate_project,
    validate_structure,
)


class MutationTests(unittest.TestCase):
    def test_duplicate_component_id_is_rejected(self) -> None:
        document = ProjectDocument()
        component_id = ComponentId("same")
        document.project.add_component(Component("board", component_id))
        with self.assertRaises(ProjectMutationError):
            document.project.add_component(Component("memory", component_id))

    def test_remove_component_cascades_connections_and_layout(self) -> None:
        document = ProjectDocument()
        source = document.add_component("source", Position(0, 0))
        target = document.add_component("target", Position(100, 0))
        connection = Connection(
            Endpoint(source.id, "out"), Endpoint(target.id, "in")
        )
        document.project.add_connection(connection)
        document.remove_component(source.id)
        self.assertEqual(document.project.connections, {})
        self.assertNotIn(source.id, document.layout.components)
        self.assertIn(target.id, document.layout.components)

    def test_connection_requires_existing_components(self) -> None:
        document = ProjectDocument()
        component = document.add_component("source", Position())
        connection = Connection(
            Endpoint(component.id, "out"),
            Endpoint(ComponentId("missing"), "in"),
        )
        with self.assertRaises(ProjectMutationError):
            document.project.add_connection(connection)

    def test_mutation_rejects_non_json_parameters(self) -> None:
        document = ProjectDocument()
        with self.assertRaises(ProjectMutationError):
            document.add_component(
                "board", Position(), parameters={"bad": object()}
            )


class PersistenceTests(unittest.TestCase):
    def test_unknown_component_type_survives_round_trip(self) -> None:
        document = ProjectDocument()
        component = document.add_component(
            "plugin.future_component",
            Position(12.5, 30),
            parameters={"future_setting": [1, "two", True]},
        )
        loaded = loads_project_document(dumps_project_document(document))
        loaded_component = loaded.project.components[component.id]
        self.assertEqual(loaded_component.type_id, "plugin.future_component")
        self.assertEqual(loaded_component.parameters, component.parameters)

    def test_duplicate_component_ids_in_document_are_invalid(self) -> None:
        data = self._empty_data()
        component = {
            "id": "same",
            "type_id": "board",
            "parameters": {},
        }
        data["project"]["components"] = [component, dict(component)]
        with self.assertRaisesRegex(ProjectFileError, "duplicate component id"):
            document_from_data(data)

    def test_duplicate_connection_ids_in_document_are_invalid(self) -> None:
        data = self._empty_data()
        data["project"]["components"] = [
            {"id": "a", "type_id": "a", "parameters": {}},
            {"id": "b", "type_id": "b", "parameters": {}},
        ]
        connection = {
            "id": "same",
            "source": {"component_id": "a", "port_id": "out", "slot": None},
            "target": {"component_id": "b", "port_id": "in", "slot": None},
            "parameters": {},
        }
        data["project"]["connections"] = [connection, dict(connection)]
        with self.assertRaisesRegex(ProjectFileError, "duplicate connection id"):
            document_from_data(data)

    def test_legacy_flat_document_is_migrated(self) -> None:
        legacy = {
            "format": "gem5-workbench",
            "version": 1,
            "nodes": [
                {"id": "cpu", "kind": "processor", "position": [10, 20]}
            ],
        }
        document = document_from_data(legacy)
        component = document.project.components[ComponentId("cpu")]
        self.assertEqual(component.type_id, "processor")
        position = document.layout.components[component.id].position
        self.assertEqual((position.x, position.y), (10, 20))

    def test_envelope_uses_separate_project_and_layout_sections(self) -> None:
        data = document_to_data(ProjectDocument())
        self.assertEqual(set(data), {"format", "schema_version", "project", "layout"})

    @staticmethod
    def _empty_data():
        return {
            "format": "gem5-workbench",
            "schema_version": 1,
            "project": {"components": [], "connections": [], "metadata": {}},
            "layout": {
                "components": {},
                "viewport": {"offset_x": 0, "offset_y": 0, "zoom": 1},
                "metadata": {},
            },
        }


class ValidationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.registry = ComponentRegistry()
        self.registry.register(
            ComponentDefinition(
                "producer",
                "Producer",
                "Test",
                ports={
                    "out": PortDefinition(
                        "out", PortDirection.OUTPUT, "test.bus"
                    )
                },
            )
        )
        self.registry.register(
            ComponentDefinition(
                "consumer",
                "Consumer",
                "Test",
                ports={
                    "in": PortDefinition(
                        "in", PortDirection.INPUT, "test.bus", required=True
                    )
                },
            )
        )

    def test_unknown_type_is_registry_error_not_load_error(self) -> None:
        document = ProjectDocument()
        component = document.add_component("unknown", Position())
        diagnostics = validate_registry(document, self.registry)
        self.assertEqual(diagnostics[0].code, "registry.unknown_component_type")
        self.assertEqual(diagnostics[0].component_id, component.id)
        self.assertEqual(diagnostics[0].layer, DiagnosticLayer.REGISTRY)

    def test_ports_are_validated_through_registry(self) -> None:
        document = ProjectDocument()
        producer = document.add_component("producer", Position())
        consumer = document.add_component("consumer", Position(100, 0))
        document.project.add_connection(
            Connection(
                Endpoint(producer.id, "out"), Endpoint(consumer.id, "in")
            )
        )
        self.assertEqual(validate_registry(document, self.registry), [])
        self.assertEqual(validate_readiness(document, self.registry), [])

    def test_required_port_is_readiness_layer(self) -> None:
        document = ProjectDocument()
        consumer = document.add_component("consumer", Position())
        diagnostics = validate_readiness(document, self.registry)
        self.assertEqual(diagnostics[0].component_id, consumer.id)
        self.assertEqual(diagnostics[0].layer, DiagnosticLayer.READINESS)

    def test_dangling_endpoint_is_structure_layer(self) -> None:
        document = ProjectDocument()
        component = document.add_component("producer", Position())
        connection = Connection(
            Endpoint(component.id, "out"),
            Endpoint(ComponentId("missing"), "in"),
            ConnectionId("dangling"),
        )
        document.project.connections[connection.id] = connection
        diagnostics = validate_structure(document)
        self.assertEqual(diagnostics[0].code, "structure.dangling_endpoint")
        self.assertEqual(diagnostics[0].layer, DiagnosticLayer.STRUCTURE)

    def test_document_shape_is_its_own_layer(self) -> None:
        diagnostics = validate_document_shape({"format": "wrong"})
        self.assertTrue(diagnostics)
        self.assertTrue(
            all(item.layer is DiagnosticLayer.DOCUMENT for item in diagnostics)
        )

    def test_pipeline_stops_before_readiness_after_registry_error(self) -> None:
        document = ProjectDocument()
        document.add_component("unknown", Position())
        document.add_component("consumer", Position(100, 0))
        diagnostics = validate_project(document, self.registry)
        self.assertTrue(
            any(item.layer is DiagnosticLayer.REGISTRY for item in diagnostics)
        )
        self.assertFalse(
            any(item.layer is DiagnosticLayer.READINESS for item in diagnostics)
        )


if __name__ == "__main__":
    unittest.main()
