"""Комната (уровень): карта из символов, стены, двери, терминал призыва, стражи.

Легенда карты (общие символы):
    ``#`` стена, `` `` (пробел) пропасть, ``.`` пол, ``=`` стекло,
    ``,`` повреждённый сектор (безопасный), ``x`` повреждённый сектор с «миной»,
    ``P`` точка появления, ``T`` терминал призыва, ``D`` выход.
Остальные символы описываются в ``legend`` конкретного уровня (levels.py).
"""
from __future__ import annotations

import math
import random
from collections import defaultdict, deque
from typing import TYPE_CHECKING, Any, Callable

import pygame
from pygame.math import Vector2

import audio
import graphics as gfx
from boss import Vibecoder
from puzzle import Interactable, LinkObjects, Puzzle, RouteMemory, create_puzzle
from settings import (
    ASSISTANT_DESCRIPTIONS,
    BG_COLOR,
    DANGER_COLOR,
    DEVICE_HEIGHT,
    FIELD_COLOR,
    FIELD_GRID_COLOR,
    FLOOR_ALT_COLOR,
    FLOOR_COLOR,
    GATE_HEIGHT,
    GLASS_COLOR,
    GLASS_HEIGHT,
    GRID_COLOR,
    MINE_REVEAL_RADIUS,
    SCREEN_HEIGHT,
    SCREEN_WIDTH,
    SENTINEL_COLOR,
    SENTINEL_FOV_DEG,
    SENTINEL_PAUSE,
    SENTINEL_SPEED,
    SENTINEL_TURN_SPEED,
    SENTINEL_VIEW_RANGE,
    DETECTION_TIME,
    SUCCESS_COLOR,
    TERMINAL_COLOR,
    TILE,
    WALL_COLOR,
    WALL_EDGE_COLOR,
    WALL_HEIGHT,
    WALL_SIDE_LIGHT,
    Color,
    Tile,
)

if TYPE_CHECKING:
    from assistant import Assistant
    from puzzle import GameAPI

DIRS4: tuple[Tile, ...] = ((1, 0), (-1, 0), (0, 1), (0, -1))
Drawer = Callable[[pygame.Surface, Vector2, float], None]


def _angle_diff(a: float, b: float) -> float:
    """Минимальная разница углов в радианах (-pi..pi)."""
    return (a - b + math.pi) % math.tau - math.pi


# --------------------------------------------------------------------------- #
# Страж-глитч
# --------------------------------------------------------------------------- #
class Sentinel:
    """Патрульный страж с конусом обзора. Заметив Оператора, отправляет его к началу уровня."""

    RAYS = 22

    def __init__(self, waypoints: list[Vector2]) -> None:
        self.waypoints = waypoints
        self.pos = Vector2(waypoints[0])
        self.index = 1 % len(waypoints)
        target = waypoints[self.index] if len(waypoints) > 1 else self.pos + Vector2(1, 0)
        self.base_angle = math.atan2(target.y - self.pos.y, target.x - self.pos.x)
        self.angle = self.base_angle
        self.wait = 0.0
        self.alert = 0.0
        self.scan = random.uniform(0, math.tau)

    def update(self, dt: float, room: Room, player_pos: Vector2, player_hidden: bool) -> bool:
        """Двигает стража; возвращает ``True``, если Оператор обнаружен."""
        self.scan += dt
        if len(self.waypoints) > 1 and self.wait <= 0:
            target = self.waypoints[self.index]
            delta = target - self.pos
            distance = delta.length()
            step = SENTINEL_SPEED * dt
            if distance <= step:
                self.pos = Vector2(target)
                self.index = (self.index + 1) % len(self.waypoints)
                self.wait = SENTINEL_PAUSE
                nxt = self.waypoints[self.index]
                self.base_angle = math.atan2(nxt.y - self.pos.y, nxt.x - self.pos.x)
            else:
                self.pos += delta / distance * step
                self.base_angle = math.atan2(delta.y, delta.x)
            desired = self.base_angle
        else:
            self.wait -= dt
            desired = self.base_angle + math.sin(self.scan * 2.2) * 0.75  # осматривается
        diff = _angle_diff(desired, self.angle)
        turn = SENTINEL_TURN_SPEED * dt
        self.angle += max(-turn, min(turn, diff))

        sees = not player_hidden and self.can_see(player_pos, room)
        if sees:
            self.alert = min(1.0, self.alert + dt / DETECTION_TIME)
        else:
            self.alert = max(0.0, self.alert - dt * 0.8)
        return self.alert >= 1.0

    def can_see(self, target: Vector2, room: Room) -> bool:
        """Попадает ли точка в конус обзора без препятствий."""
        delta = target - self.pos
        distance = delta.length()
        if distance > SENTINEL_VIEW_RANGE or distance < 1:
            return distance < 1
        if abs(_angle_diff(math.atan2(delta.y, delta.x), self.angle)) > math.radians(SENTINEL_FOV_DEG) / 2:
            return False
        return room.line_of_sight(self.pos, target)

    def draw_cone(self, surface: pygame.Surface, cam: Vector2, room: Room) -> None:
        """Конус обзора: лучи обрезаются стенами, рисуется аддитивно."""
        half = math.radians(SENTINEL_FOV_DEG) / 2
        points = [self.pos - cam]
        for i in range(self.RAYS + 1):
            angle = self.angle - half + 2 * half * i / self.RAYS
            points.append(room.cast_ray(self.pos, angle, SENTINEL_VIEW_RANGE) - cam)
        xs = [p.x for p in points]
        ys = [p.y for p in points]
        bbox = pygame.Rect(int(min(xs)) - 2, int(min(ys)) - 2, int(max(xs) - min(xs)) + 5, int(max(ys) - min(ys)) + 5)
        if not bbox.colliderect(surface.get_rect()):
            return
        color = gfx.mix_color((70, 58, 12), (110, 18, 24), self.alert)
        layer = pygame.Surface(bbox.size)
        layer.fill((0, 0, 0))
        pygame.draw.polygon(layer, color, [(p.x - bbox.x, p.y - bbox.y) for p in points])
        surface.blit(layer, bbox.topleft, special_flags=pygame.BLEND_RGB_ADD)
        edge = gfx.mix_color(SENTINEL_COLOR, DANGER_COLOR, self.alert)
        pygame.draw.lines(surface, gfx.scale_color(edge, 0.55), False, points[1:], 1)
        pygame.draw.line(surface, gfx.scale_color(edge, 0.55), points[0], points[1], 1)
        pygame.draw.line(surface, gfx.scale_color(edge, 0.55), points[0], points[-1], 1)

    def draw(self, surface: pygame.Surface, cam: Vector2, t: float) -> None:
        """Тело стража: вращающийся ромб с «глазом»."""
        center = self.pos - cam
        color = gfx.mix_color(SENTINEL_COLOR, DANGER_COLOR, self.alert)
        gfx.draw_glow(surface, color, center, 36, 0.8)
        gfx.draw_neon_polygon(surface, color, gfx.regular_polygon(center, 17, 4, t * 2), 2, 0.25)
        eye = center + Vector2(math.cos(self.angle), math.sin(self.angle)) * 8
        pygame.draw.circle(surface, color, eye, 4)
        if self.alert > 0.15:
            gfx.draw_text(surface, "!", center + Vector2(0, -32), 22, DANGER_COLOR, "center", bold=True)


# --------------------------------------------------------------------------- #
# Комната
# --------------------------------------------------------------------------- #
class Room:
    """Одна сцена-уровень: карта, объекты, головоломки, стражи и (опционально) босс."""

    def __init__(self, level: dict[str, Any]) -> None:
        self.level = level
        self.grid: list[str] = level["map"]
        self.height = len(self.grid)
        self.width = max(len(row) for row in self.grid)
        self.time = 0.0
        self.notify: Callable[[str, str, Color], None] = lambda text, speaker, color: None
        self.particles = gfx.ParticleSystem()
        self.shake = 0.0  # сила тряски камеры (выставляет босс)

        self.walls: set[Tile] = set()
        self.void: set[Tile] = set()
        self.glass: set[Tile] = set()
        self.field: set[Tile] = set()
        self.mines: set[Tile] = set()
        self.revealed_mines: set[Tile] = set()
        self.doors: list[Tile] = []
        self.door_open = False
        self.gates: dict[Tile, str] = {}
        self.opened: set[str] = set()
        self.terminal: Tile | None = None
        self.spawn = Vector2(TILE * 1.5, TILE * 1.5)
        self.link_source: LinkObjects | None = None

        elements: dict[str, dict[str, list[tuple[Tile, Any]]]] = defaultdict(lambda: defaultdict(list))
        waypoints: dict[str, list[tuple[int, Tile]]] = defaultdict(list)
        boss_tile: Tile | None = None
        legend: dict[str, tuple] = level.get("legend", {})

        for y in range(self.height):
            row = self.grid[y].ljust(self.width)
            for x, ch in enumerate(row):
                tile = (x, y)
                if ch == "#":
                    self.walls.add(tile)
                elif ch == " ":
                    self.void.add(tile)
                elif ch == "=":
                    self.glass.add(tile)
                elif ch == ",":
                    self.field.add(tile)
                elif ch == "x":
                    self.field.add(tile)
                    self.mines.add(tile)
                elif ch == "P":
                    self.spawn = gfx.tile_center(tile)
                elif ch == "T":
                    self.terminal = tile
                elif ch == "D":
                    self.doors.append(tile)
                elif ch == ".":
                    pass
                elif ch in legend:
                    entry = legend[ch]
                    kind, pid = entry[0], entry[1]
                    extra = entry[2] if len(entry) > 2 else None
                    if kind == "gate":
                        self.gates[tile] = pid
                    elif kind == "waypoint":
                        waypoints[pid].append((int(extra or 0), tile))
                    elif kind == "boss":
                        boss_tile = tile
                        self.void.add(tile)
                    else:
                        elements[pid][kind].append((tile, extra))
                else:
                    raise ValueError(f"Неизвестный символ карты {ch!r} в клетке {tile} уровня {level.get('id')}")

        self.puzzles: list[Puzzle] = [
            create_puzzle(data, self, {k: list(v) for k, v in elements.get(data["id"], {}).items()}) for data in level["puzzles"]
        ]
        self.puzzle_by_id = {p.pid: p for p in self.puzzles}
        self.static_solid: set[Tile] = set()
        for puzzle in self.puzzles:
            self.static_solid |= puzzle.solid_tiles()
        if self.terminal is not None:
            self.static_solid.add(self.terminal)

        self.sentinels = [Sentinel([gfx.tile_center(t) for _, t in sorted(points)]) for _, points in sorted(waypoints.items())]
        self.boss: Vibecoder | None = None
        boss_data = level.get("boss")
        if boss_data and boss_tile is not None:
            self.boss = Vibecoder(gfx.tile_center(boss_tile), list(boss_data["phases"]), self)

        self.floor_surface = self._render_floor()
        self.wall_blocks = self._build_wall_blocks()

    # ------------------------------------------------------------------ #
    # Клетки
    # ------------------------------------------------------------------ #
    def in_bounds(self, tile: Tile) -> bool:
        """Лежит ли клетка внутри карты."""
        return 0 <= tile[0] < self.width and 0 <= tile[1] < self.height

    def is_wall(self, tile: Tile) -> bool:
        """Стена, пропасть или край карты."""
        return not self.in_bounds(tile) or tile in self.walls or tile in self.void

    def gate_closed(self, tile: Tile) -> bool:
        """Закрыты ли ворота в этой клетке."""
        pid = self.gates.get(tile)
        return pid is not None and pid not in self.opened

    def door_closed(self, tile: Tile) -> bool:
        """Закрыт ли выход в этой клетке."""
        return tile in self.doors and not self.door_open

    def blocks_player(self, tile: Tile) -> bool:
        """Непроходимо ли для Оператора."""
        return (
            self.is_wall(tile)
            or tile in self.glass
            or tile in self.static_solid
            or self.gate_closed(tile)
            or self.door_closed(tile)
        )

    def blocks_sight(self, tile: Tile) -> bool:
        """Перекрывает ли клетка обзор стражам."""
        return not self.in_bounds(tile) or tile in self.walls or self.gate_closed(tile) or self.door_closed(tile)

    def box_tiles(self) -> set[Tile]:
        """Клетки, занятые ящиками."""
        tiles: set[Tile] = set()
        for puzzle in self.puzzles:
            tiles |= puzzle.blocking_tiles()
        return tiles

    def assistant_can_walk(self, tile: Tile, allow_field: bool = False, ignore_boxes: bool = False) -> bool:
        """Может ли Помощник пройти по клетке (повреждённые сектора он обходит)."""
        if self.blocks_player(tile) or tile in self.doors:
            return False
        if not allow_field and tile in self.field:
            return False
        return ignore_boxes or tile not in self.box_tiles()

    def free_tile_near(self, tile: Tile) -> Tile | None:
        """Ближайшая к клетке клетка, где может стоять Помощник."""
        if self.assistant_can_walk(tile):
            return tile
        seen = {tile}
        queue = deque([tile])
        while queue:
            x, y = queue.popleft()
            for dx, dy in DIRS4:
                n = (x + dx, y + dy)
                if n in seen or not self.in_bounds(n) or abs(n[0] - tile[0]) + abs(n[1] - tile[1]) > 8:
                    continue
                seen.add(n)
                if self.assistant_can_walk(n):
                    return n
                if not self.blocks_player(n):
                    queue.append(n)
        return None

    # ------------------------------------------------------------------ #
    # Поиск пути и видимость
    # ------------------------------------------------------------------ #
    def find_path(self, start: Tile, goals: set[Tile], passable: Callable[[Tile], bool]) -> list[Tile] | None:
        """Кратчайший путь (BFS, 4 направления) от ``start`` до любой из ``goals``."""
        if not goals:
            return None
        if start in goals:
            return [start]
        parents: dict[Tile, Tile | None] = {start: None}
        queue = deque([start])
        while queue:
            current = queue.popleft()
            for dx, dy in DIRS4:
                n = (current[0] + dx, current[1] + dy)
                if n in parents or not passable(n):
                    continue
                parents[n] = current
                if n in goals:
                    return self._unwind(parents, n)
                queue.append(n)
        return None

    def find_path_nearest(self, start: Tile, target: Tile, passable: Callable[[Tile], bool]) -> list[Tile] | None:
        """Путь к цели, а если она недостижима — к ближайшей к ней достижимой клетке."""
        parents: dict[Tile, Tile | None] = {start: None}
        queue = deque([start])
        best = start
        best_distance = math.dist(start, target)
        while queue:
            current = queue.popleft()
            if current == target:
                best = current
                break
            for dx, dy in DIRS4:
                n = (current[0] + dx, current[1] + dy)
                if n in parents or not passable(n):
                    continue
                parents[n] = current
                distance = math.dist(n, target)
                if distance < best_distance:
                    best, best_distance = n, distance
                queue.append(n)
        return self._unwind(parents, best)

    @staticmethod
    def _unwind(parents: dict[Tile, Tile | None], end: Tile) -> list[Tile]:
        path = [end]
        node = parents[end]
        while node is not None:
            path.append(node)
            node = parents[node]
        path.reverse()
        return path

    def line_of_sight(self, a: Vector2, b: Vector2) -> bool:
        """Нет ли препятствий между двумя точками."""
        delta = b - a
        distance = delta.length()
        steps = max(1, int(distance / 8))
        for i in range(1, steps):
            point = a + delta * (i / steps)
            if self.blocks_sight(gfx.tile_of(point)):
                return False
        return True

    def cast_ray(self, origin: Vector2, angle: float, max_distance: float) -> Vector2:
        """Точка, где луч упирается в стену (или конец луча)."""
        direction = Vector2(math.cos(angle), math.sin(angle))
        distance = 0.0
        while distance < max_distance:
            distance += 8
            point = origin + direction * distance
            if self.blocks_sight(gfx.tile_of(point)):
                return origin + direction * (distance - 4)
        return origin + direction * max_distance

    # ------------------------------------------------------------------ #
    # Игровая логика
    # ------------------------------------------------------------------ #
    @property
    def solved_count(self) -> int:
        """Сколько головоломок решено."""
        return sum(1 for p in self.puzzles if p.solved)

    @property
    def all_solved(self) -> bool:
        """Решены ли все головоломки комнаты."""
        return all(p.solved for p in self.puzzles)

    def recording(self) -> bool:
        """Идёт ли сейчас запись маршрута."""
        return any(isinstance(p, RouteMemory) and p.recording for p in self.puzzles)

    def veto(self, assistant: Assistant, puzzle: Puzzle) -> str | None:
        """Запрет на решение (используется боссом)."""
        return self.boss.veto(assistant, puzzle) if self.boss else None

    def reveal_mine(self, tile: Tile) -> None:
        """Показывает «мину» в повреждённом секторе."""
        if tile in self.mines and tile not in self.revealed_mines:
            self.revealed_mines.add(tile)
            self.particles.burst(gfx.tile_center(tile), DANGER_COLOR, 22, 180, 0.6)
            audio.play("glitch")

    def on_player_caught(self) -> None:
        """Сброс тревоги и незавершённых действий после поимки Оператора."""
        for sentinel in self.sentinels:
            sentinel.alert = 0.0
        for puzzle in self.puzzles:
            puzzle.reset_transient()

    def interactables(self, game: GameAPI) -> list[Interactable]:
        """Все объекты, с которыми сейчас можно взаимодействовать."""
        items: list[Interactable] = []
        if self.terminal is not None:
            roster = game.roster
            if roster.summoned:
                label = "Перезагрузить Помощника рядом с собой"
            else:
                label = f"Призвать Помощника: {roster.current.name}"
            items.append(Interactable(gfx.tile_center(self.terminal), label, self._on_terminal, color=TERMINAL_COLOR))
        for puzzle in self.puzzles:
            if puzzle.active:
                items.extend(puzzle.interactables())
            elif not puzzle.solved:
                items.append(Interactable(puzzle.anchor, "Модуль зашифрован Вайбкодером", self._on_locked, color=DANGER_COLOR))
        if self.doors and not self.door_open:
            items.append(Interactable(gfx.tile_center(self.doors[0]), "Выход заперт", self._on_door, color=DANGER_COLOR))
        return items

    def _on_terminal(self, game: GameAPI) -> None:
        roster = game.roster
        if not roster.summoned:
            assistant = roster.summon(game.player.pos)
            game.notify(
                f"На связи! Интеллект {assistant.level}/5. {ASSISTANT_DESCRIPTIONS[assistant.level]}", assistant.name, assistant.color
            )
            return
        assistant = roster.current
        if assistant.busy:
            game.notify(f"{assistant.name} занят задачей.")
            return
        assistant.teleport_near(game.player.pos)
        audio.play("summon")
        game.notify("Помощник перезагружен рядом с тобой.")

    def _on_locked(self, game: GameAPI) -> None:
        audio.play("error")
        game.notify("Вайбкодер шифрует этот модуль. Сначала пройди текущую фазу.", "СИСТЕМА", DANGER_COLOR)

    def _on_door(self, game: GameAPI) -> None:
        audio.play("error")
        game.notify(f"Дверь откроется, когда все головоломки будут решены ({self.solved_count}/{len(self.puzzles)}).")

    def _route_pad(self) -> Vector2 | None:
        for puzzle in self.puzzles:
            if isinstance(puzzle, RouteMemory):
                return gfx.tile_center(puzzle.pad)
        return None

    def player_on_exit(self, pos: Vector2) -> bool:
        """Вошёл ли Оператор в открытую дверь."""
        return self.door_open and gfx.tile_of(pos) in self.doors

    def update(self, dt: float, game: GameAPI) -> None:
        """Логика кадра: головоломки, мины, стражи, босс, дверь."""
        self.time += dt
        self.shake = max(0.0, self.shake - dt)
        for puzzle in self.puzzles:
            puzzle.update(dt, game)
        for puzzle in self.puzzles:
            if not puzzle.solved and puzzle.check_solved():
                puzzle.solved = True
                self._on_solved(puzzle, game)

        player = game.player
        tile = player.tile
        if tile in self.mines:
            self.reveal_mine(tile)
            game.player_caught("Повреждённый сектор! Откат к площадке записи.", self._route_pad(), force=True)
        for mine in self.mines - self.revealed_mines:
            if gfx.tile_center(mine).distance_to(player.pos) < MINE_REVEAL_RADIUS:
                self.revealed_mines.add(mine)

        for sentinel in self.sentinels:
            if sentinel.update(dt, self, player.pos, player.invulnerable > 0):
                game.player_caught("Страж-глитч заметил тебя! Откат к началу уровня.")
                break

        if self.boss is not None:
            self.boss.update(dt, game)

        if self.doors and not self.door_open and self.all_solved:
            self.door_open = True
            audio.play("door")
            self.notify("Все задачи решены. Выход открыт!", "СИСТЕМА", SUCCESS_COLOR)
            for door in self.doors:
                self.particles.burst(gfx.tile_center(door), SUCCESS_COLOR, 30, 200, 0.9)
        self.particles.update(dt)

    def _on_solved(self, puzzle: Puzzle, game: GameAPI) -> None:
        self.opened.add(puzzle.pid)
        audio.play("success")
        self.particles.burst(puzzle.anchor, SUCCESS_COLOR, 36, 240, 0.9)
        self.notify(f"Головоломка решена: {puzzle.title}.", "СИСТЕМА", SUCCESS_COLOR)
        gates = [tile for tile, pid in self.gates.items() if pid == puzzle.pid]
        if gates:
            audio.play("door")
            self.notify("Проход открыт.", "СИСТЕМА", SUCCESS_COLOR)
            for gate in gates:
                self.particles.burst(gfx.tile_center(gate), SUCCESS_COLOR, 16, 160, 0.6)
        if self.boss is not None:
            self.boss.on_puzzle_solved(puzzle, game)

    # ------------------------------------------------------------------ #
    # Отрисовка
    # ------------------------------------------------------------------ #
    def _render_floor(self) -> pygame.Surface:
        """Предрисовка статичного пола всей карты (один раз на уровень)."""
        surf = pygame.Surface((self.width * TILE, self.height * TILE))
        surf.fill(BG_COLOR)
        rng = random.Random(self.level.get("id", 0))
        for y in range(self.height):
            for x in range(self.width):
                tile = (x, y)
                rect = gfx.tile_rect(tile)
                if tile in self.walls:
                    continue
                if tile in self.void:
                    for _ in range(2):
                        px = rect.x + rng.randint(4, TILE - 4)
                        py = rect.y + rng.randint(4, TILE - 4)
                        surf.set_at((px, py), (40, 30, 70))
                    continue
                if tile in self.field:
                    pygame.draw.rect(surf, FIELD_COLOR, rect)
                    pygame.draw.rect(surf, FIELD_GRID_COLOR, rect, 1)
                    for _ in range(3):
                        px = rect.x + rng.randint(4, TILE - 8)
                        py = rect.y + rng.randint(4, TILE - 6)
                        pygame.draw.line(surf, (70, 30, 55), (px, py), (px + rng.randint(3, 9), py), 1)
                    continue
                color = FLOOR_COLOR if (x + y) % 2 == 0 else FLOOR_ALT_COLOR
                pygame.draw.rect(surf, color, rect)
                pygame.draw.rect(surf, GRID_COLOR, rect, 1)
                pygame.draw.circle(surf, (32, 34, 58), rect.topleft, 2)
        # Неоновый отсвет у основания стен и по краю пропасти.
        wall_glow = gfx.scale_color(WALL_EDGE_COLOR, 0.3)
        void_glow = (120, 50, 190)
        for y in range(self.height):
            for x in range(self.width):
                tile = (x, y)
                if tile in self.walls or tile in self.void:
                    continue
                rect = gfx.tile_rect(tile)
                edges = (
                    ((x, y - 1), rect.topleft, rect.topright),
                    ((x, y + 1), (rect.left, rect.bottom - 1), (rect.right, rect.bottom - 1)),
                    ((x - 1, y), rect.topleft, rect.bottomleft),
                    ((x + 1, y), (rect.right - 1, rect.top), (rect.right - 1, rect.bottom)),
                )
                for neighbour, a, b in edges:
                    if neighbour in self.walls:
                        pygame.draw.line(surf, wall_glow, a, b, 2)
                    elif neighbour in self.void:
                        pygame.draw.line(surf, void_glow, a, b, 3)
        return surf

    def _build_wall_blocks(self) -> dict[Tile, gfx.Block]:
        """Статичные 3D-блоки стен и стекла."""
        blocks: dict[Tile, gfx.Block] = {}
        for tile in self.walls:
            x, y = tile
            exposed = tuple((x + dx, y + dy) not in self.walls for dx, dy in ((0, -1), (1, 0), (0, 1), (-1, 0)))
            if any(exposed):
                blocks[tile] = gfx.Block(gfx.tile_rect(tile), WALL_HEIGHT, WALL_COLOR, WALL_SIDE_LIGHT, WALL_EDGE_COLOR, exposed)  # type: ignore[arg-type]
        for tile in self.glass:
            x, y = tile
            exposed = tuple((x + dx, y + dy) not in self.glass for dx, dy in ((0, -1), (1, 0), (0, 1), (-1, 0)))
            blocks[tile] = gfx.Block(gfx.tile_rect(tile), GLASS_HEIGHT, (16, 40, 52), (12, 30, 40), GLASS_COLOR, exposed, 1)  # type: ignore[arg-type]
        return blocks

    def _visible_tiles(self, cam: Vector2) -> tuple[range, range]:
        x0 = int(cam.x // TILE) - 2
        y0 = int(cam.y // TILE) - 2
        return range(x0, x0 + SCREEN_WIDTH // TILE + 5), range(y0, y0 + SCREEN_HEIGHT // TILE + 5)

    def _dynamic_blocks(self) -> list[gfx.Block]:
        blocks: list[gfx.Block] = []
        for tile in self.gates:
            if self.gate_closed(tile):
                blocks.append(gfx.Block(gfx.tile_rect(tile), GATE_HEIGHT, (40, 10, 22), (30, 8, 16), DANGER_COLOR))
        for tile in self.doors:
            if not self.door_open:
                blocks.append(gfx.Block(gfx.tile_rect(tile), WALL_HEIGHT, (44, 12, 24), (32, 8, 18), DANGER_COLOR))
        if self.terminal is not None:
            rect = gfx.tile_rect(self.terminal).inflate(-8, -8)
            blocks.append(gfx.Block(rect, DEVICE_HEIGHT, (8, 40, 48), (6, 28, 34), TERMINAL_COLOR))
        for puzzle in self.puzzles:
            blocks.extend(puzzle.blocks())
        return blocks

    def draw(self, surface: pygame.Surface, cam: Vector2, t: float, entities: list[Drawer]) -> None:
        """Рисует комнату целиком: пол → объекты → персонажи → 3D-стены → эффекты."""
        surface.fill(BG_COLOR)
        surface.blit(self.floor_surface, (-cam.x, -cam.y))
        xs, ys = self._visible_tiles(cam)

        for tile in self.revealed_mines:
            center = gfx.tile_center(tile) - cam
            pulse = 0.6 + 0.4 * math.sin(t * 6 + tile[0])
            gfx.draw_glow(surface, DANGER_COLOR, center, 26, 0.6 * pulse)
            pygame.draw.line(surface, DANGER_COLOR, center + Vector2(-10, -10), center + Vector2(10, 10), 3)
            pygame.draw.line(surface, DANGER_COLOR, center + Vector2(10, -10), center + Vector2(-10, 10), 3)
        for tile, pid in self.gates.items():
            if pid in self.opened:
                rect = gfx.tile_rect(tile).move(-cam.x, -cam.y)
                pygame.draw.rect(surface, gfx.scale_color(SUCCESS_COLOR, 0.35), rect, 1)
        if self.door_open:
            for tile in self.doors:
                rect = gfx.tile_rect(tile).move(-cam.x, -cam.y)
                pulse = 0.5 + 0.5 * math.sin(t * 5)
                gfx.draw_glow(surface, SUCCESS_COLOR, rect.center, 60, 0.6 + 0.4 * pulse)
                pygame.draw.rect(surface, gfx.scale_color(SUCCESS_COLOR, 0.3), rect)
                pygame.draw.rect(surface, SUCCESS_COLOR, rect, 2)
                pygame.draw.rect(surface, SUCCESS_COLOR, rect.inflate(-14 - 8 * pulse, -14 - 8 * pulse), 1)

        for puzzle in self.puzzles:
            puzzle.draw_floor(surface, cam, t)
        for sentinel in self.sentinels:
            sentinel.draw_cone(surface, cam, self)
        if self.boss is not None:
            self.boss.draw_ground(surface, cam, t)
        for draw in entities:
            draw(surface, cam, t)
        for sentinel in self.sentinels:
            sentinel.draw(surface, cam, t)

        blocks = [self.wall_blocks[(x, y)] for y in ys for x in xs if (x, y) in self.wall_blocks]
        blocks.extend(self._dynamic_blocks())
        gfx.draw_blocks(surface, blocks, cam)

        if self.terminal is not None:
            top = Vector2(gfx.project_point(gfx.tile_center(self.terminal) - cam, DEVICE_HEIGHT))
            gfx.draw_glow(surface, TERMINAL_COLOR, top, 30, 0.6 + 0.3 * math.sin(t * 4))
            for i in range(3):
                width = 6 + int(8 * abs(math.sin(t * 3 + i)))
                pygame.draw.line(surface, TERMINAL_COLOR, (top.x - 9, top.y - 6 + i * 6), (top.x - 9 + width, top.y - 6 + i * 6), 2)
        for puzzle in self.puzzles:
            puzzle.draw_overlay(surface, cam, t)
            if not puzzle.active and not puzzle.solved:
                center = puzzle.anchor - cam
                gfx.draw_text(surface, "ЗАШИФРОВАНО", center + Vector2(0, -34), 13, DANGER_COLOR, "center", bold=True)
                for i in range(3):
                    bar = pygame.Rect(0, 0, random.randint(16, 46), 2)
                    bar.center = (int(center.x + random.uniform(-20, 20)), int(center.y + random.uniform(-18, 18)))
                    surface.fill(DANGER_COLOR, bar)
        if self.boss is not None:
            self.boss.draw(surface, cam, t)
        self.particles.draw(surface, cam)

