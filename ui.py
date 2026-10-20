"""Интерфейс: HUD, журнал сообщений, диалоги, меню, консоль промптов, экраны.

Модуль ничего не знает об игровом цикле — ему передают готовые данные.
"""
from __future__ import annotations

import math
import random
from collections import deque
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pygame
from pygame.math import Vector2

import audio
import graphics as gfx
from settings import (
    ACCENT_COLOR,
    ASSISTANT_COLORS,
    ASSISTANT_DESCRIPTIONS,
    ASSISTANT_NAMES,
    ASSISTANT_SHAPES,
    BG_COLOR,
    BOSS_MAX_HP,
    BOSS_PHASE_NAMES,
    CONTROLS_HINT,
    DANGER_COLOR,
    DIM_TEXT_COLOR,
    LEVEL_COUNT,
    MESSAGE_DURATION,
    PLAYER_COLOR,
    SCREEN_HEIGHT,
    SCREEN_WIDTH,
    SUCCESS_COLOR,
    SYSTEM_COLOR,
    TEXT_COLOR,
    WARNING_COLOR,
    Color,
)

if TYPE_CHECKING:
    from assistant import AssistantRoster
    from puzzle import ContextTask
    from room import Room

CONFIRM_KEYS = (pygame.K_RETURN, pygame.K_KP_ENTER, pygame.K_SPACE, pygame.K_e)
UP_KEYS = (pygame.K_w, pygame.K_UP)
DOWN_KEYS = (pygame.K_s, pygame.K_DOWN)
LEFT_KEYS = (pygame.K_a, pygame.K_LEFT)
RIGHT_KEYS = (pygame.K_d, pygame.K_RIGHT)
BOSS_UI_COLOR: Color = (255, 70, 200)


def speaker_color(speaker: str) -> Color:
    """Цвет имени говорящего."""
    for level, name in ASSISTANT_NAMES.items():
        if speaker == name:
            return ASSISTANT_COLORS[level]
    return {
        "СИСТЕМА": SYSTEM_COLOR,
        "ОПЕРАТОР": PLAYER_COLOR,
        "ВАЙБКОДЕР": BOSS_UI_COLOR,
        "???": (200, 200, 255),
    }.get(speaker, TEXT_COLOR)


# --------------------------------------------------------------------------- #
# Журнал сообщений
# --------------------------------------------------------------------------- #
@dataclass
class Message:
    """Одна запись журнала."""

    speaker: str
    text: str
    color: Color
    timer: float


class MessageLog:
    """Короткие сообщения внизу экрана (реплики Помощников, системы, босса)."""

    MAX_VISIBLE = 3
    WIDTH = 820

    def __init__(self) -> None:
        self.entries: list[Message] = []

    def push(self, text: str, speaker: str = "СИСТЕМА", color: Color = SYSTEM_COLOR, duration: float = MESSAGE_DURATION) -> None:
        """Добавляет сообщение (повтор того же текста лишь продлевает его)."""
        if self.entries and self.entries[-1].text == text and self.entries[-1].speaker == speaker:
            self.entries[-1].timer = duration
            return
        self.entries.append(Message(speaker, text, color, duration))
        self.entries = self.entries[-8:]

    def clear(self) -> None:
        """Очищает журнал."""
        self.entries.clear()

    def update(self, dt: float) -> None:
        """Уменьшает таймеры и удаляет устаревшие записи."""
        for entry in self.entries:
            entry.timer -= dt
        self.entries = [e for e in self.entries if e.timer > 0]

    def draw(self, surface: pygame.Surface, bottom: int) -> None:
        """Рисует последние сообщения снизу вверх от ``bottom``."""
        y = bottom
        for entry in reversed(self.entries[-self.MAX_VISIBLE :]):
            lines = gfx.wrap_text(entry.text, 16, self.WIDTH - 32)
            height = 30 + 20 * len(lines)
            rect = pygame.Rect(0, 0, self.WIDTH, height)
            rect.midbottom = (SCREEN_WIDTH // 2, y)
            fade = min(1.0, entry.timer / 0.8)
            alpha = int(170 * fade) // 17 * 17
            gfx.draw_panel(surface, rect, gfx.scale_color(entry.color, 0.6 * fade + 0.2), alpha, 1)
            gfx.draw_text(surface, entry.speaker, (rect.x + 14, rect.y + 6), 13, gfx.scale_color(entry.color, fade), bold=True)
            for i, line in enumerate(lines):
                gfx.draw_text(surface, line, (rect.x + 14, rect.y + 24 + i * 20), 16, gfx.scale_color(TEXT_COLOR, fade))
            y = rect.top - 6


# --------------------------------------------------------------------------- #
# HUD
# --------------------------------------------------------------------------- #
class HUD:
    """Индикаторы: уровень и Помощник слева, подсказки справа, слоты 1-5 снизу, босс сверху."""

    SLOT_W = 168
    SLOT_H = 58
    SLOT_GAP = 10

    @property
    def slots_top(self) -> int:
        """Верхняя граница нижней панели Помощников."""
        return SCREEN_HEIGHT - self.SLOT_H - 14

    def draw(self, surface: pygame.Surface, level_number: int, level_title: str, roster: AssistantRoster, room: Room, t: float) -> None:
        """Рисует весь HUD."""
        self._draw_status(surface, level_number, level_title, roster, room, t)
        self._draw_controls(surface)
        self._draw_slots(surface, roster, room, t)
        if room.boss is not None:
            self._draw_boss(surface, room, t)

    def _draw_status(self, surface: pygame.Surface, level_number: int, level_title: str, roster: AssistantRoster, room: Room, t: float) -> None:
        rect = pygame.Rect(16, 16, 372, 124)
        gfx.draw_panel(surface, rect, gfx.scale_color(SYSTEM_COLOR, 0.8))
        gfx.draw_text(surface, f"УРОВЕНЬ {level_number}/{LEVEL_COUNT} · {level_title.upper()}", (30, 26), 16, ACCENT_COLOR, bold=True)
        assistant = roster.current
        if assistant.broken:
            status, status_color = "СЛОМАН", DANGER_COLOR
        elif roster.summoned:
            status, status_color = "в сети", SUCCESS_COLOR
        else:
            status, status_color = "не призван — нужен терминал", WARNING_COLOR
        label = gfx.draw_text(surface, "Помощник:", (30, 52), 16, DIM_TEXT_COLOR)
        name = gfx.draw_text(surface, assistant.name, (label.right + 8, 52), 16, assistant.color, bold=True)
        gfx.draw_text(surface, f"({status})", (name.right + 8, 53), 14, status_color)
        label = gfx.draw_text(surface, "Интеллект:", (30, 78), 16, DIM_TEXT_COLOR)
        for i in range(5):
            pip = pygame.Rect(label.right + 10 + i * 20, 81, 14, 12)
            if i < assistant.level:
                pygame.draw.rect(surface, assistant.color, pip)
            else:
                pygame.draw.rect(surface, (60, 64, 90), pip, 1)
        gfx.draw_text(surface, f"{assistant.level}/5 · {ASSISTANT_SHAPES[assistant.level]}", (label.right + 116, 78), 15, assistant.color)
        gfx.draw_text(surface, f"Задачи: {room.solved_count}/{len(room.puzzles)}", (30, 104), 16, TEXT_COLOR)
        if room.recording() and int(t * 3) % 2 == 0:
            gfx.draw_text(surface, "● REC", (rect.right - 16, 104), 16, DANGER_COLOR, "topright", bold=True)

    def _draw_controls(self, surface: pygame.Surface) -> None:
        rect = pygame.Rect(SCREEN_WIDTH - 16 - 238, 16, 238, 22 + 22 * len(CONTROLS_HINT))
        gfx.draw_panel(surface, rect, gfx.scale_color(SYSTEM_COLOR, 0.5), 170)
        for i, line in enumerate(CONTROLS_HINT):
            key, _, rest = line.partition(" — ")
            key_rect = gfx.draw_text(surface, key, (rect.x + 14, rect.y + 12 + i * 22), 15, ACCENT_COLOR, bold=True)
            gfx.draw_text(surface, f"— {rest}", (key_rect.right + 6, rect.y + 12 + i * 22), 15, DIM_TEXT_COLOR)

    def _draw_slots(self, surface: pygame.Surface, roster: AssistantRoster, room: Room, t: float) -> None:
        total = 5 * self.SLOT_W + 4 * self.SLOT_GAP
        x0 = (SCREEN_WIDTH - total) // 2
        y = self.slots_top
        copied = room.boss.copied if room.boss is not None else None
        for i, level in enumerate(range(1, 6)):
            rect = pygame.Rect(x0 + i * (self.SLOT_W + self.SLOT_GAP), y, self.SLOT_W, self.SLOT_H)
            assistant = roster.assistants[level]
            unlocked = level <= roster.unlocked
            available = level in roster.available
            active = level == roster.selected
            color = assistant.color
            if active:
                border, k = color, 1.0
                gfx.draw_glow(surface, color, rect.center, 90, 0.35)
            elif available:
                border, k = gfx.scale_color(color, 0.5), 0.8
            else:
                border, k = (48, 50, 70), 0.3
            gfx.draw_panel(surface, rect, border, 200, 3 if active else 1)
            key_box = pygame.Rect(rect.x + 6, rect.y + 6, 18, 18)
            pygame.draw.rect(surface, border, key_box, 1)
            gfx.draw_text(surface, str(level), key_box.center, 13, gfx.scale_color(TEXT_COLOR, k), "center", bold=True)
            icon = (rect.x + 40, rect.y + 31)
            if unlocked:
                gfx.draw_assistant_icon(surface, level, icon, 10, gfx.scale_color(color, k), t * 0.8 if active else 0.0)
                gfx.draw_text(surface, assistant.name, (rect.x + 62, rect.y + 10), 15, gfx.scale_color(color, k), bold=active)
            else:
                gfx.draw_text(surface, "?", icon, 22, (70, 72, 96), "center", bold=True)
                gfx.draw_text(surface, "???", (rect.x + 62, rect.y + 10), 15, (70, 72, 96))
            if not unlocked:
                state, state_color = "закрыт", (70, 72, 96)
            elif assistant.broken:
                state, state_color = "ERR: сломан", DANGER_COLOR
            elif copied == level:
                state, state_color = "скопирован", WARNING_COLOR
            elif active and roster.summoned:
                state, state_color = "активен", SUCCESS_COLOR
            elif active:
                state, state_color = "выбран", WARNING_COLOR
            elif available:
                state, state_color = "доступен", DIM_TEXT_COLOR
            else:
                state, state_color = "недоступен", (70, 72, 96)
            gfx.draw_text(surface, state, (rect.x + 62, rect.y + 32), 13, state_color)
            if assistant.broken and int(t * 10) % 2 == 0:
                for _ in range(2):
                    bar = pygame.Rect(rect.x + random.randint(0, rect.width - 40), rect.y + random.randint(0, rect.height - 3), 40, 2)
                    surface.fill(DANGER_COLOR, bar)

    def _draw_boss(self, surface: pygame.Surface, room: Room, t: float) -> None:
        boss = room.boss
        assert boss is not None
        rect = pygame.Rect(0, 0, 440, 66)
        rect.midtop = (SCREEN_WIDTH // 2, 14)
        color = gfx.hsv_color(t * 0.15, 0.7, 1.0)
        gfx.draw_panel(surface, rect, color)
        gfx.draw_text(surface, "ВАЙБКОДЕР", (rect.x + 14, rect.y + 8), 16, color, bold=True)
        phase_name = "ПЕРЕОБУЧЕН" if boss.defeated else BOSS_PHASE_NAMES[boss.phase]
        gfx.draw_text(surface, phase_name, (rect.right - 14, rect.y + 10), 13, TEXT_COLOR, "topright")
        seg_w = (rect.width - 28 - 2 * 8) // BOSS_MAX_HP
        for i in range(BOSS_MAX_HP):
            seg = pygame.Rect(rect.x + 14 + i * (seg_w + 8), rect.y + 36, seg_w, 16)
            if i < boss.hp:
                pygame.draw.rect(surface, gfx.scale_color(color, 0.85), seg)
            pygame.draw.rect(surface, color, seg, 1)
        status = boss.status_text()
        if status:
            gfx.draw_text(surface, status, (SCREEN_WIDTH // 2, rect.bottom + 8), 15, WARNING_COLOR, "midtop", bold=True)


def draw_interaction_label(surface: pygame.Surface, pos: Vector2, label: str, color: Color) -> None:
    """Подсказка «[E] действие» над объектом."""
    text = gfx.render_text(f"[E] {label}", 15, TEXT_COLOR, True)
    rect = text.get_rect()
    rect.inflate_ip(18, 10)
    rect.midbottom = (int(pos.x), int(pos.y) - 30)
    rect.clamp_ip(surface.get_rect().inflate(-8, -8))
    gfx.draw_panel(surface, rect, color, 210, 1)
    surface.blit(text, text.get_rect(center=rect.center))


# --------------------------------------------------------------------------- #
# Диалоги
# --------------------------------------------------------------------------- #
class DialogBox:
    """Сюжетные реплики с эффектом печатной машинки. Пока диалог открыт, мир стоит."""

    CHARS_PER_SECOND = 60

    def __init__(self) -> None:
        self.queue: deque[tuple[str, str]] = deque()
        self.current: tuple[str, str] | None = None
        self.shown = 0.0

    @property
    def active(self) -> bool:
        """Открыт ли диалог."""
        return self.current is not None

    def start(self, lines: list[tuple[str, str]]) -> None:
        """Ставит реплики в очередь."""
        self.queue.extend(lines)
        if self.current is None:
            self._next()

    def _next(self) -> None:
        self.current = self.queue.popleft() if self.queue else None
        self.shown = 0.0

    def handle_key(self, key: int) -> None:
        """E/Enter/Пробел — дописать или следующая реплика; Esc — пропустить всё."""
        if self.current is None:
            return
        if key == pygame.K_ESCAPE:
            self.queue.clear()
            self.current = None
            return
        if key in CONFIRM_KEYS:
            if self.shown < len(self.current[1]):
                self.shown = len(self.current[1])
            else:
                audio.play("blip")
                self._next()

    def update(self, dt: float) -> None:
        """Печатает текст."""
        if self.current is not None:
            self.shown = min(len(self.current[1]), self.shown + dt * self.CHARS_PER_SECOND)

    def draw(self, surface: pygame.Surface, t: float) -> None:
        """Панель диалога внизу экрана."""
        if self.current is None:
            return
        speaker, text = self.current
        color = speaker_color(speaker)
        rect = pygame.Rect(0, 0, 940, 168)
        rect.midbottom = (SCREEN_WIDTH // 2, SCREEN_HEIGHT - 86)
        gfx.draw_panel(surface, rect, color, 235)
        gfx.draw_glow(surface, color, (rect.x + 30, rect.y + 22), 30, 0.6)
        gfx.draw_text(surface, speaker, (rect.x + 22, rect.y + 12), 18, color, bold=True)
        visible = text[: int(self.shown)]
        for i, line in enumerate(gfx.wrap_text(visible, 19, rect.width - 48)):
            gfx.draw_text(surface, line, (rect.x + 22, rect.y + 44 + i * 26), 19, TEXT_COLOR)
        if self.shown >= len(text) and int(t * 2) % 2 == 0:
            hint = "E / Enter — далее · Esc — пропустить"
            gfx.draw_text(surface, hint, (rect.right - 16, rect.bottom - 10), 13, DIM_TEXT_COLOR, "bottomright")


# --------------------------------------------------------------------------- #
# Меню
# --------------------------------------------------------------------------- #
class Menu:
    """Вертикальное меню с выбором клавишами W/S и Enter."""

    def __init__(self, title: str, options: list[tuple[str, str]], subtitle: str = "") -> None:
        self.title = title
        self.options = options
        self.subtitle = subtitle
        self.index = 0

    def handle_key(self, key: int) -> str | None:
        """Возвращает ключ выбранного пункта или ``None``."""
        if key in UP_KEYS:
            self.index = (self.index - 1) % len(self.options)
            audio.play("blip")
        elif key in DOWN_KEYS:
            self.index = (self.index + 1) % len(self.options)
            audio.play("blip")
        elif key in CONFIRM_KEYS:
            audio.play("select")
            return self.options[self.index][0]
        return None

    def draw(self, surface: pygame.Surface, t: float, top: int = 360, title_size: int = 44) -> None:
        """Рисует заголовок и пункты."""
        if self.title:
            gfx.draw_text(surface, self.title, (SCREEN_WIDTH // 2, top - 110), title_size, ACCENT_COLOR, "center", bold=True, glow=True)
        if self.subtitle:
            gfx.draw_text(surface, self.subtitle, (SCREEN_WIDTH // 2, top - 66), 17, DIM_TEXT_COLOR, "center")
        for i, (_, label) in enumerate(self.options):
            y = top + i * 46
            if i == self.index:
                pulse = 0.75 + 0.25 * math.sin(t * 6)
                rect = pygame.Rect(0, 0, 420, 38)
                rect.center = (SCREEN_WIDTH // 2, y)
                gfx.draw_panel(surface, rect, gfx.scale_color(ACCENT_COLOR, pulse), 160)
                gfx.draw_text(surface, f">  {label}  <", (SCREEN_WIDTH // 2, y), 22, ACCENT_COLOR, "center", bold=True)
            else:
                gfx.draw_text(surface, label, (SCREEN_WIDTH // 2, y), 21, DIM_TEXT_COLOR, "center")


# --------------------------------------------------------------------------- #
# Консоль контекста (меню формулировки промпта)
# --------------------------------------------------------------------------- #
class PromptMenu:
    """Меню, в котором Оператор собирает промпт из частей: действие + объект + цель."""

    def __init__(self, puzzle: ContextTask) -> None:
        self.puzzle = puzzle
        self.row = 0
        self.choice = [0] * len(puzzle.slots)

    @property
    def prompt(self) -> tuple[str, ...]:
        """Собранный промпт."""
        return tuple(options[self.choice[i]] for i, (_, options) in enumerate(self.puzzle.slots))

    def handle_key(self, key: int) -> str | None:
        """Возвращает ``"submit"``, ``"cancel"`` или ``None``."""
        slots = self.puzzle.slots
        if key == pygame.K_ESCAPE:
            return "cancel"
        if key in (pygame.K_RETURN, pygame.K_KP_ENTER):
            return "submit"
        if key in UP_KEYS:
            self.row = (self.row - 1) % len(slots)
            audio.play("blip")
        elif key in DOWN_KEYS:
            self.row = (self.row + 1) % len(slots)
            audio.play("blip")
        elif key in LEFT_KEYS:
            self.choice[self.row] = (self.choice[self.row] - 1) % len(slots[self.row][1])
            audio.play("mark")
        elif key in RIGHT_KEYS:
            self.choice[self.row] = (self.choice[self.row] + 1) % len(slots[self.row][1])
            audio.play("mark")
        return None

    def draw(self, surface: pygame.Surface, t: float, assistant_name: str, assistant_color: Color) -> None:
        """Рисует консоль поверх игры."""
        draw_dim(surface, 190)
        rect = pygame.Rect(0, 0, 860, 470)
        rect.center = (SCREEN_WIDTH // 2, SCREEN_HEIGHT // 2)
        gfx.draw_panel(surface, rect, SYSTEM_COLOR, 250)
        gfx.draw_text(surface, "КОНСОЛЬ КОНТЕКСТА", (rect.x + 28, rect.y + 20), 24, ACCENT_COLOR, bold=True)
        gfx.draw_text(surface, "Сформулируй задачу так, чтобы Помощник понял, ЧТО сделать и ЗАЧЕМ.", (rect.x + 28, rect.y + 54), 15, DIM_TEXT_COLOR)
        y = rect.y + 86
        for line in gfx.wrap_text(self.puzzle.description, 17, rect.width - 56):
            gfx.draw_text(surface, line, (rect.x + 28, y), 17, TEXT_COLOR)
            y += 24
        y += 14
        for i, (name, options) in enumerate(self.puzzle.slots):
            selected = i == self.row
            row_rect = pygame.Rect(rect.x + 24, y, rect.width - 48, 44)
            if selected:
                gfx.draw_panel(surface, row_rect, ACCENT_COLOR, 120, 1)
            gfx.draw_text(surface, name.upper(), (row_rect.x + 16, row_rect.centery), 15, ACCENT_COLOR if selected else DIM_TEXT_COLOR, "midleft", bold=True)
            value = options[self.choice[i]]
            arrows_color = ACCENT_COLOR if selected else (70, 72, 96)
            gfx.draw_text(surface, "<", (row_rect.x + 220, row_rect.centery), 20, arrows_color, "center", bold=True)
            gfx.draw_text(surface, value, (row_rect.x + 450, row_rect.centery), 19, TEXT_COLOR if selected else DIM_TEXT_COLOR, "center", bold=selected)
            gfx.draw_text(surface, ">", (row_rect.x + 680, row_rect.centery), 20, arrows_color, "center", bold=True)
            y += 52
        preview = self.puzzle.format_prompt(self.prompt)
        y += 10
        gfx.draw_text(surface, "ПРОМПТ:", (rect.x + 28, y), 15, DIM_TEXT_COLOR, bold=True)
        cursor = "_" if int(t * 2) % 2 == 0 else " "
        gfx.draw_text(surface, f"«{preview}»{cursor}", (rect.x + 110, y - 2), 19, PLAYER_COLOR, bold=True)
        footer = f"W/S — строка · A/D — вариант · Enter — отправить ({assistant_name}) · Esc — отмена"
        gfx.draw_text(surface, footer, (rect.centerx, rect.bottom - 22), 14, assistant_color, "center")


# --------------------------------------------------------------------------- #
# Фон и экраны
# --------------------------------------------------------------------------- #
def draw_menu_background(surface: pygame.Surface, t: float) -> None:
    """Неоновый фон меню: сетка и плывущие фигуры Помощников."""
    surface.fill(BG_COLOR)
    offset = (t * 18) % 48
    for x in range(-48, SCREEN_WIDTH + 48, 48):
        pygame.draw.line(surface, (22, 24, 40), (x + offset, 0), (x + offset, SCREEN_HEIGHT))
    for y in range(-48, SCREEN_HEIGHT + 48, 48):
        pygame.draw.line(surface, (22, 24, 40), (0, y + offset), (SCREEN_WIDTH, y + offset))
    for i in range(15):
        level = i % 5 + 1
        speed = 14 + (i % 4) * 9
        x = (i * 173 + t * speed) % (SCREEN_WIDTH + 120) - 60
        y = (i * 97 + 40) % SCREEN_HEIGHT + math.sin(t * 0.7 + i) * 26
        color = gfx.scale_color(ASSISTANT_COLORS[level], 0.45)
        gfx.draw_glow(surface, color, (x, y), 40, 0.5)
        gfx.draw_assistant_icon(surface, level, (x, y), 10 + (i % 3) * 4, color, t * (0.3 + 0.07 * i))


def draw_title(surface: pygame.Surface, t: float, center: tuple[int, int]) -> None:
    """Заголовок «ПРОМПТ» с хроматической аберрацией и глитчем."""
    text = "ПРОМПТ"
    shift = 3 + (6 if random.random() < 0.06 else 0)
    gfx.draw_glow(surface, ACCENT_COLOR, center, 220, 0.5)
    for color, dx in (((255, 0, 110), -shift), ((0, 200, 255), shift)):
        # Чёрный фон вместо прозрачного: при аддитивном смешивании он ничего не добавляет.
        surf = gfx.get_font(104, True).render(text, True, color, (0, 0, 0))
        surface.blit(surf, surf.get_rect(center=(center[0] + dx, center[1])), special_flags=pygame.BLEND_RGB_ADD)
    gfx.draw_text(surface, text, center, 104, (235, 240, 255), "center", bold=True)
    if random.random() < 0.15:
        rect = pygame.Rect(center[0] - 260, center[1] + random.randint(-40, 40), 520, random.randint(3, 9)).clip(surface.get_rect())
        piece = surface.subsurface(rect).copy()
        surface.blit(piece, rect.move(random.randint(-20, 20), 0))


def draw_dim(surface: pygame.Surface, alpha: int = 170) -> None:
    """Затемняет экран (для паузы и окон)."""
    shade = pygame.Surface((SCREEN_WIDTH, SCREEN_HEIGHT), pygame.SRCALPHA)
    shade.fill((0, 0, 0, alpha))
    surface.blit(shade, (0, 0))


def draw_level_banner(surface: pygame.Surface, number: int, title: str, subtitle: str, strength: float) -> None:
    """Название уровня в начале сцены (плавно гаснет)."""
    if strength <= 0:
        return
    color = gfx.scale_color(ACCENT_COLOR, min(1.0, strength))
    y = 200
    gfx.draw_text(surface, f"УРОВЕНЬ {number}", (SCREEN_WIDTH // 2, y), 22, gfx.scale_color(DIM_TEXT_COLOR, min(1.0, strength)), "center", bold=True)
    gfx.draw_text(surface, title.upper(), (SCREEN_WIDTH // 2, y + 42), 46, color, "center", bold=True, glow=True)
    gfx.draw_text(surface, subtitle, (SCREEN_WIDTH // 2, y + 86), 18, gfx.scale_color(TEXT_COLOR, min(1.0, strength)), "center")


def draw_red_flash(surface: pygame.Surface, strength: float) -> None:
    """Красная вспышка при поимке Оператора."""
    if strength <= 0:
        return
    flash = pygame.Surface((SCREEN_WIDTH, SCREEN_HEIGHT), pygame.SRCALPHA)
    flash.fill((255, 0, 40, int(110 * min(1.0, strength))))
    surface.blit(flash, (0, 0))


def build_crt_overlay() -> pygame.Surface:
    """Сканлайны и виньетка — один раз при запуске."""
    overlay = pygame.Surface((SCREEN_WIDTH, SCREEN_HEIGHT), pygame.SRCALPHA)
    for y in range(0, SCREEN_HEIGHT, 3):
        pygame.draw.line(overlay, (0, 0, 0, 34), (0, y), (SCREEN_WIDTH, y))
    rings = 30
    for i in range(rings):  # i = 0 — внешнее кольцо, самое тёмное
        alpha = int(90 * (1 - i / rings) ** 2)
        pygame.draw.rect(overlay, (0, 0, 0, alpha), pygame.Rect(i * 4, i * 3, SCREEN_WIDTH - i * 8, SCREEN_HEIGHT - i * 6), 4)
    return overlay


HOW_TO_PLAY = (
    ("Цель", "Пройти пять комнат Песочницы и переобучить босса-Вайбкодера."),
    ("Ты — проекция", "Оператор не двигает предметы и не жмёт кнопки. Ты формулируешь задачи — Помощники их выполняют."),
    ("Терминал", "Подойди к терминалу и нажми E, чтобы призвать Помощника. Клавиши 1-5 меняют активного Помощника."),
    ("Уровни интеллекта", "1 Пиксель — ящики и кнопки · 2 Эхо — маршруты · 3 Вектор — связи · 4 Трансформер — контекст · 5 Оракул — всё (иногда капризничает)."),
    ("Стелс", "Стражи-глитчи видят конусом. Попадёшь в конус — откат к началу уровня. Повреждённые сектора видны только вблизи."),
    ("Промпты", "У консоли контекста собери промпт: действие + объект + цель. Без правильной цели Трансформер откатит изменения."),
)


def draw_how_to(surface: pygame.Surface, t: float) -> None:
    """Экран «Как играть»."""
    draw_menu_background(surface, t)
    draw_dim(surface, 150)
    gfx.draw_text(surface, "КАК ИГРАТЬ", (SCREEN_WIDTH // 2, 70), 40, ACCENT_COLOR, "center", bold=True, glow=True)
    y = 130
    for title, body in HOW_TO_PLAY:
        gfx.draw_text(surface, title.upper(), (140, y), 17, WARNING_COLOR, bold=True)
        for line in gfx.wrap_text(body, 17, 1000):
            y += 24
            gfx.draw_text(surface, line, (140, y), 17, TEXT_COLOR)
        y += 38
    gfx.draw_text(surface, "Esc / Enter — назад", (SCREEN_WIDTH // 2, SCREEN_HEIGHT - 40), 16, DIM_TEXT_COLOR, "center")


class UpgradeScreen:
    """Экран апгрейда: новый Помощник разблокирован."""

    def __init__(self, level: int, text: str) -> None:
        self.level = level
        self.text = text
        self.timer = 0.0

    def update(self, dt: float) -> None:
        """Анимация."""
        self.timer += dt

    def handle_key(self, key: int) -> bool:
        """``True`` — можно продолжать."""
        return self.timer > 0.8 and key in CONFIRM_KEYS

    def draw(self, surface: pygame.Surface, t: float) -> None:
        """Рисует экран апгрейда."""
        draw_menu_background(surface, t)
        draw_dim(surface, 140)
        color = ASSISTANT_COLORS[self.level]
        grow = min(1.0, self.timer / 0.6)
        center = (SCREEN_WIDTH // 2, 250)
        gfx.draw_text(surface, "АПГРЕЙД ПОМОЩНИКА", (SCREEN_WIDTH // 2, 90), 34, ACCENT_COLOR, "center", bold=True, glow=True)
        for i in range(3):
            radius = (60 + i * 26 + (self.timer * 60) % 26) * grow
            pygame.draw.circle(surface, gfx.scale_color(color, 0.5 - i * 0.12), center, radius, 1)
        gfx.draw_glow(surface, color, center, 150 * grow, 1.2)
        gfx.draw_assistant_icon(surface, self.level, center, 46 * grow, color, t * 0.9, 3)
        gfx.draw_text(surface, ASSISTANT_NAMES[self.level], (SCREEN_WIDTH // 2, 380), 42, color, "center", bold=True, glow=True)
        gfx.draw_text(surface, f"Интеллект: {self.level}/5 · форма: {ASSISTANT_SHAPES[self.level]}", (SCREEN_WIDTH // 2, 426), 18, TEXT_COLOR, "center")
        gfx.draw_text(surface, ASSISTANT_DESCRIPTIONS[self.level], (SCREEN_WIDTH // 2, 460), 18, DIM_TEXT_COLOR, "center")
        y = 510
        for line in gfx.wrap_text(self.text, 17, 900):
            gfx.draw_text(surface, line, (SCREEN_WIDTH // 2, y), 17, WARNING_COLOR, "center")
            y += 24
        if self.timer > 0.8 and int(t * 2) % 2 == 0:
            gfx.draw_text(surface, "Enter — следующий уровень", (SCREEN_WIDTH // 2, SCREEN_HEIGHT - 50), 17, ACCENT_COLOR, "center", bold=True)


class VictoryScreen:
    """Финал: главная фраза печатается, затем идут титры."""

    TEXT_SPEED = 26
    CREDIT_SPEED = 46

    def __init__(self, final_text: str, credits: list[str]) -> None:
        self.final_text = final_text
        self.credits = credits
        self.timer = 0.0
        self.scroll = 0.0

    @property
    def text_done(self) -> bool:
        """Допечатана ли финальная фраза."""
        return self.timer * self.TEXT_SPEED >= len(self.final_text)

    def update(self, dt: float) -> None:
        """Печать текста и прокрутка титров."""
        self.timer += dt
        if self.text_done and self.timer > len(self.final_text) / self.TEXT_SPEED + 2.5:
            self.scroll += dt * self.CREDIT_SPEED

    @property
    def finished(self) -> bool:
        """Титры закончились."""
        return self.scroll > SCREEN_HEIGHT + len(self.credits) * 34

    def handle_key(self, key: int) -> bool:
        """``True`` — вернуться в главное меню."""
        if key not in CONFIRM_KEYS and key != pygame.K_ESCAPE:
            return False
        if not self.text_done:
            self.timer = len(self.final_text) / self.TEXT_SPEED
            return False
        if self.scroll <= 0:
            self.timer = len(self.final_text) / self.TEXT_SPEED + 2.5
            self.scroll = 1.0
            return False
        return True

    def draw(self, surface: pygame.Surface, t: float) -> None:
        """Рисует финальный экран."""
        draw_menu_background(surface, t)
        draw_dim(surface, 120)
        shown = self.final_text[: int(self.timer * self.TEXT_SPEED)]
        text_y = 300 - min(self.scroll, 220)
        for i, line in enumerate(gfx.wrap_text(shown, 32, 1000, True)):
            gfx.draw_text(surface, line, (SCREEN_WIDTH // 2, text_y + i * 44), 32, ACCENT_COLOR, "center", bold=True, glow=True)
        if self.scroll > 0:
            y = SCREEN_HEIGHT + 20 - self.scroll
            for line in self.credits:
                if -40 < y < SCREEN_HEIGHT + 40:
                    big = line.isupper() and len(line) < 30
                    gfx.draw_text(surface, line, (SCREEN_WIDTH // 2, y), 24 if big else 18, WARNING_COLOR if big else TEXT_COLOR, "center", bold=big)
                y += 34
        hint = "Enter — в главное меню" if self.scroll > 0 else "Enter — продолжить"
        if self.text_done:
            gfx.draw_text(surface, hint, (SCREEN_WIDTH - 24, SCREEN_HEIGHT - 24), 14, DIM_TEXT_COLOR, "bottomright")
