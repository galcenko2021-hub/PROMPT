"""Звуковые эффекты из сгенерированных тонов (без внешних файлов).

Если звуковое устройство недоступно, модуль тихо отключается —
игра продолжает работать без звука.
"""
from __future__ import annotations

import array
import math
import random

import pygame

_SAMPLE_RATE = 22050
_sounds: dict[str, pygame.mixer.Sound] = {}
_enabled = False
_channels = 1


def _wave(kind: str, phase: float) -> float:
    if kind == "square":
        return 1.0 if (phase % 1.0) < 0.5 else -1.0
    if kind == "triangle":
        p = phase % 1.0
        return 4 * p - 1 if p < 0.5 else 3 - 4 * p
    if kind == "noise":
        return random.uniform(-1.0, 1.0)
    return math.sin(phase * math.tau)


def _synth(notes: list[tuple[float, float]], kind: str = "square", volume: float = 0.25, sweep_to: float | None = None) -> pygame.mixer.Sound:
    """Собирает звук из последовательности нот ``(частота, длительность)``.

    ``sweep_to`` превращает единственную ноту в плавный глиссандо до заданной частоты.
    """
    samples = array.array("h")
    phase = 0.0
    for freq, duration in notes:
        count = max(1, int(_SAMPLE_RATE * duration))
        for i in range(count):
            t = i / count
            current = freq if sweep_to is None else freq + (sweep_to - freq) * t
            phase += current / _SAMPLE_RATE
            envelope = min(1.0, i / 60) * (1.0 - t) ** 1.6
            value = int(_wave(kind, phase) * envelope * volume * 32767)
            for _ in range(_channels):
                samples.append(value)
    return pygame.mixer.Sound(buffer=samples.tobytes())


def init() -> None:
    """Инициализирует микшер и генерирует все звуки."""
    global _enabled, _channels
    try:
        if not pygame.mixer.get_init():
            pygame.mixer.init(frequency=_SAMPLE_RATE, size=-16, channels=1, buffer=512)
        freq, fmt, channels = pygame.mixer.get_init()
        if fmt != -16 or freq != _SAMPLE_RATE:
            pygame.mixer.quit()
            pygame.mixer.init(frequency=_SAMPLE_RATE, size=-16, channels=1, buffer=512)
            freq, fmt, channels = pygame.mixer.get_init()
        if fmt != -16:
            return
        _channels = channels
    except pygame.error:
        return

    _sounds.update(
        {
            "blip": _synth([(880, 0.05)], "square", 0.12),
            "select": _synth([(660, 0.04), (990, 0.05)], "square", 0.12),
            "summon": _synth([(440, 0.06), (554, 0.06), (659, 0.06), (880, 0.12)], "triangle", 0.3),
            "error": _synth([(180, 0.09), (130, 0.14)], "square", 0.16),
            "success": _synth([(523, 0.07), (659, 0.07), (784, 0.07), (1046, 0.16)], "triangle", 0.3),
            "door": _synth([(200, 0.45)], "triangle", 0.3, sweep_to=640),
            "detected": _synth([(880, 0.08), (660, 0.08), (880, 0.08), (660, 0.12)], "square", 0.16),
            "mark": _synth([(1250, 0.035)], "square", 0.1),
            "push": _synth([(110, 0.12)], "noise", 0.12),
            "hit": _synth([(90, 0.35)], "noise", 0.3),
            "glitch": _synth([(300, 0.16)], "noise", 0.18),
            "beam": _synth([(300, 0.32)], "triangle", 0.22, sweep_to=1300),
            "rec": _synth([(1000, 0.05), (1500, 0.05)], "sine", 0.25),
            "upgrade": _synth([(392, 0.08), (523, 0.08), (659, 0.08), (784, 0.08), (1046, 0.08), (1318, 0.22)], "triangle", 0.3),
            "victory": _synth([(523, 0.12), (659, 0.12), (784, 0.12), (1046, 0.2), (784, 0.1), (1046, 0.35)], "triangle", 0.32),
            "tone0": _synth([(523, 0.2)], "sine", 0.35),
            "tone1": _synth([(659, 0.2)], "sine", 0.35),
            "tone2": _synth([(784, 0.2)], "sine", 0.35),
            "tone3": _synth([(988, 0.2)], "sine", 0.35),
        }
    )
    _enabled = True


def play(name: str) -> None:
    """Проигрывает звук по имени (если звук включён)."""
    if not _enabled:
        return
    sound = _sounds.get(name)
    if sound is not None:
        sound.play()
