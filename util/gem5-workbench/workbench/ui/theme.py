from dataclasses import dataclass

Color = tuple[int, int, int]


@dataclass(frozen=True, slots=True)
class Theme:
    window_background: Color = (24, 26, 32)
    panel_background: Color = (34, 37, 45)
    panel_border: Color = (62, 67, 80)

    canvas_background: Color = (22, 24, 29)
    grid: Color = (40, 43, 51)

    text: Color = (235, 237, 242)
    muted_text: Color = (155, 161, 176)

    accent: Color = (91, 141, 239)
    accent_hover: Color = (112, 158, 245)

    button: Color = (52, 57, 69)
    button_hover: Color = (67, 73, 88)

    node_background: Color = (47, 74, 113)
    node_border: Color = (112, 158, 245)
