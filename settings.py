"""Глобальные константы игры «ПРОМПТ»: экран, цвета, скорости, Помощники, босс.

Модуль не содержит логики — только данные, которые читают остальные модули.
"""
from __future__ import annotations

Color = tuple[int, int, int]
Tile = tuple[int, int]


def hex_to_rgb(value: str) -> Color:
    """Переводит строку вида ``#rrggbb`` в кортеж RGB."""
    value = value.lstrip("#")
    return int(value[0:2], 16), int(value[2:4], 16), int(value[4:6], 16)


# --------------------------------------------------------------------------- #
# Экран
# --------------------------------------------------------------------------- #
TITLE = "ПРОМПТ"
SCREEN_WIDTH = 1280
SCREEN_HEIGHT = 720
SCREEN_CENTER = (SCREEN_WIDTH // 2, SCREEN_HEIGHT // 2)
FPS = 60
MAX_FRAME_TIME = 1 / 30  # защита от «телепортов» при подвисании окна
TILE = 48

# Псевдо-3D: насколько «верх» блока смещается от центра экрана (доля расстояния).
WALL_HEIGHT = 0.075
GATE_HEIGHT = 0.075
GLASS_HEIGHT = 0.045
DEVICE_HEIGHT = 0.035  # терминалы, консоли, панели, замки
BOX_HEIGHT = 0.04

# --------------------------------------------------------------------------- #
# Палитра (минималистичный неон)
# --------------------------------------------------------------------------- #
BG_COLOR = hex_to_rgb("#0d0d0d")
WALL_COLOR = hex_to_rgb("#1a1a2e")
WALL_SIDE_LIGHT = (24, 24, 44)
WALL_SIDE_DARK = (15, 15, 28)
WALL_EDGE_COLOR = (88, 96, 255)
FLOOR_COLOR = (17, 17, 24)
FLOOR_ALT_COLOR = (19, 19, 28)
GRID_COLOR = (30, 32, 52)
FIELD_COLOR = (24, 14, 22)
FIELD_GRID_COLOR = (60, 26, 48)
PLAYER_COLOR = hex_to_rgb("#4d6bfe")
TEXT_COLOR = (225, 230, 245)
DIM_TEXT_COLOR = (120, 126, 158)
ACCENT_COLOR = (0, 255, 200)
SYSTEM_COLOR = (140, 160, 255)
DANGER_COLOR = (255, 60, 90)
SUCCESS_COLOR = (60, 255, 140)
WARNING_COLOR = (255, 205, 60)
GLASS_COLOR = (90, 200, 255)
TERMINAL_COLOR = (0, 230, 255)
BOX_COLOR = (255, 150, 60)
PANEL_BG = (10, 10, 22)

# --------------------------------------------------------------------------- #
# Головоломки
# --------------------------------------------------------------------------- #
PUSH_BOX = "push_box"
BUTTON_SEQUENCE = "button_sequence"
ROUTE_MEMORY = "route_memory"
LINK_OBJECTS = "link_objects"
CONTEXT_TASK = "context_task"

PUZZLE_TITLES: dict[str, str] = {
    PUSH_BOX: "Толкни ящик",
    BUTTON_SEQUENCE: "Последовательность кнопок",
    ROUTE_MEMORY: "Память маршрута",
    LINK_OBJECTS: "Связь объектов",
    CONTEXT_TASK: "Контекстная задача",
}

# Минимальный уровень интеллекта Помощника для каждого типа головоломки.
PUZZLE_MIN_LEVEL: dict[str, int] = {
    PUSH_BOX: 1,
    BUTTON_SEQUENCE: 1,
    ROUTE_MEMORY: 2,
    LINK_OBJECTS: 3,
    CONTEXT_TASK: 4,
}

# --------------------------------------------------------------------------- #
# Помощники
# --------------------------------------------------------------------------- #
ASSISTANT_NAMES: dict[int, str] = {
    1: "Пиксель",
    2: "Эхо",
    3: "Вектор",
    4: "Трансформер",
    5: "Оракул",
}
ASSISTANT_COLORS: dict[int, Color] = {
    1: hex_to_rgb("#00ff88"),
    2: hex_to_rgb("#ffaa00"),
    3: hex_to_rgb("#ff00aa"),
    4: hex_to_rgb("#00aaff"),
    5: hex_to_rgb("#ffffff"),
}
ASSISTANT_SHAPES: dict[int, str] = {
    1: "квадрат",
    2: "круг",
    3: "треугольник",
    4: "шестиугольник",
    5: "звезда",
}
ASSISTANT_DESCRIPTIONS: dict[int, str] = {
    1: "Простые действия: толкает ящики и нажимает кнопки.",
    2: "Запоминает маршрут, который ты ему показал.",
    3: "Связывает объекты, даже если они в разных комнатах.",
    4: "Понимает контекст задачи, сформулированной через меню.",
    5: "Решает всё. Но иногда считает задачу плохо сформулированной.",
}
ASSISTANT_ACKS: dict[int, str] = {
    1: "Принято! Толкаю, жму, бегу.",
    2: "Повторяю... повторяю... повторяю.",
    3: "Строю вектор связи.",
    4: "Контекст ясен. Приступаю.",
    5: "Я знал, что ты попросишь именно это.",
}
ASSISTANT_REFUSALS: dict[int, str] = {
    1: "Не понимаю. Я умею только толкать и нажимать.",
    2: "Я могу лишь повторять то, что видел.",
    3: "Вижу объекты, но не вижу смысла.",
    4: "Эта задача мне не по силам.",
    5: "Даже Оракул не всесилен.",
}
ASSISTANT_SKILLS: dict[int, frozenset[str]] = {
    level: frozenset(kind for kind, need in PUZZLE_MIN_LEVEL.items() if need <= level)
    for level in range(1, 6)
}
ORACLE_REFUSAL_CHANCE = 0.2
ASSISTANT_COUNT = 5

# --------------------------------------------------------------------------- #
# Движение и взаимодействие
# --------------------------------------------------------------------------- #
PLAYER_SPEED = 230.0
PLAYER_SIZE = 26
ASSISTANT_SPEED = 265.0
ASSISTANT_TASK_SPEED = 210.0
PUSH_SPEED = 150.0
INTERACT_RANGE = TILE * 1.35
KEY_INTERACT_RANGE = TILE * 2.4  # ключи стоят за стеклом — их отмечают издалека
FOLLOW_DISTANCE = TILE * 1.3
REPATH_INTERVAL = 0.35
ASSISTANT_CATCHUP_DISTANCE = TILE * 11
ASSISTANT_CATCHUP_TIME = 2.5
RESPAWN_INVULNERABILITY = 1.4

# --------------------------------------------------------------------------- #
# Стелс и опасности
# --------------------------------------------------------------------------- #
SENTINEL_SPEED = 85.0
SENTINEL_TURN_SPEED = 3.2
SENTINEL_VIEW_RANGE = TILE * 4.6
SENTINEL_FOV_DEG = 64.0
SENTINEL_PAUSE = 1.1
SENTINEL_COLOR = (255, 215, 70)
DETECTION_TIME = 0.5
MINE_REVEAL_RADIUS = TILE * 1.15
SHADOW_RADIUS = TILE * 2.8
WIND_SPEED = 330.0

# --------------------------------------------------------------------------- #
# Босс «Вайбкодер»
# --------------------------------------------------------------------------- #
BOSS_MAX_HP = 3
BOSS_RADIUS = 66
CONTROL_SWAP_DURATION = 5.0
CONTROL_SWAP_INTERVAL = 7.0
FLIP_ON_DURATION = 6.0
FLIP_OFF_DURATION = 3.5
CORRUPTION_INTERVAL = 7.0
BUG_SPEED = 62.0
BUG_COUNT_BY_PHASE = {1: 2, 2: 1, 3: 3}
BOSS_PHASE_NAMES = {
    1: "ФАЗА 1 · ПУТАНИЦА УПРАВЛЕНИЯ",
    2: "ФАЗА 2 · ПЕРЕВОРОТ МИРА",
    3: "ФАЗА 3 · ПОДМЕНА ПОМОЩНИКОВ",
}

# --------------------------------------------------------------------------- #
# Уровни и интерфейс
# --------------------------------------------------------------------------- #
LEVEL_COUNT = 5
FONT_CANDIDATES = (
    "consolas",
    "dejavusansmono",
    "couriernew",
    "lucidaconsole",
    "liberationmono",
    "ubuntumono",
    "menlo",
    "arial",
)
CONTROLS_HINT = (
    "WASD — движение",
    "E — взаимодействие",
    "1-5 — выбор Помощника",
    "ESC — пауза",
)
MESSAGE_DURATION = 5.5
