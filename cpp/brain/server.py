"""Сервер «мозга» Помощников: JSON-строки через stdin/stdout.

Игра на C++ запускает этот процесс скрытым и общается с ним так:
    запрос:  {"id": 7, "type": "think", "level": 4, "text": "...", "objects": [5, 6]}
    ответ:   {"id": 7, "type": "thought", "level": 4, ...}
Сообщения без id (``progress``, ``ready``, ``log``) сервер отправляет сам.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time
import traceback
from typing import Any

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from models import Brain  # noqa: E402

PROTOCOL_VERSION = 1
AUTOSAVE_EVERY = 10

_out_lock = threading.Lock()


def send(message: dict[str, Any]) -> None:
    """Отправляет одну JSON-строку игре."""
    line = json.dumps(message, ensure_ascii=False, separators=(",", ":"))
    with _out_lock:
        sys.stdout.write(line + "\n")
        sys.stdout.flush()


def log(text: str) -> None:
    print(f"[brain {time.strftime('%H:%M:%S')}] {text}", file=sys.stderr, flush=True)


class BrainServer:
    """Пять моделей, фоновое предобучение и обработка запросов."""

    def __init__(self, models_dir: str) -> None:
        self.models_dir = models_dir
        self.brains = {level: Brain(level) for level in range(1, 6)}
        self.learn_count = 0
        self.trainer: threading.Thread | None = None

    # ------------------------------------------------------------------ #
    def start_training(self, levels: list[int] | None = None) -> None:
        """Загружает сохранённые веса или предобучает модели в фоне."""
        levels = levels or list(self.brains)

        def work() -> None:
            for level in levels:
                brain = self.brains[level]
                with brain.lock:
                    if brain.ready:
                        continue
                    if brain.load(self.models_dir):
                        log(f"модель {level}: загружена с диска")
                        send({"type": "progress", "level": level, "value": 1.0, "loaded": True})
                        continue
                    log(f"модель {level}: предобучение")
                    last = [0.0]

                    def report(value: float, level: int = level) -> None:
                        if value - last[0] >= 0.02 or value >= 1.0:
                            last[0] = value
                            send({"type": "progress", "level": level, "value": round(value, 3)})

                    started = time.time()
                    brain.pretrain(report)
                    brain.save(self.models_dir)
                    log(f"модель {level}: готово за {time.time() - started:.1f} с, точность {brain.stats.acc_action:.2%}")
                    send({"type": "progress", "level": level, "value": 1.0})
            send({"type": "ready", "status": self.status()})

        self.trainer = threading.Thread(target=work, name="pretrain", daemon=True)
        self.trainer.start()

    def status(self) -> dict[str, Any]:
        return {str(level): brain.status() for level, brain in self.brains.items()}

    def save_all(self) -> None:
        for brain in self.brains.values():
            if brain.ready:
                with brain.lock:
                    brain.save(self.models_dir)

    # ------------------------------------------------------------------ #
    def handle(self, msg: dict[str, Any]) -> dict[str, Any] | None:
        kind = msg.get("type")
        rid = msg.get("id")
        if kind == "hello":
            return {"id": rid, "type": "hello", "version": PROTOCOL_VERSION, "pid": os.getpid()}
        if kind == "status":
            return {"id": rid, "type": "status", "status": self.status()}
        if kind == "save":
            self.save_all()
            return {"id": rid, "type": "saved"}
        if kind == "shutdown":
            self.save_all()
            return {"id": rid, "type": "bye"}

        level = int(msg.get("level", 0))
        brain = self.brains.get(level)
        if brain is None:
            return {"id": rid, "type": "error", "error": f"нет модели уровня {level}"}
        if not brain.ready:
            return {"id": rid, "type": "error", "level": level, "error": "training", "progress": brain.progress}

        with brain.lock:
            if kind == "think":
                thought = brain.think(str(msg.get("text", "")), msg.get("objects"))
                return {"id": rid, "type": "thought", "level": level, **thought}
            if kind == "learn":
                intent = tuple(int(x) for x in msg["intent"])
                mode = msg.get("mode", "reinforce")
                text = str(msg.get("text", ""))
                if mode == "correct":
                    brain.correct(text, intent)
                elif mode == "discourage":
                    brain.discourage(text, intent)
                else:
                    brain.reinforce(text, intent)
                self.learn_count += 1
                if self.learn_count % AUTOSAVE_EVERY == 0:
                    brain.save(self.models_dir)
                after = brain.think(text)
                return {
                    "id": rid,
                    "type": "learned",
                    "level": level,
                    "mode": mode,
                    "intent_after": after["intent"],
                    "confidence_after": after["confidence"],
                    "memory": len(brain.memory),
                }
            if kind == "corrupt":
                brain.corrupt(float(msg.get("amount", 1.5)))
                return {"id": rid, "type": "corrupted", "level": level}
            if kind == "heal":
                brain.heal()
                return {"id": rid, "type": "healed", "level": level}
            if kind == "consolidate":
                brain.consolidate(int(msg.get("steps", 20)))
                brain.save(self.models_dir)
                return {"id": rid, "type": "consolidated", "level": level, "status": brain.status()}
            if kind == "reset":
                brain.reset()
                self.start_training([level])
                return {"id": rid, "type": "reset", "level": level}
        return {"id": rid, "type": "error", "error": f"неизвестный запрос {kind!r}"}


def main() -> None:
    parser = argparse.ArgumentParser(description="Нейросети Помощников игры «ПРОМПТ»")
    parser.add_argument("--models", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "models"))
    args = parser.parse_args()

    sys.stdin.reconfigure(encoding="utf-8")  # type: ignore[attr-defined]
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")  # type: ignore[attr-defined]

    server = BrainServer(os.path.abspath(args.models))
    send({"type": "hello", "version": PROTOCOL_VERSION, "pid": os.getpid()})
    server.start_training()
    try:
        for raw in sys.stdin:
            raw = raw.strip()
            if not raw:
                continue
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError as exc:
                send({"type": "error", "error": f"плохой JSON: {exc}"})
                continue
            try:
                reply = server.handle(msg)
            except Exception as exc:  # noqa: BLE001 — сервер не должен падать из-за одного запроса
                log(traceback.format_exc())
                reply = {"id": msg.get("id"), "type": "error", "error": str(exc)}
            if reply is not None:
                send(reply)
            if msg.get("type") == "shutdown":
                break
    finally:
        server.save_all()
        log("остановлен")


if __name__ == "__main__":
    main()
