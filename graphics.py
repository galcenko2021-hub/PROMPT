"""Неоновые примитивы, шрифты, частицы и псевдо-3D блоки.

Вся графика игры строится из ``pygame.draw`` (rect, circle, polygon, line).
Свечение — заранее отрисованные радиальные градиенты, которые
накладываются аддитивным смешиванием.
"""
from __future__ import annotations

import math
import random
from dataclasses import dataclass, field

import pygame
from pygame.math import Vector2

from settings import (
    FONT_CANDIDATES,
    SCREEN_CENTER,
    TEXT_COLOR,
    TILE,
    Color,
    Tile,
)

Point = tuple[float, float]


# --------------------------------------------------------------------------- #
# Геометрия сетки
# --------------------------------------------------------------------------- #
def tile_center(tile: Tile) -> Vector2:
    """Мировые координаты центра клетки."""
    return Vector2(tile[0] * TILE + TILE / 2, tile[1] * TILE + TILE / 2)


def tile_of(pos: Vector2 | Point) -> Tile:
    """Клетка, в которой находится мировая точка."""
    return int(pos[0] // TILE), int(pos[1] // TILE)


def tile_rect(tile: Tile) -> pygame.Rect:
    """Прямоугольник клетки в мировых координатах."""
    return pygame.Rect(tile[0] * TILE, tile[1] * TILE, TILE, TILE)


# --------------------------------------------------------------------------- #
# Цвета
# --------------------------------------------------------------------------- #
def clamp(value: float, low: float, high: float) -> float:
    """Ограничивает значение диапазоном."""
    return max(low, min(high, value))


def scale_color(color: Color, k: float) -> Color:
    """Умножает яркость цвета на ``k``."""
    return (
        int(clamp(color[0] * k, 0, 255)),
        int(clamp(color[1] * k, 0, 255)),
        int(clamp(color[2] * k, 0, 255)),
    )


def mix_color(a: Color, b: Color, t: float) -> Color:
    """Линейная интерполяция между двумя цветами."""
    t = clamp(t, 0.0, 1.0)
    return (
        int(a[0] + (b[0] - a[0]) * t),
        int(a[1] + (b[1] - a[1]) * t),
        int(a[2] + (b[2] - a[2]) * t),
    )


def hsv_color(hue: float, sat: float = 1.0, val: float = 1.0) -> Color:
    """Цвет по оттенку (0..1), насыщенности и яркости."""
    color = pygame.Color(0, 0, 0)
    color.hsva = ((hue % 1.0) * 360.0, sat * 100.0, val * 100.0, 100.0)
    return color.r, color.g, color.b


# --------------------------------------------------------------------------- #
# Свечение
# --------------------------------------------------------------------------- #
_glow_cache: dict[tuple[Color, int], pygame.Surface] = {}


def _glow_sprite(color: Color, radius: int) -> pygame.Surface:
    key = (color, radius)
    sprite = _glow_cache.get(key)
    if sprite is None:
        if len(_glow_cache) > 320:
            _glow_cache.clear()
        sprite = pygame.Surface((radius * 2, radius * 2))
        sprite.fill((0, 0, 0))
        steps = max(6, radius // 2)
        for i in range(steps, 0, -1):
            frac = i / steps
            intensity = (1.0 - frac) ** 2 * 0.55
            pygame.draw.circle(sprite, scale_color(color, intensity), (radius, radius), max(1, int(radius * frac)))
        _glow_cache[key] = sprite
    return sprite


def draw_glow(surface: pygame.Surface, color: Color, pos: Vector2 | Point, radius: float, strength: float = 1.0) -> None:
    """Мягкое неоновое свечение вокруг точки (аддитивно)."""
    radius_i = max(4, int(radius) // 2 * 2)
    strength = round(clamp(strength, 0.0, 2.0) * 8) / 8
    if strength <= 0:
        return
    sprite = _glow_sprite(scale_color(color, strength), radius_i)
    surface.blit(sprite, (pos[0] - radius_i, pos[1] - radius_i), special_flags=pygame.BLEND_RGB_ADD)


# --------------------------------------------------------------------------- #
# Фигуры
# --------------------------------------------------------------------------- #
def regular_polygon(center: Vector2 | Point, radius: float, sides: int, rotation: float = 0.0) -> list[Point]:
    """Вершины правильного многоугольника."""
    cx, cy = center
    return [
        (cx + math.cos(rotation + i * math.tau / sides) * radius, cy + math.sin(rotation + i * math.tau / sides) * radius)
        for i in range(sides)
    ]


def draw_neon_polygon(surface: pygame.Surface, color: Color, points: list[Point], width: int = 2, fill_k: float = 0.22) -> None:
    """Многоугольник с тёмной заливкой и яркой обводкой."""
    if len(points) < 3:
        return
    if fill_k > 0:
        pygame.draw.polygon(surface, scale_color(color, fill_k), points)
    pygame.draw.polygon(surface, color, points, width)


def draw_neon_circle(surface: pygame.Surface, color: Color, center: Vector2 | Point, radius: float, width: int = 2, fill_k: float = 0.22) -> None:
    """Круг с тёмной заливкой и яркой обводкой."""
    if fill_k > 0:
        pygame.draw.circle(surface, scale_color(color, fill_k), center, radius)
    pygame.draw.circle(surface, color, center, radius, width)


def draw_dashed_line(surface: pygame.Surface, color: Color, a: Vector2, b: Vector2, dash: float = 10, gap: float = 7, offset: float = 0.0, width: int = 2) -> None:
    """Пунктирная линия; ``offset`` позволяет «бежать» штрихам."""
    delta = b - a
    length = delta.length()
    if length < 1:
        return
    direction = delta / length
    period = dash + gap
    pos = -(offset % period)
    while pos < length:
        start = max(pos, 0.0)
        end = min(pos + dash, length)
        if end > start:
            pygame.draw.line(surface, color, a + direction * start, a + direction * end, width)
        pos += period


def draw_star_lines(surface: pygame.Surface, color: Color, center: Vector2 | Point, radius: float, rotation: float, width: int = 2) -> None:
    """Пятиконечная звезда, нарисованная линиями (вершина i → i+2)."""
    pts = regular_polygon(center, radius, 5, rotation - math.pi / 2)
    for i in range(5):
        pygame.draw.line(surface, color, pts[i], pts[(i + 2) % 5], width)


def draw_assistant_icon(
    surface: pygame.Surface,
    level: int,
    center: Vector2 | Point,
    size: float,
    color: Color,
    rotation: float = 0.0,
    width: int = 2,
) -> None:
    """Фигура Помощника: квадрат, круг, треугольник, шестиугольник или звезда."""
    cx, cy = center
    if level == 1:
        half = size * 0.72
        rect = pygame.Rect(0, 0, int(half * 2), int(half * 2))
        rect.center = (int(cx), int(cy))
        pygame.draw.rect(surface, scale_color(color, 0.25), rect)
        pygame.draw.rect(surface, color, rect, width)
        inner = rect.inflate(-int(half), -int(half))
        pygame.draw.rect(surface, color, inner)
    elif level == 2:
        draw_neon_circle(surface, color, (cx, cy), size, width)
        pygame.draw.circle(surface, color, (cx, cy), max(2, size * 0.35))
    elif level == 3:
        draw_neon_polygon(surface, color, regular_polygon((cx, cy), size * 1.15, 3, rotation), width)
        pygame.draw.circle(surface, color, (cx, cy), max(2, size * 0.22))
    elif level == 4:
        draw_neon_polygon(surface, color, regular_polygon((cx, cy), size * 1.05, 6, rotation), width)
        draw_neon_polygon(surface, color, regular_polygon((cx, cy), size * 0.5, 6, -rotation * 1.6), max(1, width - 1), fill_k=0)
    else:
        draw_star_lines(surface, color, (cx, cy), size * 1.2, rotation, width)
        pygame.draw.circle(surface, color, (cx, cy), max(2, size * 0.18))


# --------------------------------------------------------------------------- #
# Текст
# --------------------------------------------------------------------------- #
_font_path: str | None = None
_font_resolved = False
_font_cache: dict[tuple[int, bool], pygame.font.Font] = {}
_text_cache: dict[tuple[str, int, Color, bool], pygame.Surface] = {}


def _resolve_font_path() -> str | None:
    """Ищет моноширинный системный шрифт с кириллицей, иначе — шрифт pygame по умолчанию."""
    for name in FONT_CANDIDATES:
        try:
            path = pygame.font.match_font(name)
        except Exception:  # noqa: BLE001 - на некоторых системах поиск шрифтов падает
            path = None
        if path:
            return path
    return None


def get_font(size: int, bold: bool = False) -> pygame.font.Font:
    """Возвращает (кэшированный) шрифт нужного размера."""
    global _font_path, _font_resolved
    key = (size, bold)
    font = _font_cache.get(key)
    if font is None:
        if not _font_resolved:
            _font_path = _resolve_font_path()
            _font_resolved = True
        try:
            font = pygame.font.Font(_font_path, size)
        except (OSError, FileNotFoundError):
            font = pygame.font.Font(None, size)
        font.set_bold(bold)
        _font_cache[key] = font
    return font


def render_text(text: str, size: int, color: Color = TEXT_COLOR, bold: bool = False) -> pygame.Surface:
    """Рендерит строку с кэшированием готовых поверхностей."""
    key = (text, size, color, bold)
    surf = _text_cache.get(key)
    if surf is None:
        if len(_text_cache) > 900:
            _text_cache.clear()
        surf = get_font(size, bold).render(text, True, color)
        _text_cache[key] = surf
    return surf


def draw_text(
    surface: pygame.Surface,
    text: str,
    pos: Vector2 | Point,
    size: int = 18,
    color: Color = TEXT_COLOR,
    anchor: str = "topleft",
    bold: bool = False,
    glow: bool = False,
) -> pygame.Rect:
    """Рисует текст с привязкой (``topleft``, ``center``, ``midtop``...)."""
    surf = render_text(text, size, color, bold)
    rect = surf.get_rect(**{anchor: (int(pos[0]), int(pos[1]))})
    if glow:
        draw_glow(surface, color, rect.center, max(rect.width, rect.height) * 0.7, 0.35)
    surface.blit(surf, rect)
    return rect


def wrap_text(text: str, size: int, max_width: int, bold: bool = False) -> list[str]:
    """Разбивает текст на строки по ширине."""
    font = get_font(size, bold)
    lines: list[str] = []
    for paragraph in text.split("\n"):
        words = paragraph.split(" ")
        current = ""
        for word in words:
            candidate = word if not current else f"{current} {word}"
            if font.size(candidate)[0] <= max_width:
                current = candidate
            else:
                if current:
                    lines.append(current)
                current = word
        lines.append(current)
    return lines


# --------------------------------------------------------------------------- #
# Панели
# --------------------------------------------------------------------------- #
_panel_cache: dict[tuple[int, int, int], pygame.Surface] = {}


def draw_panel(surface: pygame.Surface, rect: pygame.Rect, border: Color, alpha: int = 200, width: int = 2) -> None:
    """Полупрозрачная панель с неоновой рамкой."""
    key = (rect.width, rect.height, alpha)
    panel = _panel_cache.get(key)
    if panel is None:
        if len(_panel_cache) > 120:
            _panel_cache.clear()
        panel = pygame.Surface(rect.size, pygame.SRCALPHA)
        panel.fill((8, 8, 18, alpha))
        _panel_cache[key] = panel
    surface.blit(panel, rect.topleft)
    pygame.draw.rect(surface, border, rect, width)
    corner = 8
    bright = scale_color(border, 1.4)
    for cx, cy, dx, dy in (
        (rect.left, rect.top, 1, 1),
        (rect.right - 1, rect.top, -1, 1),
        (rect.left, rect.bottom - 1, 1, -1),
        (rect.right - 1, rect.bottom - 1, -1, -1),
    ):
        pygame.draw.line(surface, bright, (cx, cy), (cx + corner * dx, cy), 2)
        pygame.draw.line(surface, bright, (cx, cy), (cx, cy + corner * dy), 2)


# --------------------------------------------------------------------------- #
# Частицы
# --------------------------------------------------------------------------- #
@dataclass
class Particle:
    """Одна светящаяся частица."""

    pos: Vector2
    vel: Vector2
    life: float
    max_life: float
    color: Color
    size: float


@dataclass
class ParticleSystem:
    """Простая система частиц с аддитивной отрисовкой."""

    items: list[Particle] = field(default_factory=list)

    def burst(self, pos: Vector2, color: Color, count: int = 18, speed: float = 160.0, life: float = 0.7, size: float = 3.0) -> None:
        """Взрыв частиц во все стороны."""
        for _ in range(count):
            angle = random.uniform(0, math.tau)
            velocity = Vector2(math.cos(angle), math.sin(angle)) * random.uniform(speed * 0.3, speed)
            lifetime = random.uniform(life * 0.5, life)
            self.items.append(Particle(Vector2(pos), velocity, lifetime, lifetime, color, random.uniform(size * 0.6, size * 1.3)))

    def stream(self, pos: Vector2, velocity: Vector2, color: Color, life: float = 0.6, size: float = 2.0) -> None:
        """Одна частица с заданной скоростью (ветер, след)."""
        self.items.append(Particle(Vector2(pos), Vector2(velocity), life, life, color, size))

    def update(self, dt: float) -> None:
        """Двигает частицы и удаляет погасшие."""
        alive: list[Particle] = []
        for p in self.items:
            p.life -= dt
            if p.life > 0:
                p.pos += p.vel * dt
                p.vel *= 0.93
                alive.append(p)
        self.items = alive

    def draw(self, surface: pygame.Surface, cam: Vector2) -> None:
        """Рисует частицы со смещением камеры."""
        for p in self.items:
            k = p.life / p.max_life
            s = max(1, int(p.size * (0.5 + k * 0.5)))
            rect = pygame.Rect(int(p.pos.x - cam.x - s / 2), int(p.pos.y - cam.y - s / 2), s, s)
            surface.fill(scale_color(p.color, k), rect, special_flags=pygame.BLEND_RGB_ADD)


# --------------------------------------------------------------------------- #
# Псевдо-3D блоки
# --------------------------------------------------------------------------- #
@dataclass
class Block:
    """Выдавленный вверх прямоугольник (стена, ящик, консоль) в мировых координатах."""

    rect: pygame.Rect
    height: float
    top: Color
    side: Color
    edge: Color
    exposed: tuple[bool, bool, bool, bool] = (True, True, True, True)  # N, E, S, W
    edge_width: int = 2


def project(x: float, y: float, k: float) -> Point:
    """Экранная точка на высоте ``k`` (перспектива к центру экрана)."""
    cx, cy = SCREEN_CENTER
    return cx + (x - cx) * (1 + k), cy + (y - cy) * (1 + k)


def project_point(point: Vector2 | Point, k: float) -> Point:
    """То же, что :func:`project`, но для точки-вектора."""
    return project(point[0], point[1], k)


def draw_blocks(surface: pygame.Surface, blocks: list[Block], cam: Vector2) -> None:
    """Рисует блоки с перспективой к центру экрана: сначала боковые грани, затем крыши.

    Низкие блоки рисуются раньше высоких, чтобы высокие стены корректно
    перекрывали их.
    """
    cx, cy = SCREEN_CENTER
    for height in sorted({b.height for b in blocks}):
        group = [b for b in blocks if b.height == height]
        # Боковые грани видны только с той стороны, что смотрит на центр экрана.
        for b in group:
            x0 = b.rect.left - cam.x
            y0 = b.rect.top - cam.y
            x1 = x0 + b.rect.width
            y1 = y0 + b.rect.height
            n, e, s, w = b.exposed
            k = b.height
            light = b.side
            dark = scale_color(b.side, 0.7)
            if n and y0 > cy:
                pygame.draw.polygon(surface, light, [(x0, y0), (x1, y0), project(x1, y0, k), project(x0, y0, k)])
            if s and y1 < cy:
                pygame.draw.polygon(surface, light, [(x0, y1), (x1, y1), project(x1, y1, k), project(x0, y1, k)])
            if w and x0 > cx:
                pygame.draw.polygon(surface, dark, [(x0, y0), (x0, y1), project(x0, y1, k), project(x0, y0, k)])
            if e and x1 < cx:
                pygame.draw.polygon(surface, dark, [(x1, y0), (x1, y1), project(x1, y1, k), project(x1, y0, k)])
        for b in group:
            x0 = b.rect.left - cam.x
            y0 = b.rect.top - cam.y
            tx, ty = project(x0, y0, b.height)
            size_w = b.rect.width * (1 + b.height)
            size_h = b.rect.height * (1 + b.height)
            top = pygame.Rect(int(tx), int(ty), int(size_w + 1), int(size_h + 1))
            pygame.draw.rect(surface, b.top, top)
            n, e, s, w = b.exposed
            ew = b.edge_width
            if n:
                pygame.draw.line(surface, b.edge, top.topleft, top.topright, ew)
            if s:
                pygame.draw.line(surface, b.edge, (top.left, top.bottom - 1), (top.right, top.bottom - 1), ew)
            if w:
                pygame.draw.line(surface, b.edge, top.topleft, (top.left, top.bottom), ew)
            if e:
                pygame.draw.line(surface, b.edge, (top.right - 1, top.top), (top.right - 1, top.bottom), ew)
