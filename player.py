"""Оператор — персонаж игрока: движение WASD, коллизии со стенами, отрисовка.

Оператор — лишь проекция внутри Песочницы: он проходит сквозь ящики
и не может нажимать кнопки сам, но упирается в стены и двери.
"""
from __future__ import annotations

import math
from collections import deque
from typing import TYPE_CHECKING, Sequence

import pygame
from pygame.math import Vector2

import graphics as gfx
from settings import PLAYER_COLOR, PLAYER_SIZE, PLAYER_SPEED, RESPAWN_INVULNERABILITY, TILE, Tile

if TYPE_CHECKING:
    from room import Room

# Логические направления и клавиши, которые им соответствуют.
DIRECTIONS: dict[str, Vector2] = {
    "w": Vector2(0, -1),
    "a": Vector2(-1, 0),
    "s": Vector2(0, 1),
    "d": Vector2(1, 0),
}
KEY_BINDINGS: dict[str, tuple[int, ...]] = {
    "w": (pygame.K_w, pygame.K_UP),
    "a": (pygame.K_a, pygame.K_LEFT),
    "s": (pygame.K_s, pygame.K_DOWN),
    "d": (pygame.K_d, pygame.K_RIGHT),
}


class Player:
    """Оператор, которым управляет игрок."""

    HALF = PLAYER_SIZE / 2

    def __init__(self, pos: Vector2 | tuple[float, float]) -> None:
        self.pos = Vector2(pos)
        self.facing = Vector2(0, 1)
        self.velocity = Vector2()
        self.external_push = Vector2()
        self.invulnerable = 0.0
        self.trail: deque[Vector2] = deque(maxlen=8)
        self._trail_timer = 0.0

    # ------------------------------------------------------------------ #
    @property
    def tile(self) -> Tile:
        """Клетка, в которой находится центр Оператора."""
        return gfx.tile_of(self.pos)

    def respawn(self, pos: Vector2) -> None:
        """Переносит Оператора в точку и даёт короткую неуязвимость."""
        self.pos = Vector2(pos)
        self.velocity = Vector2()
        self.external_push = Vector2()
        self.trail.clear()
        self.invulnerable = RESPAWN_INVULNERABILITY

    # ------------------------------------------------------------------ #
    @staticmethod
    def read_direction(keys: Sequence[bool], scramble: dict[str, str] | None = None) -> Vector2:
        """Направление движения по зажатым клавишам.

        ``scramble`` — словарь подмены (например ``{"w": "s"}``): так босс ломает управление.
        """
        direction = Vector2()
        for key_name, codes in KEY_BINDINGS.items():
            if any(keys[code] for code in codes):
                target = scramble.get(key_name, key_name) if scramble else key_name
                direction += DIRECTIONS[target]
        if direction.length_squared() > 0:
            direction = direction.normalize()
        return direction

    def update(self, dt: float, keys: Sequence[bool], room: Room, scramble: dict[str, str] | None = None) -> None:
        """Читает ввод, двигает Оператора и разрешает коллизии."""
        direction = self.read_direction(keys, scramble)
        if direction.length_squared() > 0:
            self.facing = Vector2(direction)
        self.velocity = direction * PLAYER_SPEED + self.external_push
        self.external_push = Vector2()
        self._move(self.velocity * dt, room)

        if self.invulnerable > 0:
            self.invulnerable -= dt
        self._trail_timer -= dt
        if self._trail_timer <= 0 and direction.length_squared() > 0:
            self._trail_timer = 0.05
            self.trail.append(Vector2(self.pos))
        elif direction.length_squared() == 0 and self.trail and self._trail_timer <= 0:
            self._trail_timer = 0.05
            self.trail.popleft()

    def _move(self, delta: Vector2, room: Room) -> None:
        if delta.x:
            self.pos.x += delta.x
            self._resolve(room, horizontal=True, moving_positive=delta.x > 0)
        if delta.y:
            self.pos.y += delta.y
            self._resolve(room, horizontal=False, moving_positive=delta.y > 0)

    def _resolve(self, room: Room, horizontal: bool, moving_positive: bool) -> None:
        """Выталкивает Оператора из твёрдых клеток вдоль одной оси."""
        half = self.HALF
        left, right = self.pos.x - half, self.pos.x + half
        top, bottom = self.pos.y - half, self.pos.y + half
        for ty in range(math.floor(top / TILE), math.floor((bottom - 0.001) / TILE) + 1):
            for tx in range(math.floor(left / TILE), math.floor((right - 0.001) / TILE) + 1):
                if not room.blocks_player((tx, ty)):
                    continue
                if horizontal:
                    self.pos.x = tx * TILE - half - 0.01 if moving_positive else (tx + 1) * TILE + half + 0.01
                else:
                    self.pos.y = ty * TILE - half - 0.01 if moving_positive else (ty + 1) * TILE + half + 0.01
                return

    # ------------------------------------------------------------------ #
    def draw(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        """Рисует Оператора: ядро, кольцо, указатель направления и шлейф."""
        if self.invulnerable > 0 and int(t * 16) % 2 == 0:
            return
        for i, point in enumerate(self.trail):
            k = (i + 1) / (len(self.trail) + 1)
            pygame.draw.circle(surface, gfx.scale_color(PLAYER_COLOR, 0.35 * k), point - cam, 3 + 4 * k)
        center = self.pos - cam
        pulse = 1.0 + 0.08 * math.sin(t * 5)
        gfx.draw_glow(surface, PLAYER_COLOR, center, 40 * pulse, 0.9)
        diamond = gfx.regular_polygon(center, 15 * pulse, 4, math.pi / 4 + t * 0.6)
        gfx.draw_neon_polygon(surface, PLAYER_COLOR, diamond, 2, 0.3)
        pygame.draw.circle(surface, gfx.scale_color(PLAYER_COLOR, 1.6), center, 5)
        tip = center + self.facing * 21
        pygame.draw.line(surface, gfx.scale_color(PLAYER_COLOR, 1.5), center + self.facing * 11, tip, 3)
