"""Помощники — олицетворения нейросетей пяти уровней интеллекта.

Помощник следует за Оператором, а получив задачу, выполняет «сценарий» —
очередь шагов (идти, ждать, нажать, построить луч). Уровень определяет,
какие головоломки Помощник вообще способен решить.
"""
from __future__ import annotations

import math
import random
from collections import deque
from typing import TYPE_CHECKING, Callable

import pygame
from pygame.math import Vector2

import audio
import graphics as gfx
from settings import (
    ASSISTANT_ACKS,
    ASSISTANT_CATCHUP_DISTANCE,
    ASSISTANT_CATCHUP_TIME,
    ASSISTANT_COLORS,
    ASSISTANT_COUNT,
    ASSISTANT_NAMES,
    ASSISTANT_REFUSALS,
    ASSISTANT_SKILLS,
    ASSISTANT_SPEED,
    ASSISTANT_TASK_SPEED,
    DANGER_COLOR,
    FOLLOW_DISTANCE,
    ORACLE_REFUSAL_CHANCE,
    PUZZLE_MIN_LEVEL,
    PUZZLE_TITLES,
    REPATH_INTERVAL,
    Color,
    Tile,
)

if TYPE_CHECKING:
    from player import Player
    from puzzle import Puzzle
    from room import Room

SayCallback = Callable[["Assistant", str, Color], None]


# --------------------------------------------------------------------------- #
# Шаги сценария
# --------------------------------------------------------------------------- #
class Step:
    """Базовый шаг сценария Помощника."""

    def start(self, assistant: Assistant) -> None:
        """Вызывается один раз, когда шаг становится текущим."""

    def update(self, assistant: Assistant, dt: float) -> bool:
        """Продвигает шаг; возвращает ``True``, когда шаг завершён."""
        return True


class Walk(Step):
    """Движение по списку мировых точек."""

    def __init__(self, points: list[Vector2], speed: float = ASSISTANT_TASK_SPEED) -> None:
        self.points = [Vector2(p) for p in points]
        self.speed = speed

    def arrived(self, assistant: Assistant, point: Vector2) -> bool:
        """Хук: вызывается в каждой точке. ``False`` прерывает сценарий."""
        return True

    def update(self, assistant: Assistant, dt: float) -> bool:
        remaining = self.speed * dt
        while remaining > 0 and self.points:
            target = self.points[0]
            delta = target - assistant.pos
            distance = delta.length()
            if distance <= remaining:
                assistant.pos = Vector2(target)
                remaining -= distance
                self.points.pop(0)
                if not self.arrived(assistant, target):
                    return True
            else:
                assistant.heading = delta / distance
                assistant.pos += assistant.heading * remaining
                remaining = 0
        return not self.points


class RouteWalk(Walk):
    """Проход по записанному маршруту: наступив на повреждённый сектор, Помощник ломается."""

    def arrived(self, assistant: Assistant, point: Vector2) -> bool:
        room = assistant.room
        tile = gfx.tile_of(point)
        if room is not None and tile in room.mines:
            room.reveal_mine(tile)
            assistant.abort("Повреждённый сектор! Маршрут был неверным.")
            return False
        return True


class GoTo(Step):
    """Дойти до клетки (или до соседней с ней, если ``near``) по кратчайшему пути."""

    def __init__(self, tile: Tile, near: bool = False, speed: float = ASSISTANT_TASK_SPEED) -> None:
        self.tile = tile
        self.near = near
        self.speed = speed
        self.walk: Walk | None = None

    def start(self, assistant: Assistant) -> None:
        room = assistant.room
        if room is None:
            return
        passable = room.assistant_can_walk
        goals: set[Tile] = set()
        if passable(self.tile):
            goals.add(self.tile)
        if self.near or not goals:
            x, y = self.tile
            goals |= {n for n in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)) if passable(n)}
        path = room.find_path(assistant.tile, goals, passable) if goals else None
        if path is None:
            assistant.abort("Не могу добраться туда — путь закрыт.")
            return
        points = [gfx.tile_center(t) for t in path[1:]] or [gfx.tile_center(path[0])]
        self.walk = Walk(points, self.speed)

    def update(self, assistant: Assistant, dt: float) -> bool:
        return self.walk.update(assistant, dt) if self.walk else True


class Wait(Step):
    """Пауза."""

    def __init__(self, seconds: float) -> None:
        self.left = seconds

    def update(self, assistant: Assistant, dt: float) -> bool:
        self.left -= dt
        return self.left <= 0


class Call(Step):
    """Мгновенный вызов функции."""

    def __init__(self, fn: Callable[[], None]) -> None:
        self.fn = fn

    def start(self, assistant: Assistant) -> None:
        self.fn()


class Beam(Step):
    """Луч энергии от Помощника к цели."""

    def __init__(self, target: Vector2, seconds: float) -> None:
        self.target = Vector2(target)
        self.left = seconds

    def start(self, assistant: Assistant) -> None:
        assistant.beam_target = self.target
        audio.play("beam")

    def update(self, assistant: Assistant, dt: float) -> bool:
        self.left -= dt
        if self.left <= 0:
            assistant.beam_target = None
            return True
        return False


# --------------------------------------------------------------------------- #
# Помощник
# --------------------------------------------------------------------------- #
class Assistant:
    """Помощник с уровнем интеллекта 1-5."""

    def __init__(self, level: int, on_say: SayCallback | None = None) -> None:
        if not 1 <= level <= ASSISTANT_COUNT:
            raise ValueError(f"Уровень Помощника должен быть 1-5, получено {level}")
        self.level = level
        self.name = ASSISTANT_NAMES[level]
        self.color = ASSISTANT_COLORS[level]
        self.skills = ASSISTANT_SKILLS[level]
        self.on_say = on_say
        self.room: Room | None = None
        self.pos = Vector2()
        self.heading = Vector2(1, 0)
        self.summoned = False
        self.broken = False
        self.script: deque[Step] = deque()
        self.current: Step | None = None
        self.on_fail: Callable[[], None] | None = None
        self.path: list[Vector2] = []
        self.repath_timer = 0.0
        self.far_timer = 0.0
        self.beam_target: Vector2 | None = None
        self.mood = ""
        self.mood_timer = 0.0
        self.anim = random.uniform(0, 10)

    # ------------------------------------------------------------------ #
    @property
    def tile(self) -> Tile:
        """Клетка, в которой находится Помощник."""
        return gfx.tile_of(self.pos)

    @property
    def busy(self) -> bool:
        """Выполняет ли Помощник сейчас задачу."""
        return self.current is not None or bool(self.script)

    def say(self, text: str, mood: str = "!", color: Color | None = None) -> None:
        """Реплика Помощника (уходит в журнал сообщений) и значок над головой."""
        self.mood = mood
        self.mood_timer = 2.2
        if self.on_say:
            self.on_say(self, text, color or self.color)

    # ------------------------------------------------------------------ #
    def can_solve(self, puzzle: Puzzle) -> bool:
        """Способен ли Помощник этого уровня решить головоломку такого типа."""
        return puzzle.kind in self.skills

    def solve(self, puzzle: Puzzle) -> bool:
        """Пытается решить головоломку.

        Возвращает ``True``, если Помощник взялся за задачу (сценарий запущен),
        и ``False``, если не может: не тот уровень, поломка, вето босса,
        отказ Оракула или задача ещё не подготовлена Оператором.
        """
        if self.busy:
            self.say("Я ещё занят предыдущей задачей.", "…")
            return False
        if self.broken:
            self.say("ERR#0xV1BE: веса повреждены Вайбкодером. Выбери другого Помощника!", "×", DANGER_COLOR)
            audio.play("glitch")
            return False
        if not self.can_solve(puzzle):
            need = PUZZLE_MIN_LEVEL[puzzle.kind]
            self.say(
                f"{ASSISTANT_REFUSALS[self.level]} «{PUZZLE_TITLES[puzzle.kind]}» — нужен уровень {need}+ ({ASSISTANT_NAMES[need]}).",
                "?",
            )
            audio.play("error")
            return False
        if self.room is not None:
            veto = self.room.veto(self, puzzle)
            if veto:
                self.say(veto, "×", DANGER_COLOR)
                audio.play("error")
                return False
        if self.level == 5 and random.random() < ORACLE_REFUSAL_CHANCE:
            self.say("Задача сформулирована плохо. Я отказываюсь. Попробуй ещё раз.", "?")
            audio.play("error")
            return False
        plan = puzzle.build_plan(self)
        if plan is None:
            self.say(puzzle.last_error or "Не понимаю, что от меня нужно.", "?")
            audio.play("error")
            return False
        self.run(plan, on_fail=puzzle.cancel)
        self.say(ASSISTANT_ACKS[self.level], "!")
        audio.play("select")
        return True

    # ------------------------------------------------------------------ #
    def run(self, steps: list[Step], on_fail: Callable[[], None] | None = None) -> None:
        """Запускает сценарий."""
        self.script = deque(steps)
        self.current = None
        self.on_fail = on_fail
        self.path = []

    def abort(self, reason: str = "") -> None:
        """Прерывает сценарий (например, путь оказался закрыт)."""
        callback = self.on_fail
        self.script.clear()
        self.current = None
        self.on_fail = None
        self.beam_target = None
        if reason:
            self.say(reason, "×", DANGER_COLOR)
            audio.play("error")
        if callback:
            callback()

    def stop(self) -> None:
        """Тихо прекращает текущую работу (при смене уровня или отзыве)."""
        callback = self.on_fail
        self.script.clear()
        self.current = None
        self.on_fail = None
        self.beam_target = None
        if callback:
            callback()

    # ------------------------------------------------------------------ #
    def update(self, dt: float, player: Player) -> None:
        """Логика кадра: сценарий либо следование за Оператором."""
        self.anim += dt
        if self.mood_timer > 0:
            self.mood_timer -= dt
        if not self.summoned or self.room is None:
            return
        if self.busy:
            self._run_script(dt)
        else:
            self._follow(dt, player)

    def _run_script(self, dt: float) -> None:
        for _ in range(12):  # несколько мгновенных шагов за кадр
            if self.current is None:
                if not self.script:
                    self.on_fail = None
                    return
                self.current = self.script.popleft()
                step = self.current
                step.start(self)
                if self.current is not step:  # шаг прервал сценарий
                    return
            step = self.current
            done = step.update(self, dt)
            if self.current is not step:
                return
            if not done:
                return
            self.current = None
            dt = 0.0
        if not self.script and self.current is None:
            self.on_fail = None

    def _follow(self, dt: float, player: Player) -> None:
        room = self.room
        assert room is not None
        to_player = player.pos - self.pos
        distance = to_player.length()

        # Если Оператор убежал далеко (или недоступен) — догоняем «телепортом».
        if distance > ASSISTANT_CATCHUP_DISTANCE:
            self.far_timer += dt
            if self.far_timer > ASSISTANT_CATCHUP_TIME:
                self.teleport_near(player.pos)
                return
        else:
            self.far_timer = 0.0

        if distance < FOLLOW_DISTANCE:
            self.path = []
            return
        self.repath_timer -= dt
        if not self.path or self.repath_timer <= 0:
            self.repath_timer = REPATH_INTERVAL
            path = room.find_path_nearest(self.tile, player.tile, room.assistant_can_walk)
            self.path = [gfx.tile_center(t) for t in path[1:]] if path else []
        remaining = ASSISTANT_SPEED * dt
        while remaining > 0 and self.path:
            target = self.path[0]
            delta = target - self.pos
            dist = delta.length()
            if (player.pos - self.pos).length() < FOLLOW_DISTANCE:
                break
            if dist <= remaining:
                self.pos = Vector2(target)
                remaining -= dist
                self.path.pop(0)
            else:
                self.heading = delta / dist
                self.pos += self.heading * remaining
                remaining = 0

    def teleport_near(self, pos: Vector2) -> None:
        """Переносит Помощника на свободную клетку рядом с точкой."""
        if self.room is None:
            return
        tile = self.room.free_tile_near(gfx.tile_of(pos))
        if tile is not None:
            self.pos = gfx.tile_center(tile)
            self.path = []
            self.far_timer = 0.0
            self.room.particles.burst(self.pos, self.color, 14, 120, 0.5)

    # ------------------------------------------------------------------ #
    def draw(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        """Светящаяся фигура Помощника, луч и значок настроения."""
        if not self.summoned:
            return
        bob = math.sin(self.anim * 3.2) * 3
        center = self.pos - cam + Vector2(0, bob)
        color = self.color
        jitter = Vector2()
        if self.broken:
            flicker = int(t * 20) % 3
            color = DANGER_COLOR if flicker == 0 else gfx.scale_color(self.color, 0.45)
            jitter = Vector2(random.uniform(-3, 3), random.uniform(-3, 3))

        if self.beam_target is not None:
            target = self.beam_target - cam
            width = 3 + int(2 * abs(math.sin(t * 20)))
            pygame.draw.line(surface, gfx.scale_color(color, 0.5), center, target, width + 4)
            pygame.draw.line(surface, color, center, target, width)
            gfx.draw_glow(surface, color, target, 30, 1.0)

        gfx.draw_glow(surface, color, center, 34, 0.9)
        if self.level == 2:  # Эхо оставляет расходящиеся «эхо-кольца»
            ring = (self.anim * 0.9) % 1.0
            pygame.draw.circle(surface, gfx.scale_color(color, 1 - ring), center, 12 + ring * 22, 1)
        rotation = self.anim * (1.5 if self.level >= 4 else 0.0)
        if self.level == 3:
            rotation = math.atan2(self.heading.y, self.heading.x)
        if self.level == 5:
            rotation = self.anim * 0.8
        gfx.draw_assistant_icon(surface, self.level, center + jitter, 12, color, rotation)
        if self.broken:
            for _ in range(2):
                bar = pygame.Rect(0, 0, random.randint(10, 30), 3)
                bar.center = (int(center.x + random.uniform(-14, 14)), int(center.y + random.uniform(-14, 14)))
                surface.fill(DANGER_COLOR, bar)
            gfx.draw_text(surface, "ERR", center + Vector2(0, -28), 13, DANGER_COLOR, "center", bold=True)

        if self.mood_timer > 0 and self.mood:
            bubble = center + Vector2(16, -26)
            pygame.draw.circle(surface, (10, 10, 20), bubble, 10)
            pygame.draw.circle(surface, color, bubble, 10, 2)
            gfx.draw_text(surface, self.mood, bubble, 14, color, "center", bold=True)


# --------------------------------------------------------------------------- #
# Набор Помощников
# --------------------------------------------------------------------------- #
class AssistantRoster:
    """Все пять Помощников, доступность на уровне и выбор активного."""

    def __init__(self, on_say: SayCallback) -> None:
        self.assistants: dict[int, Assistant] = {lvl: Assistant(lvl, on_say) for lvl in range(1, ASSISTANT_COUNT + 1)}
        self.available: list[int] = [1]
        self.unlocked = 1
        self.selected = 1
        self.summoned = False
        self.room: Room | None = None

    @property
    def current(self) -> Assistant:
        """Выбранный Помощник (даже если ещё не призван)."""
        return self.assistants[self.selected]

    @property
    def active(self) -> Assistant | None:
        """Призванный Помощник или ``None``."""
        return self.current if self.summoned else None

    def setup_level(self, room: Room, available: list[int], unlocked: int) -> None:
        """Сбрасывает состояние под новую комнату."""
        self.room = room
        self.available = sorted(available)
        self.unlocked = unlocked
        self.selected = self.available[0]
        self.summoned = False
        for assistant in self.assistants.values():
            assistant.stop()
            assistant.room = room
            assistant.summoned = False
            assistant.broken = False
            assistant.beam_target = None

    def summon(self, near: Vector2) -> Assistant:
        """Призывает выбранного Помощника рядом с точкой."""
        assistant = self.current
        assistant.summoned = True
        self.summoned = True
        assistant.teleport_near(near)
        if self.room is not None:
            self.room.particles.burst(assistant.pos, assistant.color, 28, 200, 0.8)
        audio.play("summon")
        return assistant

    def select(self, level: int) -> tuple[bool, str]:
        """Выбор Помощника клавишами 1-5. Возвращает (успех, сообщение)."""
        if level > self.unlocked:
            return False, f"Помощник уровня {level} ещё не разблокирован."
        if level not in self.available:
            return False, f"{ASSISTANT_NAMES[level]} недоступен на этом уровне."
        if level == self.selected:
            return False, ""
        old = self.current
        if self.summoned and old.busy:
            return False, f"{old.name} занят задачей — дождись окончания."
        self.selected = level
        new = self.current
        if self.summoned:
            old.summoned = False
            new.summoned = True
            new.pos = Vector2(old.pos)
            new.path = []
            if self.room is not None:
                self.room.particles.burst(new.pos, new.color, 22, 170, 0.6)
            audio.play("summon")
        else:
            audio.play("select")
        return True, f"Активный Помощник: {new.name} (интеллект {new.level}/5)."

    def set_broken(self, level: int | None) -> None:
        """Делает одного Помощника «сломанным» (фаза 3 босса); ``None`` — починить всех."""
        for assistant in self.assistants.values():
            assistant.broken = assistant.level == level

    def update(self, dt: float, player: Player) -> None:
        """Обновляет активного Помощника."""
        for assistant in self.assistants.values():
            if assistant.summoned:
                assistant.update(dt, player)
            else:
                assistant.anim += dt

    def draw(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        """Рисует призванного Помощника."""
        for assistant in self.assistants.values():
            if assistant.summoned:
                assistant.draw(surface, cam, t)
