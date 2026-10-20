"""Босс «Вайбкодер» — нестабильная фигура, которая ломает правила игры.

Фаза 1 — путает клавиши WASD на 5 секунд.
Фаза 2 — переворачивает экран (``pygame.transform.flip`` в main.py).
Фаза 3 — подменяет Помощников: случайный из них становится «сломанным».

Каждая решённая в бою головоломка снимает 1/3 HP. Помощник, нанёсший урон,
«копируется» боссом и в следующей фазе бесполезен — приходится чередовать
Помощников, чтобы переобучить Вайбкодера.
"""
from __future__ import annotations

import math
import random
from typing import TYPE_CHECKING

import pygame
from pygame.math import Vector2

import audio
import graphics as gfx
from settings import (
    ASSISTANT_NAMES,
    BOSS_MAX_HP,
    BOSS_RADIUS,
    BUG_COUNT_BY_PHASE,
    BUG_SPEED,
    CONTROL_SWAP_DURATION,
    CONTROL_SWAP_INTERVAL,
    CORRUPTION_INTERVAL,
    DANGER_COLOR,
    FLIP_OFF_DURATION,
    FLIP_ON_DURATION,
    TILE,
    Color,
)

if TYPE_CHECKING:
    from assistant import Assistant
    from puzzle import GameAPI, Puzzle
    from room import Room

BOSS_SPEAKER = "ВАЙБКОДЕР"
BOSS_COLOR: Color = (255, 70, 200)
CODE_FRAGMENTS = (
    "vibe()",
    "// TODO",
    "rm -rf /",
    "while(true)",
    "copy+paste",
    "200 OK?",
    "it works!",
    "import *",
    "LGTM",
    "git push -f",
)
TAUNTS = (
    "Зачем понимать код, если можно его нагенерировать?",
    "Тесты? Я просто чувствую, что всё работает.",
    "Ещё один промпт — и всё точно заработает!",
    "Мой код компилируется с третьего раза. Это успех.",
    "Документация — для слабых.",
)
PHASE_LINES = {
    1: "Сначала поиграем с управлением. Где тут W, а где S?",
    2: "А теперь — вверх ногами! Мир — это просто массив, его можно развернуть.",
    3: "Твои Помощники — мои Помощники. Попробуй угадать, кто из них сломан.",
}


class GlitchBug:
    """«Баг» босса: медленно ползёт к Оператору; касание откатывает к началу арены."""

    RADIUS = 11

    def __init__(self, pos: Vector2) -> None:
        self.pos = Vector2(pos)
        self.velocity = Vector2()
        self.seed = random.uniform(0, 100)

    def _blocked(self, room: Room, point: Vector2) -> bool:
        tile = gfx.tile_of(point)
        return room.blocks_player(tile) or tile in room.field

    def update(self, dt: float, room: Room, target: Vector2) -> bool:
        """Двигается к цели; возвращает ``True`` при касании."""
        self.seed += dt
        desired = target - self.pos
        if desired.length_squared() > 0:
            desired = desired.normalize()
        wobble = Vector2(math.cos(self.seed * 3.1), math.sin(self.seed * 2.3)) * 0.6
        self.velocity = self.velocity.lerp((desired + wobble) * BUG_SPEED, min(1.0, dt * 3))
        step_x = Vector2(self.velocity.x * dt, 0)
        if not self._blocked(room, self.pos + step_x + Vector2(math.copysign(self.RADIUS, step_x.x or 1), 0)):
            self.pos += step_x
        step_y = Vector2(0, self.velocity.y * dt)
        if not self._blocked(room, self.pos + step_y + Vector2(0, math.copysign(self.RADIUS, step_y.y or 1))):
            self.pos += step_y
        return self.pos.distance_to(target) < self.RADIUS + 12

    def draw(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        """Мерцающий красный «битый пиксель»."""
        center = self.pos - cam
        gfx.draw_glow(surface, DANGER_COLOR, center, 28, 0.8)
        for i in range(3):
            size = random.randint(6, 14)
            rect = pygame.Rect(0, 0, size, size)
            rect.center = (int(center.x + random.uniform(-5, 5)), int(center.y + random.uniform(-5, 5)))
            pygame.draw.rect(surface, DANGER_COLOR if i == 0 else (255, 160, 200), rect, 0 if i == 0 else 1)


class Vibecoder:
    """Финальный босс."""

    def __init__(self, pos: Vector2, phase_ids: list[str], room: Room) -> None:
        if len(phase_ids) != BOSS_MAX_HP:
            raise ValueError("У Вайбкодера должно быть ровно 3 фазовые головоломки")
        self.pos = Vector2(pos)
        self.phase_ids = phase_ids
        self.room = room
        self.hp = BOSS_MAX_HP
        self.time = 0.0
        self.started = False
        self.hurt = 0.0
        self.defeated = False
        self.defeat_timer = 0.0
        self.finished = False
        self.swap: dict[str, str] | None = None
        self.swap_timer = 0.0
        self.swap_cooldown = 3.0
        self.flip: tuple[bool, bool] = (False, False)
        self.flip_timer = 0.0
        self.flip_cooldown = 2.5
        self.corrupted: int | None = None
        self.corrupt_timer = 1.5
        self.copied: int | None = None
        self.bugs: list[GlitchBug] = []
        self.taunt_timer = 12.0
        self._activate_phase()

    # ------------------------------------------------------------------ #
    @property
    def phase(self) -> int:
        """Текущая фаза боя (1-3)."""
        return min(BOSS_MAX_HP, BOSS_MAX_HP - self.hp + 1)

    @property
    def flip_flags(self) -> tuple[bool, bool]:
        """Как сейчас перевёрнут экран: (по горизонтали, по вертикали)."""
        return (False, False) if self.defeated else self.flip

    @property
    def control_scramble(self) -> dict[str, str] | None:
        """Текущая подмена клавиш или ``None``."""
        return None if self.defeated else self.swap

    def status_text(self) -> str:
        """Строка о текущем эффекте для HUD."""
        if self.defeated:
            return "Вайбкодер переобучен..."
        if self.swap:
            pairs = ", ".join(f"{k.upper()}→{v.upper()}" for k, v in self.swap.items())
            return f"Управление сломано ({self.swap_timer:.1f} с): {pairs}"
        if any(self.flip):
            return f"Мир перевёрнут ({self.flip_timer:.1f} с)"
        if self.corrupted is not None:
            return f"Сломан: {ASSISTANT_NAMES[self.corrupted]} (смена через {self.corrupt_timer:.1f} с)"
        if self.copied is not None:
            return f"Скопирован: {ASSISTANT_NAMES[self.copied]} — нужен другой Помощник"
        return ""

    # ------------------------------------------------------------------ #
    def _activate_phase(self) -> None:
        for index, pid in enumerate(self.phase_ids):
            puzzle = self.room.puzzle_by_id.get(pid)
            if puzzle is not None and not puzzle.solved:
                puzzle.active = index == self.phase - 1

    def _spawn_bugs(self, game: GameAPI) -> None:
        self.bugs = []
        candidates = [
            (x, y)
            for y in range(self.room.height)
            for x in range(self.room.width)
            if not self.room.blocks_player((x, y))
            and (x, y) not in self.room.field
            and gfx.tile_center((x, y)).distance_to(game.player.pos) > TILE * 7
            and gfx.tile_center((x, y)).distance_to(self.pos) > TILE * 3
        ]
        random.shuffle(candidates)
        for tile in candidates[: BUG_COUNT_BY_PHASE.get(self.phase, 0)]:
            self.bugs.append(GlitchBug(gfx.tile_center(tile)))

    def _say(self, text: str) -> None:
        self.room.notify(text, BOSS_SPEAKER, BOSS_COLOR)

    def _start(self, game: GameAPI) -> None:
        self.started = True
        self._spawn_bugs(game)
        self._say(PHASE_LINES[1])

    @staticmethod
    def _derangement() -> dict[str, str]:
        keys = ["w", "a", "s", "d"]
        shuffled = keys[:]
        while any(a == b for a, b in zip(keys, shuffled)):
            random.shuffle(shuffled)
        return dict(zip(keys, shuffled))

    # ------------------------------------------------------------------ #
    def update(self, dt: float, game: GameAPI) -> None:
        """Логика боя: эффекты фаз, баги, реплики."""
        self.time += dt
        if not self.started:
            self._start(game)
        if self.defeated:
            self.defeat_timer += dt
            if random.random() < 0.3:
                self.room.particles.burst(self.pos, gfx.hsv_color(random.random()), 8, 260, 0.9)
            if self.defeat_timer > 3.4 and not self.finished:
                self.finished = True
                game.on_victory()
            return
        if self.hurt > 0:
            self.hurt -= dt

        phase = self.phase
        if phase == 1:
            self._update_swap(dt)
        elif phase == 2:
            self._update_flip(dt)
        else:
            self._update_corruption(dt, game)

        for bug in self.bugs:
            if bug.update(dt, self.room, game.player.pos) and game.player.invulnerable <= 0:
                game.player_caught("Баг Вайбкодера задел тебя! Откат к точке входа.")
                break

        self.taunt_timer -= dt
        if self.taunt_timer <= 0:
            self.taunt_timer = random.uniform(13, 18)
            self._say(random.choice(TAUNTS))

    def _update_swap(self, dt: float) -> None:
        if self.swap:
            self.swap_timer -= dt
            if self.swap_timer <= 0:
                self.swap = None
                self.swap_cooldown = CONTROL_SWAP_INTERVAL
                self.room.notify("Управление восстановлено.", "СИСТЕМА", (140, 160, 255))
        else:
            self.swap_cooldown -= dt
            if self.swap_cooldown <= 0:
                self.swap = self._derangement()
                self.swap_timer = CONTROL_SWAP_DURATION
                audio.play("glitch")
                self.room.shake = max(self.room.shake, 0.3)
                self._say("WASD — это просто буквы. Перемешаю-ка их на 5 секунд!")

    def _update_flip(self, dt: float) -> None:
        if any(self.flip):
            self.flip_timer -= dt
            if self.flip_timer <= 0:
                self.flip = (False, False)
                self.flip_cooldown = FLIP_OFF_DURATION
        else:
            self.flip_cooldown -= dt
            if self.flip_cooldown <= 0:
                self.flip = random.choice(((False, True), (True, False), (True, True)))
                self.flip_timer = FLIP_ON_DURATION
                audio.play("glitch")
                self.room.shake = max(self.room.shake, 0.35)
                self._say("Переворачиваю экран! transform.flip(world)!")

    def _update_corruption(self, dt: float, game: GameAPI) -> None:
        self.corrupt_timer -= dt
        if self.corrupt_timer <= 0:
            self.corrupt_timer = CORRUPTION_INTERVAL
            options = [lvl for lvl in game.roster.available if lvl != self.corrupted] or list(game.roster.available)
            self.corrupted = random.choice(options)
            game.roster.set_broken(self.corrupted)
            audio.play("glitch")
            self._say(f"Подменяю веса «{ASSISTANT_NAMES[self.corrupted]}». Удачи с ним!")

    # ------------------------------------------------------------------ #
    def veto(self, assistant: Assistant, puzzle: Puzzle) -> str | None:
        """Босс не поддаётся Помощнику, которого уже «скопировал»."""
        if puzzle.pid in self.phase_ids and assistant.level == self.copied:
            return "Вайбкодер уже скопировал мои приёмы — со мной он не сработает. Позови другого Помощника!"
        return None

    def on_puzzle_solved(self, puzzle: Puzzle, game: GameAPI) -> None:
        """Решённая фазовая головоломка снимает 1/3 HP."""
        if self.defeated or puzzle.pid not in self.phase_ids:
            return
        self.hp -= 1
        self.copied = puzzle.solver_level
        self.hurt = 1.4
        self.swap = None
        self.flip = (False, False)
        self.corrupted = None
        game.roster.set_broken(None)
        self.room.shake = 0.8
        audio.play("hit")
        self.room.particles.burst(self.pos, BOSS_COLOR, 60, 380, 1.2)
        if self.hp <= 0:
            self.defeated = True
            self.bugs = []
            self._say("Я... понимаю... что пишу?.. Это... лучше, чем вайб...")
            return
        self._say(f"Ай! Минус треть HP. {PHASE_LINES[self.phase]}")
        if self.copied is not None:
            self.room.notify(
                f"Вайбкодер скопировал приёмы «{ASSISTANT_NAMES[self.copied]}» — в следующей фазе нужен другой Помощник.",
                "СИСТЕМА",
                (255, 205, 60),
            )
        self._activate_phase()
        self._spawn_bugs(game)
        self.taunt_timer = 14.0

    # ------------------------------------------------------------------ #
    def draw_ground(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        """Кольца над пропастью и баги (под стенами)."""
        center = self.pos - cam
        if not self.defeated:
            for i in range(3):
                radius = 90 + i * 22 + 6 * math.sin(t * 2 + i)
                color = gfx.hsv_color(t * 0.1 + i * 0.12, 0.8, 0.35)
                pygame.draw.circle(surface, color, center, radius, 1)
        for bug in self.bugs:
            bug.draw(surface, cam, t)

    def draw(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        """Тело босса: меняющийся многоугольник, обрывки кода и глитч-срезы."""
        scale = 1.0 if not self.defeated else max(0.0, 1.0 - self.defeat_timer / 3.2)
        if scale <= 0:
            return
        jitter = Vector2(random.uniform(-4, 4), random.uniform(-4, 4)) if self.hurt > 0 else Vector2()
        center = self.pos - cam + jitter
        hue = (t * 0.15) % 1.0
        color = gfx.hsv_color(hue, 0.75, 1.0)
        if self.hurt > 0 and int(t * 20) % 2 == 0:
            color = (255, 255, 255)
        radius = BOSS_RADIUS * (1 + 0.08 * math.sin(t * 3)) * scale
        sides = 3 + int(t * 0.8) % 6
        points = []
        for i in range(sides):
            angle = t * 0.7 + i * math.tau / sides
            r = radius * (1 + 0.16 * math.sin(t * 5 + i * 2.1) + random.uniform(-0.05, 0.05))
            points.append((center.x + math.cos(angle) * r, center.y + math.sin(angle) * r))
        gfx.draw_glow(surface, color, center, radius * 2.4, 1.1)
        gfx.draw_neon_polygon(surface, color, points, 3, 0.2)
        inner = gfx.regular_polygon(center, radius * 0.55, sides + 2, -t * 1.3)
        gfx.draw_neon_polygon(surface, gfx.hsv_color(hue + 0.5, 0.8, 1.0), inner, 2, 0)
        pygame.draw.circle(surface, (255, 255, 255), center, max(3, radius * 0.16))
        if int(t * 2) % 2 == 0:
            gfx.draw_text(surface, "_", center + Vector2(radius * 0.25, -radius * 0.05), 22, (10, 10, 10), "center", bold=True)

        if not self.defeated:
            for i, fragment in enumerate(CODE_FRAGMENTS):
                angle = t * 0.5 + i * math.tau / len(CODE_FRAGMENTS)
                orbit = radius + 52 + 8 * math.sin(t * 2 + i)
                pos = center + Vector2(math.cos(angle), math.sin(angle)) * orbit
                gfx.draw_text(surface, fragment, pos, 13, gfx.hsv_color(hue + i * 0.07, 0.6, 0.95), "center")

        # Глитч-срезы: горизонтальные полосы картинки сдвигаются вбок.
        bounds = surface.get_rect()
        for _ in range(3 if self.hurt > 0 else 1):
            if random.random() < 0.5:
                slice_rect = pygame.Rect(
                    int(center.x - radius * 1.6), int(center.y + random.uniform(-radius, radius)), int(radius * 3.2), random.randint(3, 10)
                ).clip(bounds)
                if slice_rect.width > 0 and slice_rect.height > 0:
                    piece = surface.subsurface(slice_rect).copy()
                    surface.blit(piece, slice_rect.move(random.randint(-16, 16), 0))
