"""Проверка нейросетей Помощников: предобучение, точность, рассуждения, дообучение, протокол.

Запуск: python brain/test_brain.py
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.stdout.reconfigure(encoding="utf-8")  # type: ignore[attr-defined]

from language import ACTIONS, GOALS, OBJECTS  # noqa: E402
from models import Brain  # noqa: E402

PROMPTS = [
    "Толкни ящик на плиту",
    "пиксель, подвинь коробку пожалуйста",
    "нажми кнопки по порядку",
    "повтори мой маршрут",
    "соедини ключ с замком",
    "Выключи вентилятор, чтобы пройти по коридору",
    "включи свет чтобы враг исчез",
    "выключи свет просто так",
    "Объясни Вайбкодеру, чтобы он понимал, что пишет",
    "иди ко мне",
    "привет, как дела?",
    "вырубай пропеллер, мне надо пройти",
    "зажги лампы, пусть тень пропадёт",
]


def show(brain: Brain, text: str, objects: list[int] | None = None) -> dict:
    th = brain.think(text, objects)
    a, o, g = th["intent"]
    words = " ".join(
        f"{t}{'' if k else '?'}({imp:.0%})" for t, k, imp in zip(th["tokens"], th["known"], th["importance"] or [1.0])
    )
    att = (" внимание: " + " ".join(f"{x:.0%}" for x in th["attention"])) if th["attention"] else ""
    print(f"  «{text}» -> {ACTIONS[a]}/{OBJECTS[o]}/{GOALS[g]} conf={th['confidence']:.0%} слова: {words}{att}")
    return th


def test_models() -> None:
    brains = {level: Brain(level) for level in range(1, 6)}
    for level, brain in brains.items():
        started = time.time()
        brain.pretrain()
        st = brain.stats
        print(
            f"[{level}] {brain.describe()} · параметров {brain.param_count()} · корпус {st.corpus_size} · "
            f"{time.time() - started:.1f} с · точность {st.acc_action:.0%}/{st.acc_object:.0%}/{st.acc_goal:.0%} · "
            f"loss {st.loss_history[0]:.3f} -> {st.loss_history[-1]:.3f}"
        )
        assert st.acc_action > 0.9 and st.acc_object > 0.9, "модель плохо обучилась"
    for level, brain in brains.items():
        print(f"--- Помощник {level}")
        for prompt in PROMPTS:
            show(brain, prompt)

    # контекст комнаты: «свет» отсутствует, есть только вентилятор
    th = brains[4].think("включи свет", objects=[5])
    assert th["object_present"] is False and th["context_object"] == 5

    pixel = brains[1]
    print("--- Дообучение Пикселя: «пни груз до плиты» (слово «пни» он не знает)")
    show(pixel, "пни груз до плиты")
    started = time.time()
    pixel.correct("пни груз до плиты", (1, 1, 0))
    print(f"  исправление: {1000 * (time.time() - started):.0f} мс")
    th = show(pixel, "пни груз до плиты")
    assert th["intent"][:2] == [1, 1]
    show(pixel, "нажми кнопки")

    print("--- Провал: Эхо считает, что «закинь мусор» — это толкнуть ящик?")
    echo = brains[2]
    before = show(echo, "закинь ящик")
    echo.discourage("закинь ящик", tuple(before["intent"]))
    show(echo, "закинь ящик")

    with tempfile.TemporaryDirectory() as tmp:
        pixel.save(tmp)
        again = Brain(1)
        assert again.load(tmp), "загрузка не удалась"
        th = show(again, "пни груз до плиты")
        assert th["intent"][:2] == [1, 1] and len(again.memory) == len(pixel.memory)
        print("  сохранение и загрузка: ок")

    oracle = brains[5]
    oracle.corrupt(1.5)
    print("--- Оракул с испорченными весами")
    show(oracle, "Объясни Вайбкодеру, чтобы он понимал, что пишет")
    oracle.heal()
    th = show(oracle, "Объясни Вайбкодеру, чтобы он понимал, что пишет")
    assert th["intent"] == [7, 9, 3]


def test_protocol() -> None:
    print("--- Протокол JSON через stdin/stdout")
    with tempfile.TemporaryDirectory() as tmp:
        server = os.path.join(os.path.dirname(os.path.abspath(__file__)), "server.py")
        proc = subprocess.Popen(
            [sys.executable, "-u", server, "--models", tmp],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            encoding="utf-8",
        )
        assert proc.stdin and proc.stdout

        def request(msg: dict) -> None:
            proc.stdin.write(json.dumps(msg, ensure_ascii=False) + "\n")
            proc.stdin.flush()

        def read_until(pred) -> dict:
            while True:
                line = proc.stdout.readline()
                assert line, "сервер закрыл поток"
                msg = json.loads(line)
                if pred(msg):
                    return msg

        hello = read_until(lambda m: m["type"] == "hello")
        print("  hello:", hello)
        request({"id": 1, "type": "think", "level": 1, "text": "толкни ящик"})
        reply = read_until(lambda m: m.get("id") == 1)
        print("  до готовности:", reply["type"], reply.get("error"))
        started = time.time()
        read_until(lambda m: m["type"] == "ready")
        print(f"  все модели готовы за {time.time() - started:.1f} с")
        request({"id": 2, "type": "think", "level": 4, "text": "Выключи вентилятор, чтобы пройти", "objects": [5, 1]})
        thought = read_until(lambda m: m.get("id") == 2)
        assert thought["intent"] == [6, 5, 1], thought
        print("  мысль:", thought["intent"], thought["confidence"])
        request({"id": 3, "type": "learn", "level": 1, "text": "пни груз", "intent": [1, 1, 0], "mode": "correct"})
        learned = read_until(lambda m: m.get("id") == 3)
        print("  дообучение:", learned)
        request({"id": 4, "type": "shutdown"})
        read_until(lambda m: m.get("id") == 4)
        proc.wait(timeout=20)
        assert os.path.exists(os.path.join(tmp, "brain1.npz"))
        print("  завершение и сохранение: ок")


if __name__ == "__main__":
    test_models()
    test_protocol()
    print("ВСЕ ПРОВЕРКИ ПРОЙДЕНЫ")
