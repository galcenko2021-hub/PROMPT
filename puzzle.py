"""Пять типов головоломок: PushBox, ButtonSequence, RouteMemory, LinkObjects, ContextTask.

Общий принцип: Оператор — проекция и физически ничего не делает. Он готовит
задачу (отмечает кнопки, записывает маршрут, указывает ключ, формулирует
промпт), а выполняет её Помощник подходящего уровня. Каждая головоломка
сообщает о решении через ``check_solved()``.
"""
from __future__ import annotations

import math
import random
from collections import deque
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, Callable, Protocol

import pygame
from pygame.math import Vector2

import audio
import graphics as gfx
from assistant import Beam, Call, GoTo, RouteWalk, Step, Wait, Walk
from settings import (
    ASSISTANT_TASK_SPEED,
    BOX_COLOR,
    BOX_HEIGHT,
    BUTTON_SEQUENCE,
    CONTEXT_TASK,
    DANGER_COLOR,
    DEVICE_HEIGHT,
    DIM_TEXT_COLOR,
    INTERACT_RANGE,
    KEY_INTERACT_RANGE,
    LINK_OBJECTS,
    PLAYER_COLOR,
    PUSH_BOX,
    PUSH_SPEED,
    PUZZLE_TITLES,
    ROUTE_MEMORY,
    SHADOW_RADIUS,
    SUCCESS_COLOR,
    SYSTEM_COLOR,
    TEXT_COLOR,
    TILE,
    WARNING_COLOR,
    WIND_SPEED,
    Color,
    Tile,
)

if TYPE_CHECKING:
    from assistant import Assistant, AssistantRoster
    from player import Player
    from room import Room

Elements = dict[str, list[tuple[Tile, Any]]]
DIRS4: tuple[Tile, ...] = ((1, 0), (-1, 0), (0, 1), (0, -1))
BUTTON_COLORS: tuple[Color, ...] = ((255, 80, 90), (70, 255, 130), (80, 160, 255), (255, 220, 60))


class GameAPI(Protocol):
    """То, что головоломкам нужно от игры (реализует ``Game`` в main.py)."""

    player: Player
    roster: AssistantRoster
    room: Room

    def notify(self, text: str, speaker: str = "СИСТЕМА", color: Color = SYSTEM_COLOR) -> None:
        """Сообщение в журнал."""

    def open_prompt_menu(self, puzzle: ContextTask) -> None:
        """Открыть меню формулировки задачи."""

    def player_caught(self, reason: str, respawn_at: Vector2 | None = None, force: bool = False) -> None:
        """Оператор пойман/повреждён — откат к точке возрождения."""

    def on_victory(self) -> None:
        """Босс побеждён — финальный экран."""


@dataclass
class Interactable:
    """Объект, с которым можно взаимодействовать клавишей E."""

    pos: Vector2
    label: str
    action: Callable[[GameAPI], None]
    radius: float = INTERACT_RANGE
    color: Color = TEXT_COLOR


# --------------------------------------------------------------------------- #
# Базовый класс
# --------------------------------------------------------------------------- #
class Puzzle:
    """Базовая головоломка комнаты."""

    kind = ""

    def __init__(self, pid: str, room: Room, data: dict[str, Any], elements: Elements) -> None:
        self.pid = pid
        self.room = room
        self.data = data
        self.elements = elements
        self.solved = False  # выставляет комната, когда check_solved() впервые вернул True
        self.active = bool(data.get("active", True))
        self.in_progress = False
        self.last_error = ""
        self.solver_level: int | None = None

    # ------------------------------------------------------------------ #
    def _tiles(self, kind: str) -> list[Tile]:
        return [tile for tile, _ in self.elements.get(kind, [])]

    def _tile(self, kind: str) -> Tile:
        tiles = self._tiles(kind)
        if not tiles:
            raise ValueError(f"Головоломка «{self.pid}»: на карте нет элемента «{kind}»")
        return tiles[0]

    @property
    def title(self) -> str:
        """Человекочитаемое название типа головоломки."""
        return PUZZLE_TITLES[self.kind]

    @property
    def anchor(self) -> Vector2:
        """Главная точка головоломки (для подсказок и «зашифрованного» состояния)."""
        raise NotImplementedError

    # ------------------------------------------------------------------ #
    def check_solved(self) -> bool:
        """Решена ли головоломка."""
        raise NotImplementedError

    def build_plan(self, assistant: Assistant) -> list[Step] | None:
        """Сценарий для Помощника или ``None`` (причина — в ``last_error``)."""
        raise NotImplementedError

    def interactables(self) -> list[Interactable]:
        """Точки взаимодействия."""
        return []

    def solid_tiles(self) -> set[Tile]:
        """Статичные твёрдые клетки (консоли, панели, замки)."""
        return set()

    def blocking_tiles(self) -> set[Tile]:
        """Динамические препятствия для Помощников (ящики)."""
        return set()

    def blocks(self) -> list[gfx.Block]:
        """Псевдо-3D блоки головоломки."""
        return []

    def update(self, dt: float, game: GameAPI) -> None:
        """Логика кадра."""

    def draw_floor(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        """Объекты на полу (под персонажами)."""

    def draw_overlay(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        """Эффекты поверх стен (лучи, значки)."""

    def cancel(self) -> None:
        """Помощник не смог закончить сценарий."""
        self.in_progress = False

    def reset_transient(self) -> None:
        """Сбрасывает незавершённые действия Оператора (например, запись маршрута)."""

    # ------------------------------------------------------------------ #
    def request(self, game: GameAPI) -> None:
        """Передаёт задачу активному Помощнику."""
        if self.in_progress:
            game.notify("Помощник уже работает над этой задачей.")
            return
        assistant = game.roster.active
        if assistant is None:
            game.notify("Сначала призови Помощника: подойди к терминалу и нажми E.", "СИСТЕМА", WARNING_COLOR)
            audio.play("error")
            return
        assistant.solve(self)

    def _accept(self, assistant: Assistant) -> None:
        self.in_progress = True
        self.solver_level = assistant.level

    def notify(self, text: str, speaker: str = "СИСТЕМА", color: Color = SYSTEM_COLOR) -> None:
        """Сообщение в журнал через комнату."""
        self.room.notify(text, speaker, color)

    @staticmethod
    def _device_block(tile: Tile, color: Color, height: float = DEVICE_HEIGHT, shrink: int = 8) -> gfx.Block:
        rect = gfx.tile_rect(tile).inflate(-shrink, -shrink)
        return gfx.Block(rect, height, gfx.scale_color(color, 0.18), gfx.scale_color(color, 0.12), color)


# --------------------------------------------------------------------------- #
# 1. PushBox
# --------------------------------------------------------------------------- #
class PushBox(Puzzle):
    """Толкнуть ящик на плиту. Помощник сам находит решение (поиск в ширину)."""

    kind = PUSH_BOX

    def __init__(self, pid: str, room: Room, data: dict[str, Any], elements: Elements) -> None:
        super().__init__(pid, room, data, elements)
        self.box_tile = self._tile("box")
        self.target = self._tile("plate")
        self.box_pos = gfx.tile_center(self.box_tile)

    @property
    def anchor(self) -> Vector2:
        return self.box_pos

    def check_solved(self) -> bool:
        return self.box_tile == self.target and self.box_pos.distance_to(gfx.tile_center(self.target)) < 1.0

    def blocking_tiles(self) -> set[Tile]:
        return {self.box_tile}

    def interactables(self) -> list[Interactable]:
        if self.solved:
            return []
        label = "Помощник толкает ящик..." if self.in_progress else "Попросить задвинуть ящик на плиту"
        return [Interactable(self.box_pos, label, self.request, INTERACT_RANGE * 1.15, BOX_COLOR)]

    def build_plan(self, assistant: Assistant) -> list[Step] | None:
        moves = self._plan_moves(assistant.tile)
        if moves is None:
            self.last_error = "Не вижу решения: до ящика не добраться или его некуда толкать."
            return None
        steps: list[Step] = [Walk([gfx.tile_center(assistant.tile)])]
        current = assistant.tile
        for direction, push in moves:
            nxt = (current[0] + direction[0], current[1] + direction[1])
            if push:
                steps.append(Call(lambda d=direction: self._push(d)))
            steps.append(Walk([gfx.tile_center(nxt)], PUSH_SPEED if push else ASSISTANT_TASK_SPEED))
            current = nxt
        steps.append(Call(self._finish))
        self._accept(assistant)
        return steps

    def _push(self, direction: Tile) -> None:
        self.box_tile = (self.box_tile[0] + direction[0], self.box_tile[1] + direction[1])
        audio.play("push")

    def _finish(self) -> None:
        self.in_progress = False

    # ------------------------------------------------------------------ #
    def _plan_moves(self, start: Tile) -> list[tuple[Tile, bool]] | None:
        """Поиск решения «сокобана» для одного ящика.

        Состояние — (клетка ящика, сторона последнего толчка). Возвращает
        список ходов ``(направление, толчок?)`` от позиции Помощника.
        """
        room = self.room
        others = room.box_tiles() - {self.box_tile}

        def free(tile: Tile) -> bool:
            return tile not in others and room.assistant_can_walk(tile, ignore_boxes=True)

        box0 = self.box_tile
        if box0 == self.target:
            return []
        start_key: tuple[Tile, Tile] = (box0, (0, 0))
        parents: dict[tuple[Tile, Tile], tuple[tuple[Tile, Tile], Tile, Tile, Tile] | None] = {start_key: None}
        queue: deque[tuple[Tile, Tile, tuple[Tile, Tile]]] = deque([(box0, start, start_key)])
        while queue:
            box, pusher, key = queue.popleft()
            area = self._flood(pusher, box, free)
            for d in DIRS4:
                stand = (box[0] - d[0], box[1] - d[1])
                dest = (box[0] + d[0], box[1] + d[1])
                if stand not in area or not free(dest):
                    continue
                new_key = (dest, d)
                if new_key in parents:
                    continue
                parents[new_key] = (key, box, stand, d)
                if dest == self.target:
                    return self._moves_from(parents, new_key, start, free)
                queue.append((dest, box, new_key))
        return None

    @staticmethod
    def _flood(start: Tile, box: Tile, free: Callable[[Tile], bool]) -> set[Tile]:
        seen = {start}
        queue = deque([start])
        while queue:
            x, y = queue.popleft()
            for dx, dy in DIRS4:
                n = (x + dx, y + dy)
                if n not in seen and n != box and free(n):
                    seen.add(n)
                    queue.append(n)
        return seen

    def _moves_from(self, parents: dict, key: tuple[Tile, Tile], start: Tile, free: Callable[[Tile], bool]) -> list[tuple[Tile, bool]] | None:
        pushes: list[tuple[Tile, Tile, Tile]] = []
        node = parents[key]
        while node is not None:
            prev_key, box_before, stand, d = node
            pushes.append((box_before, stand, d))
            node = parents[prev_key]
        pushes.reverse()
        moves: list[tuple[Tile, bool]] = []
        current = start
        for box_before, stand, d in pushes:
            path = self.room.find_path(current, {stand}, lambda t, b=box_before: free(t) and t != b)
            if path is None:
                return None
            for a, b in zip(path, path[1:]):
                moves.append(((b[0] - a[0], b[1] - a[1]), False))
            moves.append((d, True))
            current = box_before
        return moves

    # ------------------------------------------------------------------ #
    def update(self, dt: float, game: GameAPI) -> None:
        target = gfx.tile_center(self.box_tile)
        delta = target - self.box_pos
        step = PUSH_SPEED * dt
        self.box_pos = Vector2(target) if delta.length() <= step else self.box_pos + delta.normalize() * step

    def draw_floor(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        rect = gfx.tile_rect(self.target).inflate(-8, -8).move(-cam.x, -cam.y)
        color = SUCCESS_COLOR if self.box_tile == self.target else WARNING_COLOR
        pulse = 0.5 + 0.5 * math.sin(t * 3)
        gfx.draw_glow(surface, color, rect.center, 30, 0.35 + 0.3 * pulse)
        pygame.draw.rect(surface, gfx.scale_color(color, 0.25), rect)
        pygame.draw.rect(surface, color, rect, 2)
        pygame.draw.rect(surface, color, rect.inflate(-18, -18), 1)

    def blocks(self) -> list[gfx.Block]:
        rect = pygame.Rect(0, 0, TILE - 10, TILE - 10)
        rect.center = (int(self.box_pos.x), int(self.box_pos.y))
        color = SUCCESS_COLOR if self.solved else BOX_COLOR
        return [gfx.Block(rect, BOX_HEIGHT, gfx.scale_color(color, 0.22), gfx.scale_color(color, 0.14), color)]


# --------------------------------------------------------------------------- #
# 2. ButtonSequence
# --------------------------------------------------------------------------- #
class ButtonSequence(Puzzle):
    """Нажать кнопки в порядке, показанном миганием.

    Оператор смотрит демонстрацию, отмечает кнопки по порядку,
    а Помощник физически их нажимает.
    """

    kind = BUTTON_SEQUENCE

    def __init__(self, pid: str, room: Room, data: dict[str, Any], elements: Elements) -> None:
        super().__init__(pid, room, data, elements)
        ordered = sorted(elements.get("button", []), key=lambda item: item[1])
        self.buttons: list[Tile] = [tile for tile, _ in ordered]
        if not self.buttons:
            raise ValueError(f"Головоломка «{pid}»: нет кнопок")
        self.panel = self._tile("panel")
        self.order: list[int] = list(data.get("order", range(len(self.buttons))))
        self.marks: list[int] = []
        self.pressed: list[int] = []
        self.demo_queue: list[int] = []
        self.demo_timer = 0.0
        self.flash: dict[int, float] = {}
        self.fail_timer = 0.0
        self.complete = False

    @property
    def anchor(self) -> Vector2:
        return gfx.tile_center(self.panel)

    def check_solved(self) -> bool:
        return self.complete

    def solid_tiles(self) -> set[Tile]:
        return {self.panel}

    def interactables(self) -> list[Interactable]:
        if self.solved:
            return []
        count = len(self.buttons)
        panel_label = "Отправить последовательность Помощнику" if len(self.marks) == count else "Показать порядок кнопок"
        items = [Interactable(gfx.tile_center(self.panel), panel_label, self._on_panel, color=SYSTEM_COLOR)]
        for index, tile in enumerate(self.buttons):
            label = "Сбросить метки" if index in self.marks else f"Отметить кнопку · шаг {len(self.marks) + 1}/{count}"
            items.append(
                Interactable(gfx.tile_center(tile), label, lambda game, i=index: self._on_button(game, i), color=BUTTON_COLORS[index % 4])
            )
        return items

    def _on_panel(self, game: GameAPI) -> None:
        if self.in_progress:
            game.notify("Помощник уже нажимает кнопки.")
            return
        if len(self.marks) == len(self.buttons):
            self.request(game)
        else:
            self.start_demo()
            game.notify("Запомни порядок мигания и отметь кнопки по очереди (E у каждой кнопки).")

    def _on_button(self, game: GameAPI, index: int) -> None:
        if self.in_progress:
            game.notify("Помощник уже нажимает кнопки.")
            return
        if index in self.marks:
            self.marks.clear()
            audio.play("mark")
            game.notify("Метки сброшены.")
            return
        self.marks.append(index)
        self.flash[index] = 0.25
        audio.play(f"tone{index % 4}")
        if len(self.marks) == len(self.buttons):
            game.notify("Последовательность составлена — передаю Помощнику.")
            self.request(game)

    def start_demo(self) -> None:
        """Запускает демонстрацию порядка миганием."""
        self.demo_queue = list(self.order)
        self.demo_timer = 0.5

    def build_plan(self, assistant: Assistant) -> list[Step] | None:
        if len(self.marks) < len(self.buttons):
            self.last_error = "Сначала отметь все кнопки в правильном порядке."
            return None
        steps: list[Step] = []
        for index in self.marks:
            steps += [GoTo(self.buttons[index]), Call(lambda i=index: self._press(i)), Wait(0.25)]
        steps.append(Call(self._evaluate))
        self.pressed = []
        self._accept(assistant)
        return steps

    def _press(self, index: int) -> None:
        self.pressed.append(index)
        self.flash[index] = 0.6
        audio.play(f"tone{index % 4}")

    def _evaluate(self) -> None:
        self.in_progress = False
        if self.pressed == self.order:
            self.complete = True
            return
        self.fail_timer = 1.2
        self.marks.clear()
        self.pressed.clear()
        audio.play("error")
        self.notify("Неверный порядок! Посмотри мигание ещё раз (E у панели).", "СИСТЕМА", DANGER_COLOR)

    def cancel(self) -> None:
        self.in_progress = False
        self.pressed.clear()

    def update(self, dt: float, game: GameAPI) -> None:
        for index in list(self.flash):
            self.flash[index] -= dt
            if self.flash[index] <= 0:
                del self.flash[index]
        if self.fail_timer > 0:
            self.fail_timer -= dt
        if self.demo_queue:
            self.demo_timer -= dt
            if self.demo_timer <= 0:
                index = self.demo_queue.pop(0)
                self.flash[index] = 0.45
                audio.play(f"tone{index % 4}")
                self.demo_timer = 0.7

    def draw_floor(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        for index, tile in enumerate(self.buttons):
            center = gfx.tile_center(tile) - cam
            base = BUTTON_COLORS[index % 4]
            lit = index in self.flash or (self.in_progress and index in self.pressed)
            if self.solved:
                color, k = SUCCESS_COLOR, 0.8
            elif self.fail_timer > 0:
                color, k = DANGER_COLOR, 0.5 + 0.5 * abs(math.sin(t * 12))
            else:
                color, k = base, (1.0 if lit else 0.35)
            if lit or self.solved:
                gfx.draw_glow(surface, color, center, 46, 1.0)
            pygame.draw.circle(surface, gfx.scale_color(color, 0.18 * k + 0.05), center, 17)
            pygame.draw.circle(surface, gfx.scale_color(color, k), center, 17, 3)
            pygame.draw.circle(surface, gfx.scale_color(color, k), center, 7)
            if index in self.marks:
                step = self.marks.index(index) + 1
                gfx.draw_text(surface, str(step), center + Vector2(15, -18), 16, WARNING_COLOR, "center", bold=True)

    def blocks(self) -> list[gfx.Block]:
        color = SUCCESS_COLOR if self.solved else SYSTEM_COLOR
        return [self._device_block(self.panel, color)]

    def draw_overlay(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        center = Vector2(gfx.project_point(gfx.tile_center(self.panel) - cam, DEVICE_HEIGHT))
        for i in range(len(self.buttons)):
            led = center + Vector2((i - (len(self.buttons) - 1) / 2) * 8, 0)
            color = BUTTON_COLORS[i % 4] if (i in self.flash and not self.in_progress) else gfx.scale_color(SYSTEM_COLOR, 0.4)
            pygame.draw.circle(surface, color, led, 3)


# --------------------------------------------------------------------------- #
# 3. RouteMemory
# --------------------------------------------------------------------------- #
class RouteMemory(Puzzle):
    """Помощник проходит маршрут, который Оператор заранее записал своим движением (WASD)."""

    kind = ROUTE_MEMORY

    def __init__(self, pid: str, room: Room, data: dict[str, Any], elements: Elements) -> None:
        super().__init__(pid, room, data, elements)
        self.pad = self._tile("pad")
        self.goal = self._tile("goal")
        self.recording = False
        self.ready = False
        self.route: list[Tile] = []
        self.complete = False

    @property
    def anchor(self) -> Vector2:
        return gfx.tile_center(self.pad)

    def check_solved(self) -> bool:
        return self.complete

    def interactables(self) -> list[Interactable]:
        if self.solved:
            return []
        pad_label = "Перезаписать маршрут" if (self.ready or self.recording) else "Начать запись маршрута"
        goal_label = "Отправить Помощника по маршруту" if self.ready else "Цель: сначала запиши маршрут"
        return [
            Interactable(gfx.tile_center(self.pad), pad_label, self._on_pad, color=DANGER_COLOR),
            Interactable(gfx.tile_center(self.goal), goal_label, self._on_goal, color=WARNING_COLOR),
        ]

    def _on_pad(self, game: GameAPI) -> None:
        if self.in_progress:
            game.notify("Помощник уже идёт по маршруту.")
            return
        self.recording = True
        self.ready = False
        self.route = [self.pad]
        if game.player.tile != self.pad:
            self.route.append(game.player.tile)
        audio.play("rec")
        game.notify("● REC: веди запись — дойди до цели, не наступая на повреждённые сектора.", "СИСТЕМА", DANGER_COLOR)

    def _on_goal(self, game: GameAPI) -> None:
        if self.ready:
            self.request(game)
        else:
            game.notify("Маршрут не записан. Встань на площадку REC и нажми E.")

    def stop_recording(self, reason: str = "") -> None:
        """Прерывает запись маршрута."""
        if self.recording:
            self.recording = False
            self.route = []
            if reason:
                self.notify(reason, "СИСТЕМА", DANGER_COLOR)

    def reset_transient(self) -> None:
        self.stop_recording()

    def update(self, dt: float, game: GameAPI) -> None:
        if not self.recording or not self.active:
            return
        tile = game.player.tile
        if tile != self.route[-1]:
            if tile in self.route:  # петли вырезаем — Помощнику не нужно ходить кругами
                del self.route[self.route.index(tile) + 1 :]
            else:
                self.route.append(tile)
        if tile == self.goal:
            self.recording = False
            self.ready = True
            audio.play("success")
            game.notify(f"Маршрут записан: {len(self.route) - 1} шагов. Нажми E, чтобы отправить Помощника.", "СИСТЕМА", SUCCESS_COLOR)

    def build_plan(self, assistant: Assistant) -> list[Step] | None:
        if not self.ready or len(self.route) < 2:
            self.last_error = "Маршрут не записан. Встань на площадку REC и нажми E."
            return None
        points = [gfx.tile_center(t) for t in self.route[1:]]
        steps: list[Step] = [GoTo(self.route[0]), RouteWalk(points), Call(self._arrive)]
        self._accept(assistant)
        return steps

    def _arrive(self) -> None:
        self.in_progress = False
        self.complete = True
        self.room.particles.burst(gfx.tile_center(self.goal), SUCCESS_COLOR, 30, 220, 0.9)

    def cancel(self) -> None:
        self.in_progress = False

    def draw_floor(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        pad = gfx.tile_center(self.pad) - cam
        rec_color = DANGER_COLOR if self.recording else gfx.scale_color(DANGER_COLOR, 0.6)
        pulse = 0.5 + 0.5 * math.sin(t * (8 if self.recording else 2))
        gfx.draw_glow(surface, rec_color, pad, 40, 0.4 + 0.5 * pulse)
        pygame.draw.circle(surface, gfx.scale_color(rec_color, 0.2), pad, 19)
        pygame.draw.circle(surface, rec_color, pad, 19, 2)
        pygame.draw.circle(surface, rec_color, pad, 6 + 3 * pulse)
        gfx.draw_text(surface, "REC", pad + Vector2(0, 26), 12, rec_color, "center", bold=True)

        goal = gfx.tile_center(self.goal) - cam
        goal_color = SUCCESS_COLOR if self.complete else WARNING_COLOR
        gfx.draw_glow(surface, goal_color, goal, 40, 0.6)
        gfx.draw_neon_polygon(surface, goal_color, gfx.regular_polygon(goal, 17, 4, t * 1.5), 2, 0.3)

        if len(self.route) >= 2 and (self.recording or self.ready):
            color = gfx.scale_color(WARNING_COLOR, 0.9)
            points = [gfx.tile_center(tile) - cam for tile in self.route]
            for a, b in zip(points, points[1:]):
                gfx.draw_dashed_line(surface, color, a, b, 8, 6, t * 30, 2)
            for p in points[1:]:
                pygame.draw.circle(surface, color, p, 3)


# --------------------------------------------------------------------------- #
# 4. LinkObjects
# --------------------------------------------------------------------------- #
class LinkObjects(Puzzle):
    """Связать ключ и замок, стоящие в разных комнатах (ключ — за стеклом)."""

    kind = LINK_OBJECTS

    def __init__(self, pid: str, room: Room, data: dict[str, Any], elements: Elements) -> None:
        super().__init__(pid, room, data, elements)
        self.key = self._tile("key")
        self.lock = self._tile("lock")
        self.shape: str = data.get("shape", "triangle")
        self.color: Color = tuple(data.get("color", (255, 230, 80)))  # type: ignore[assignment]
        self.symbol: str = data.get("symbol", "▲")
        self.linked = False
        self.progress = 0.0
        self.complete = False

    @property
    def anchor(self) -> Vector2:
        return gfx.tile_center(self.lock)

    def check_solved(self) -> bool:
        return self.complete

    def solid_tiles(self) -> set[Tile]:
        return {self.key, self.lock}

    @property
    def marked(self) -> bool:
        """Отмечен ли ключ этой пары как источник связи."""
        return self.room.link_source is self

    def interactables(self) -> list[Interactable]:
        if self.solved or self.linked:
            return []
        key_label = f"Ключ {self.symbol} отмечен" if self.marked else f"Отметить ключ {self.symbol}"
        return [
            Interactable(gfx.tile_center(self.key), key_label, self._on_key, KEY_INTERACT_RANGE, self.color),
            Interactable(gfx.tile_center(self.lock), f"Связать ключ с замком {self.symbol}", self.request, INTERACT_RANGE, self.color),
        ]

    def _on_key(self, game: GameAPI) -> None:
        self.room.link_source = self
        audio.play("mark")
        game.notify(f"Ключ {self.symbol} отмечен. Теперь подойди к замку и попроси Помощника связать их.")

    def build_plan(self, assistant: Assistant) -> list[Step] | None:
        source = self.room.link_source
        if source is None:
            self.last_error = "Сначала отметь ключ: подойди к нему и нажми E."
            return None
        if source is not self:
            self.last_error = f"Ключ {source.symbol} не подходит к замку {self.symbol}. Нужна пара одной формы."
            return None
        steps: list[Step] = [GoTo(self.lock, near=True), Wait(0.2), Beam(gfx.tile_center(self.key), 0.9), Call(self._link)]
        self._accept(assistant)
        return steps

    def _link(self) -> None:
        self.linked = True
        self.in_progress = False
        if self.room.link_source is self:
            self.room.link_source = None

    def cancel(self) -> None:
        self.in_progress = False

    def update(self, dt: float, game: GameAPI) -> None:
        if self.linked and not self.complete:
            self.progress = min(1.0, self.progress + dt / 0.9)
            if self.progress >= 1.0:
                self.complete = True
                self.room.particles.burst(gfx.tile_center(self.lock), self.color, 30, 200, 0.8)

    def _draw_shape(self, surface: pygame.Surface, center: Vector2, size: float, rotation: float, color: Color) -> None:
        if self.shape == "diamond":
            gfx.draw_neon_polygon(surface, color, gfx.regular_polygon(center, size, 4, rotation), 2, 0.3)
        elif self.shape == "circle":
            gfx.draw_neon_circle(surface, color, center, size * 0.8, 2, 0.3)
        else:
            gfx.draw_neon_polygon(surface, color, gfx.regular_polygon(center, size, 3, rotation - math.pi / 2), 2, 0.3)

    def draw_floor(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        key = gfx.tile_center(self.key) - cam
        gfx.draw_glow(surface, self.color, key, 46, 0.9)
        if self.marked:
            pygame.draw.circle(surface, self.color, key, 26 + 3 * math.sin(t * 6), 2)

    def blocks(self) -> list[gfx.Block]:
        color = SUCCESS_COLOR if self.complete else self.color
        return [self._device_block(self.lock, color, DEVICE_HEIGHT, 6), self._device_block(self.key, self.color, 0.02, 16)]

    def draw_overlay(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        key = Vector2(gfx.project_point(gfx.tile_center(self.key) - cam, 0.02))
        lock = Vector2(gfx.project_point(gfx.tile_center(self.lock) - cam, DEVICE_HEIGHT))
        self._draw_shape(surface, key, 13, t * 1.4, self.color)
        lock_color = SUCCESS_COLOR if self.complete else self.color
        self._draw_shape(surface, lock, 10, 0.0, lock_color)
        if self.linked:
            end = key.lerp(lock, self.progress)
            pygame.draw.line(surface, gfx.scale_color(self.color, 0.35), key, end, 7)
            gfx.draw_dashed_line(surface, self.color, key, end, 12, 6, -t * 60, 3)
            gfx.draw_glow(surface, self.color, end, 26, 0.8)


# --------------------------------------------------------------------------- #
# 5. ContextTask
# --------------------------------------------------------------------------- #
class ContextTask(Puzzle):
    """Задача, описанная текстом. Оператор формулирует промпт в меню, Помощник выполняет.

    Сценарии: ``fan`` (вентилятор с ветром), ``shadow`` (Тень в темноте),
    ``boss`` (переобучение Вайбкодера).
    """

    kind = CONTEXT_TASK

    def __init__(self, pid: str, room: Room, data: dict[str, Any], elements: Elements) -> None:
        super().__init__(pid, room, data, elements)
        self.console = self._tile("console")
        self.switch = self._tile("switch")
        self.scenario: str = data.get("scenario", "generic")
        self.description: str = data["description"]
        self.slots: list[tuple[str, list[str]]] = [(name, list(options)) for name, options in data["slots"]]
        self.answers: list[tuple[str, ...]] = [tuple(answer) for answer in data["answers"]]
        self.success_text: str = data.get("success", "Готово.")
        self.formulated: tuple[str, ...] | None = None
        self.effect_on = False
        self.effect = 0.0
        self.complete = False
        shadows = self._tiles("shadow")
        self.shadow: Tile | None = shadows[0] if shadows else None
        self.dark: set[Tile] = set(self._tiles("dark")) | ({self.shadow} if self.shadow else set())
        fans = self._tiles("fan")
        self.fan: Tile | None = fans[0] if fans else None
        self.wind: set[Tile] = set(self._tiles("wind"))
        self.wind_dir = Vector2(data.get("wind_dir", (-1, 0)))
        self.fan_angle = 0.0
        self._wind_timer = 0.0

    @property
    def anchor(self) -> Vector2:
        return gfx.tile_center(self.console)

    def check_solved(self) -> bool:
        return self.complete

    def solid_tiles(self) -> set[Tile]:
        tiles = {self.console, self.switch}
        if self.fan:
            tiles.add(self.fan)
        return tiles

    @property
    def hazard_active(self) -> bool:
        """Опасность сценария (ветер или Тень) ещё действует."""
        return self.effect < 0.35

    @staticmethod
    def format_prompt(prompt: tuple[str, ...]) -> str:
        """Собирает промпт в строку: «Включи свет, чтобы враг исчез»."""
        if len(prompt) >= 3:
            text = f"{prompt[0]} {prompt[1]}, {', '.join(prompt[2:])}"
        else:
            text = " ".join(prompt)
        return text[:1].upper() + text[1:]

    def interactables(self) -> list[Interactable]:
        if self.solved:
            return []
        label = "Помощник выполняет задачу..." if self.in_progress else "Сформулировать задачу (промпт)"
        return [Interactable(gfx.tile_center(self.console), label, self._on_console, color=SYSTEM_COLOR)]

    def _on_console(self, game: GameAPI) -> None:
        if self.in_progress:
            game.notify("Помощник уже выполняет задачу.")
            return
        audio.play("blip")
        game.open_prompt_menu(self)

    def submit(self, prompt: tuple[str, ...], game: GameAPI) -> None:
        """Принимает промпт из меню и передаёт задачу активному Помощнику."""
        self.formulated = prompt
        game.notify(f"«{self.format_prompt(prompt)}»", "ОПЕРАТОР", PLAYER_COLOR)
        self.request(game)

    def build_plan(self, assistant: Assistant) -> list[Step] | None:
        if self.formulated is None:
            self.last_error = "Задача не сформулирована. Подойди к консоли и составь промпт."
            return None
        prompt = self.formulated
        # Оракул угадывает намерение даже по неточному промпту.
        correct = prompt in self.answers or assistant.level >= 5
        boss = self.room.boss
        if self.scenario == "boss" and boss is not None:
            beam = Beam(boss.pos, 1.4)
        else:
            beam = Beam(gfx.tile_center(self.switch), 0.5)
        steps: list[Step] = [GoTo(self.switch, near=True), Wait(0.3), beam, Call(lambda: self._apply(prompt, correct, assistant))]
        self._accept(assistant)
        return steps

    def _apply(self, prompt: tuple[str, ...], correct: bool, assistant: Assistant) -> None:
        self.in_progress = False
        self.formulated = None
        text = self.format_prompt(prompt)
        if correct:
            self.effect_on = True
            audio.play("success")
            self.notify(self.success_text, assistant.name, assistant.color)
            return
        if any(prompt[:2] == answer[:2] for answer in self.answers):
            assistant.say(f"Сделал «{text}», но цель не та — откатываю изменения. Уточни контекст.", "?")
        else:
            assistant.say(f"Выполнил буквально: «{text}». Задачу это не решает.", "?")
        audio.play("error")

    def cancel(self) -> None:
        self.in_progress = False

    def update(self, dt: float, game: GameAPI) -> None:
        if self.effect_on and not self.complete:
            self.effect = min(1.0, self.effect + dt / 1.4)
            if self.effect >= 1.0:
                self.complete = True
        if self.scenario == "fan" and self.fan is not None:
            self.fan_angle += dt * 14 * (1.0 - self.effect)
            if self.hazard_active and self.active:
                if game.player.tile in self.wind:
                    game.player.external_push += self.wind_dir * WIND_SPEED
                self._wind_timer -= dt
                if self._wind_timer <= 0 and self.wind:
                    self._wind_timer = 0.03
                    tile = random.choice(tuple(self.wind))
                    pos = gfx.tile_center(tile) + Vector2(random.uniform(-TILE / 2, TILE / 2), random.uniform(-TILE / 2, TILE / 2))
                    self.room.particles.stream(pos, self.wind_dir * 420, (90, 140, 200), 0.5, 2)
        if self.scenario == "shadow" and self.shadow is not None and self.hazard_active and self.active:
            if game.player.pos.distance_to(gfx.tile_center(self.shadow)) < SHADOW_RADIUS:
                game.player_caught("Тень поглотила тебя. Тени боятся света...")

    _dark_cache: dict[int, pygame.Surface] = {}

    @classmethod
    def _dark_tile(cls, alpha: int) -> pygame.Surface:
        alpha = max(0, min(255, alpha // 8 * 8))
        surf = cls._dark_cache.get(alpha)
        if surf is None:
            surf = pygame.Surface((TILE, TILE), pygame.SRCALPHA)
            surf.fill((0, 0, 0, alpha))
            cls._dark_cache[alpha] = surf
        return surf

    def draw_floor(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        if self.dark:
            dark = self._dark_tile(int(225 * (1.0 - self.effect)))
            for tile in self.dark:
                surface.blit(dark, (tile[0] * TILE - cam.x, tile[1] * TILE - cam.y))
                if self.effect > 0:
                    gfx.draw_glow(surface, (255, 230, 170), gfx.tile_center(tile) - cam, 40, 0.5 * self.effect)
        if self.shadow is not None and self.effect < 1.0:
            center = gfx.tile_center(self.shadow) - cam
            fade = 1.0 - self.effect
            ring = gfx.scale_color(DANGER_COLOR, 0.45 * fade)
            for i in range(24):  # пунктирное кольцо радиуса обнаружения
                a0 = i / 24 * math.tau + t * 0.3
                a1 = a0 + math.tau / 48
                p0 = center + Vector2(math.cos(a0), math.sin(a0)) * SHADOW_RADIUS
                p1 = center + Vector2(math.cos(a1), math.sin(a1)) * SHADOW_RADIUS
                pygame.draw.line(surface, ring, p0, p1, 2)
            points = []
            for i in range(14):
                angle = i / 14 * math.tau
                radius = (26 + 7 * math.sin(t * 4 + i * 1.9)) * (0.4 + 0.6 * fade)
                points.append((center.x + math.cos(angle) * radius, center.y + math.sin(angle) * radius))
            pygame.draw.polygon(surface, (4, 0, 8), points)
            pygame.draw.polygon(surface, gfx.scale_color((150, 40, 220), fade), points, 2)
            for side in (-1, 1):
                eye = center + Vector2(side * 8, -4)
                gfx.draw_glow(surface, DANGER_COLOR, eye, 12, fade)
                pygame.draw.circle(surface, gfx.scale_color(DANGER_COLOR, fade), eye, 3)

    def blocks(self) -> list[gfx.Block]:
        done_color = SUCCESS_COLOR if (self.effect_on or self.solved) else SYSTEM_COLOR
        items = [self._device_block(self.console, done_color), self._device_block(self.switch, done_color, DEVICE_HEIGHT, 14)]
        if self.fan is not None:
            items.append(self._device_block(self.fan, (90, 140, 200), 0.02, 2))
        return items

    def draw_overlay(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        console = Vector2(gfx.project_point(gfx.tile_center(self.console) - cam, DEVICE_HEIGHT))
        color = SUCCESS_COLOR if self.solved else SYSTEM_COLOR
        gfx.draw_text(surface, ">_" if int(t * 2) % 2 else ">", console, 16, color, "center", bold=True)
        if self.fan is not None:
            center = gfx.tile_center(self.fan) - cam
            fan_color = gfx.mix_color((120, 190, 255), DIM_TEXT_COLOR, self.effect)
            gfx.draw_glow(surface, fan_color, center, 70, 0.5 * (1 - self.effect) + 0.1)
            pygame.draw.circle(surface, fan_color, center, 60, 2)
            for i in range(4):
                angle = self.fan_angle + i * math.pi / 2
                tip = center + Vector2(math.cos(angle), math.sin(angle)) * 54
                side = center + Vector2(math.cos(angle + 0.45), math.sin(angle + 0.45)) * 30
                pygame.draw.polygon(surface, gfx.scale_color(fan_color, 0.35), [center, side, tip])
                pygame.draw.line(surface, fan_color, center, tip, 2)
            pygame.draw.circle(surface, fan_color, center, 8)


PUZZLE_TYPES: dict[str, type[Puzzle]] = {
    PUSH_BOX: PushBox,
    BUTTON_SEQUENCE: ButtonSequence,
    ROUTE_MEMORY: RouteMemory,
    LINK_OBJECTS: LinkObjects,
    CONTEXT_TASK: ContextTask,
}


def create_puzzle(data: dict[str, Any], room: Room, elements: Elements) -> Puzzle:
    """Создаёт головоломку нужного типа по данным уровня."""
    cls = PUZZLE_TYPES[data["type"]]
    return cls(data["id"], room, data, elements)
