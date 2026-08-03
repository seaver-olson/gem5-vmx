"""Headless tests for the initial gem5 Workbench UI frame."""

import os
import unittest

os.environ.setdefault("SDL_AUDIODRIVER", "dummy")
os.environ.setdefault("SDL_VIDEODRIVER", "dummy")

import pygame

from workbench.app import WorkbenchApp
from workbench.state import CanvasNode, WorkbenchState
from workbench.ui.canvas import Canvas
from workbench.ui.layout import create_layout
from workbench.ui.palette import Palette
from workbench.ui.theme import Theme


class LayoutTests(unittest.TestCase):
    def test_regions_are_contiguous(self) -> None:
        for size in ((1400, 850), (1000, 650), (200, 100), (0, 0)):
            with self.subTest(size=size):
                layout = create_layout(size)
                self.assertEqual(layout.palette.right, layout.canvas.left)
                self.assertEqual(layout.canvas.right, layout.inspector.left)
                self.assertEqual(layout.inspector.right, max(0, size[0]))
                self.assertEqual(layout.status_bar.bottom, max(0, size[1]))


class InteractionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        pygame.init()
        pygame.display.set_mode((640, 480))

    @classmethod
    def tearDownClass(cls) -> None:
        pygame.quit()

    def setUp(self) -> None:
        self.theme = Theme()
        self.state = WorkbenchState()
        self.palette = Palette(pygame.Rect(0, 0, 230, 400), self.theme)
        self.canvas = Canvas(pygame.Rect(230, 0, 410, 400), self.theme)

    def test_click_to_place_and_delete(self) -> None:
        self.palette.handle_event(
            pygame.event.Event(
                pygame.MOUSEBUTTONDOWN, button=1, pos=(20, 70)
            ),
            self.state,
        )
        self.assertEqual(self.state.selected_component, "board")
        self.assertEqual(
            self.state.status_message,
            "Board selected — click the canvas to place it",
        )
        self.canvas.handle_event(
            pygame.event.Event(
                pygame.MOUSEBUTTONDOWN, button=1, pos=(400, 200)
            ),
            self.state,
        )
        self.assertEqual(len(self.state.nodes), 1)
        self.assertEqual(self.state.selected_node_id, self.state.nodes[0].id)
        self.canvas.handle_event(
            pygame.event.Event(pygame.KEYDOWN, key=pygame.K_DELETE), self.state
        )
        self.assertEqual(self.state.nodes, [])

    def test_selection_survives_reordering(self) -> None:
        first = CanvasNode("board", pygame.Vector2(10, 10))
        second = CanvasNode("memory", pygame.Vector2(200, 200))
        self.state.nodes = [first, second]
        self.state.selected_node_id = first.id
        self.state.nodes.reverse()
        self.canvas.handle_event(
            pygame.event.Event(pygame.KEYDOWN, key=pygame.K_DELETE), self.state
        )
        self.assertEqual(self.state.nodes, [second])

    def test_palette_drag_does_not_place_node(self) -> None:
        self.palette.handle_event(
            pygame.event.Event(
                pygame.MOUSEBUTTONDOWN, button=1, pos=(20, 128)
            ),
            self.state,
        )
        consumed = self.canvas.handle_event(
            pygame.event.Event(
                pygame.MOUSEBUTTONUP, button=1, pos=(400, 200)
            ),
            self.state,
        )
        self.assertFalse(consumed)
        self.assertEqual(self.state.nodes, [])

    def test_constrain_nodes_after_resize(self) -> None:
        self.state.nodes.append(CanvasNode("memory", pygame.Vector2(900, 900)))
        self.canvas.constrain_nodes(self.state)
        node = self.state.nodes[0]
        self.assertEqual(node.position, (260, 336))


class ProjectTests(unittest.TestCase):
    def test_project_validation(self) -> None:
        document = {
            "format": "gem5-workbench",
            "version": 1,
            "nodes": [
                {"id": "cpu-1", "kind": "processor", "position": [10, 20]}
            ],
        }
        nodes = WorkbenchApp._nodes_from_document(document)
        self.assertEqual(nodes[0].id, "cpu-1")
        self.assertEqual(nodes[0].kind, "processor")
        self.assertEqual(nodes[0].position, (10, 20))

    def test_duplicate_node_ids_are_rejected(self) -> None:
        document = {
            "format": "gem5-workbench",
            "version": 1,
            "nodes": [
                {"id": "same", "kind": "board", "position": [0, 0]},
                {"id": "same", "kind": "memory", "position": [20, 20]},
            ],
        }
        with self.assertRaises(ValueError):
            WorkbenchApp._nodes_from_document(document)

    def test_non_finite_position_is_rejected(self) -> None:
        document = {
            "format": "gem5-workbench",
            "version": 1,
            "nodes": [{"kind": "memory", "position": [float("nan"), 0]}],
        }
        with self.assertRaises(ValueError):
            WorkbenchApp._nodes_from_document(document)

    def test_unrepresentably_large_position_is_rejected(self) -> None:
        document = {
            "format": "gem5-workbench",
            "version": 1,
            "nodes": [{"kind": "memory", "position": [10**1000, 0]}],
        }
        with self.assertRaises(ValueError):
            WorkbenchApp._nodes_from_document(document)


if __name__ == "__main__":
    unittest.main()
