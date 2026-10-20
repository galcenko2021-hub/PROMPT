"""«ПРОМПТ» — точка входа: игровой цикл, меню, пауза и переходы между уровнями.

Запуск: ``python main.py``
"""
from __future__ import annotations

import math
import random

import pygame
from pygame.math import Vector2

import audio
import graphics as gfx
import ui
from assistant import Assistant, AssistantRoster
from levels import CREDITS, FINAL_TEXT, INTRO_STORY, LEVELS
from player import Player
from puzzle import ContextTask, Interactable
from room import Room
from settings import (
    DANGER_COLOR,
    DIM_TEXT_COLOR,
    FPS,
    MAX_FRAME_TIME,
    SCREEN_HEIGHT,
    SCREEN_WIDTH,
    SYSTEM_COLOR,
    TITLE,
    Color,
)

# Физические позиции W/A/S/D/E (SDL scancode) — чтобы игра работала и в русской раскладке.
SCANCODE_KEYS = {26: pygame.K_w, 4: pygame.K_a, 22: pygame.K_s, 7: pygame.K_d, 8: pygame.K_e}
ASSISTANT_KEYS = {pygame.K_1: 1, pygame.K_2: 2, pygame.K_3: 3, pygame.K_4: 4, pygame.K_5: 5}


class Game:
    """Состояние игры и главный цикл."""

    def __init__(self) -> None:
        pygame.init()
        audio.init()
        pygame.display.set_caption(TITLE)
        self.screen = pygame.display.set_mode((SCREEN_WIDTH, SCREEN_HEIGHT))
        self.clock = pygame.time.Clock()
        self.world = pygame.Surface((SCREEN_WIDTH, SCREEN_HEIGHT))
        self.crt = ui.build_crt_overlay()
        self.running = True
        self.time = 0.0
        self.state = "menu"

        self.main_menu = ui.Menu(
            "",
            [("new", "Новая игра"), ("levels", "Выбор уровня"), ("howto", "Как играть"), ("quit", "Выход")],
        )
        self.level_menu = ui.Menu(
            "ВЫБОР УРОВНЯ",
            [(str(i), f"{i + 1}. {level['title']}") for i, level in enumerate(LEVELS)] + [("back", "Назад")],
            "Каждый уровень можно запустить отдельно",
        )
        self.pause_menu = ui.Menu(
            "ПАУЗА",
            [("resume", "Продолжить"), ("restart", "Перезапустить уровень"), ("menu", "Главное меню"), ("quit", "Выход из игры")],
        )
        self.messages = ui.MessageLog()
        self.dialog = ui.DialogBox()
        self.hud = ui.HUD()
        self.roster = AssistantRoster(self._assistant_said)
        self.player = Player((0, 0))
        self.room: Room = Room(LEVELS[0])
        self.level_index = 0
        self.prompt_menu: ui.PromptMenu | None = None
        self.upgrade_screen: ui.UpgradeScreen | None = None
        self.victory_screen: ui.VictoryScreen | None = None
        self.banner = 0.0
        self.flash = 0.0
        self.focus: Interactable | None = None

    # ------------------------------------------------------------------ #
    # API для головоломок, комнат и босса
    # ------------------------------------------------------------------ #
    def notify(self, text: str, speaker: str = "СИСТЕМА", color: Color = SYSTEM_COLOR) -> None:
        """Сообщение в журнал внизу экрана."""
        self.messages.push(text, speaker, color)

    def _assistant_said(self, assistant: Assistant, text: str, color: Color) -> None:
        self.messages.push(text, assistant.name, color)

    def open_prompt_menu(self, puzzle: ContextTask) -> None:
        """Открывает консоль контекста для формулировки промпта."""
        self.prompt_menu = ui.PromptMenu(puzzle)
        self.state = "prompt"

    def player_caught(self, reason: str, respawn_at: Vector2 | None = None, force: bool = False) -> None:
        """Оператора заметили или он наступил на повреждённый сектор — откат."""
        if self.player.invulnerable > 0 and not force:
            return
        audio.play("detected")
        self.flash = 1.0
        self.room.shake = max(self.room.shake, 0.4)
        self.notify(reason, "СИСТЕМА", DANGER_COLOR)
        self.room.particles.burst(self.player.pos, DANGER_COLOR, 30, 220, 0.7)
        target = respawn_at if respawn_at is not None else self.room.spawn
        self.player.respawn(target)
        self.room.on_player_caught()
        assistant = self.roster.active
        if assistant is not None and not assistant.busy:
            assistant.teleport_near(target)

    def on_victory(self) -> None:
        """Финальный экран и титры."""
        audio.play("victory")
        self.victory_screen = ui.VictoryScreen(FINAL_TEXT, CREDITS)
        self.state = "victory"

    # ------------------------------------------------------------------ #
    # Уровни
    # ------------------------------------------------------------------ #
    def load_level(self, index: int, with_story: bool = False) -> None:
        """Загружает уровень по индексу и показывает вступительные реплики."""
        self.level_index = index
        data = LEVELS[index]
        self.room = Room(data)
        self.room.notify = self.notify
        self.player.respawn(self.room.spawn)
        self.player.invulnerable = 0.0
        self.roster.setup_level(self.room, data["available"], data["unlocked"])
        self.messages.clear()
        self.prompt_menu = None
        self.focus = None
        self.dialog = ui.DialogBox()
        self.dialog.start((INTRO_STORY if with_story else []) + list(data["intro"]))
        self.banner = 3.5
        self.flash = 0.0
        self.state = "play"

    def complete_level(self) -> None:
        """Оператор вошёл в открытую дверь."""
        data = LEVELS[self.level_index]
        if data.get("upgrade"):
            audio.play("upgrade")
            self.upgrade_screen = ui.UpgradeScreen(data["upgrade"], data["upgrade_text"])
            self.state = "upgrade"
        elif self.level_index + 1 < len(LEVELS):
            self.load_level(self.level_index + 1)
        else:
            self.on_victory()

    # ------------------------------------------------------------------ #
    # События
    # ------------------------------------------------------------------ #
    @staticmethod
    def _normalize_key(event: pygame.event.Event) -> int:
        scancode = getattr(event, "scancode", -1)
        return SCANCODE_KEYS.get(scancode, event.key)

    def handle_event(self, event: pygame.event.Event) -> None:
        """Обрабатывает событие в зависимости от состояния."""
        if event.type == pygame.QUIT:
            self.running = False
            return
        if event.type != pygame.KEYDOWN:
            return
        key = self._normalize_key(event)
        handler = getattr(self, f"_key_{self.state}", None)
        if handler is not None:
            handler(key)

    def _key_menu(self, key: int) -> None:
        choice = self.main_menu.handle_key(key)
        if choice == "new":
            self.load_level(0, with_story=True)
        elif choice == "levels":
            self.state = "levels"
        elif choice == "howto":
            self.state = "howto"
        elif choice == "quit":
            self.running = False

    def _key_levels(self, key: int) -> None:
        if key == pygame.K_ESCAPE:
            self.state = "menu"
            return
        choice = self.level_menu.handle_key(key)
        if choice == "back":
            self.state = "menu"
        elif choice is not None:
            self.load_level(int(choice))

    def _key_howto(self, key: int) -> None:
        if key in (pygame.K_ESCAPE, *ui.CONFIRM_KEYS):
            self.state = "menu"

    def _key_play(self, key: int) -> None:
        if self.dialog.active:
            self.dialog.handle_key(key)
            return
        if key == pygame.K_ESCAPE:
            self.pause_menu.index = 0
            self.state = "pause"
        elif key == pygame.K_e:
            focus = self._find_focus()
            if focus is not None:
                focus.action(self)
        elif key in ASSISTANT_KEYS:
            ok, message = self.roster.select(ASSISTANT_KEYS[key])
            if message:
                color = self.roster.current.color if ok else DANGER_COLOR
                self.notify(message, "СИСТЕМА", color)
            if not ok and message:
                audio.play("error")

    def _key_pause(self, key: int) -> None:
        if key == pygame.K_ESCAPE:
            self.state = "play"
            return
        choice = self.pause_menu.handle_key(key)
        if choice == "resume":
            self.state = "play"
        elif choice == "restart":
            self.load_level(self.level_index)
        elif choice == "menu":
            self.state = "menu"
        elif choice == "quit":
            self.running = False

    def _key_prompt(self, key: int) -> None:
        if self.prompt_menu is None:
            self.state = "play"
            return
        result = self.prompt_menu.handle_key(key)
        if result == "submit":
            menu = self.prompt_menu
            self.prompt_menu = None
            self.state = "play"
            menu.puzzle.submit(menu.prompt, self)
        elif result == "cancel":
            self.prompt_menu = None
            self.state = "play"

    def _key_upgrade(self, key: int) -> None:
        if self.upgrade_screen is not None and self.upgrade_screen.handle_key(key):
            self.upgrade_screen = None
            self.load_level(self.level_index + 1)

    def _key_victory(self, key: int) -> None:
        if self.victory_screen is not None and self.victory_screen.handle_key(key):
            self.victory_screen = None
            self.state = "menu"

    # ------------------------------------------------------------------ #
    # Логика
    # ------------------------------------------------------------------ #
    def _find_focus(self) -> Interactable | None:
        best: Interactable | None = None
        best_distance = math.inf
        for item in self.room.interactables(self):
            distance = item.pos.distance_to(self.player.pos)
            if distance <= item.radius and distance < best_distance:
                best, best_distance = item, distance
        return best

    def update(self, dt: float) -> None:
        """Обновление текущего состояния."""
        if self.state == "play":
            self._update_play(dt)
        elif self.state == "upgrade" and self.upgrade_screen is not None:
            self.upgrade_screen.update(dt)
        elif self.state == "victory" and self.victory_screen is not None:
            self.victory_screen.update(dt)
            if self.victory_screen.finished:
                self.victory_screen = None
                self.state = "menu"

    def _update_play(self, dt: float) -> None:
        self.messages.update(dt)
        self.banner = max(0.0, self.banner - dt)
        self.flash = max(0.0, self.flash - dt * 2.5)
        if self.dialog.active:
            self.dialog.update(dt)
            self.focus = None
            return
        room = self.room
        room.update(dt, self)
        if self.state != "play":  # босс мог завершить игру
            return
        scramble = room.boss.control_scramble if room.boss is not None else None
        self.player.update(dt, pygame.key.get_pressed(), room, scramble)
        self.roster.update(dt, self.player)
        self.focus = self._find_focus()
        if room.player_on_exit(self.player.pos):
            self.complete_level()

    # ------------------------------------------------------------------ #
    # Отрисовка
    # ------------------------------------------------------------------ #
    def _camera(self) -> Vector2:
        cam = self.player.pos - Vector2(SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2)
        if self.room.shake > 0:
            power = 10 * self.room.shake
            cam += Vector2(random.uniform(-power, power), random.uniform(-power, power))
        return Vector2(round(cam.x), round(cam.y))

    def _draw_world(self) -> None:
        room = self.room
        cam = self._camera()
        room.draw(self.world, cam, self.time, [self.roster.draw, self.player.draw])
        flip_x, flip_y = room.boss.flip_flags if room.boss is not None else (False, False)
        if flip_x or flip_y:
            self.screen.blit(pygame.transform.flip(self.world, flip_x, flip_y), (0, 0))
        else:
            self.screen.blit(self.world, (0, 0))

        if self.state == "play" and not self.dialog.active and self.focus is not None:
            pos = self.focus.pos - cam
            if flip_x:
                pos.x = SCREEN_WIDTH - pos.x
            if flip_y:
                pos.y = SCREEN_HEIGHT - pos.y
            ui.draw_interaction_label(self.screen, pos, self.focus.label, self.focus.color)

        level = LEVELS[self.level_index]
        self.hud.draw(self.screen, self.level_index + 1, level["title"], self.roster, room, self.time)
        self.messages.draw(self.screen, self.hud.slots_top - 10)
        ui.draw_level_banner(self.screen, self.level_index + 1, level["title"], level["subtitle"], min(1.0, self.banner))
        self.dialog.draw(self.screen, self.time)
        ui.draw_red_flash(self.screen, self.flash)

    def draw(self) -> None:
        """Отрисовка текущего состояния."""
        screen = self.screen
        t = self.time
        if self.state == "menu":
            ui.draw_menu_background(screen, t)
            ui.draw_title(screen, t, (SCREEN_WIDTH // 2, 170))
            gfx.draw_text(screen, "головоломка о правильных формулировках", (SCREEN_WIDTH // 2, 248), 18, DIM_TEXT_COLOR, "center")
            self.main_menu.draw(screen, t, top=370)
            gfx.draw_text(screen, "W/S — выбор · Enter — подтвердить", (SCREEN_WIDTH // 2, SCREEN_HEIGHT - 36), 15, DIM_TEXT_COLOR, "center")
        elif self.state == "levels":
            ui.draw_menu_background(screen, t)
            ui.draw_dim(screen, 110)
            self.level_menu.draw(screen, t, top=250)
        elif self.state == "howto":
            ui.draw_how_to(screen, t)
        elif self.state in ("play", "pause", "prompt"):
            self._draw_world()
            if self.state == "pause":
                ui.draw_dim(screen, 170)
                self.pause_menu.draw(screen, t, top=330)
            elif self.state == "prompt" and self.prompt_menu is not None:
                current = self.roster.current
                name = current.name if self.roster.summoned else "Помощник не призван"
                self.prompt_menu.draw(screen, t, name, current.color)
        elif self.state == "upgrade" and self.upgrade_screen is not None:
            self.upgrade_screen.draw(screen, t)
        elif self.state == "victory" and self.victory_screen is not None:
            self.victory_screen.draw(screen, t)
        screen.blit(self.crt, (0, 0))

    # ------------------------------------------------------------------ #
    def run(self) -> None:
        """Главный цикл."""
        while self.running:
            dt = min(self.clock.tick(FPS) / 1000.0, MAX_FRAME_TIME)
            self.time += dt
            for event in pygame.event.get():
                self.handle_event(event)
            self.update(dt)
            self.draw()
            pygame.display.flip()
        pygame.quit()


def main() -> None:
    """Запуск игры."""
    Game().run()


if __name__ == "__main__":
    main()
