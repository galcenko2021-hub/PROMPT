"""Модели Помощников: линейная, перцептрон и трансформер с self-attention.

Каждая модель по фразе Оператора предсказывает намерение (действие, объект, цель),
объясняет решение (вклад слов, внимание, гипотезы) и дообучается во время игры:
на успехах, провалах и исправлениях Оператора. Веса и память сохраняются на диск.
"""
from __future__ import annotations

import json
import os
import random
import threading
from functools import lru_cache
from typing import Any, Callable

import numpy as np

import nn
from language import (
    GOALS,
    NO_OBJECT_ACTIONS,
    OBJECTS,
    ACTIONS,
    Example,
    Stats,
    fnv1a,
    generate_corpus,
    stem,
    tokenize,
    word_feature_ids,
)

BAG_FEATURES = 2048
VOCAB_BUCKETS = 2048
MAX_TOKENS = 16
GOAL_WEIGHT = 0.6
BATCH = 32
MEMORY_LIMIT = 600

SPECS: dict[int, dict[str, Any]] = {
    1: {"arch": "linear", "epochs": 14, "lr": 0.02, "per": 30, "floor": 0.30},
    2: {"arch": "mlp", "hidden": 24, "epochs": 14, "lr": 0.01, "per": 35, "floor": 0.30},
    3: {"arch": "mlp", "hidden": 48, "epochs": 16, "lr": 0.01, "per": 45, "floor": 0.28},
    4: {"arch": "transformer", "d": 32, "layers": 1, "ffn": 64, "epochs": 16, "lr": 0.005, "per": 45, "floor": 0.25},
    5: {"arch": "transformer", "d": 48, "layers": 2, "ffn": 96, "epochs": 18, "lr": 0.004, "per": 55, "floor": 0.22},
}


@lru_cache(maxsize=20000)
def _features(word: str, buckets: int) -> tuple[int, ...]:
    return tuple(word_feature_ids(word, buckets))


@lru_cache(maxsize=20000)
def _pair_feature(a: str, b: str) -> int:
    return fnv1a(f"{stem(a)}|{stem(b)}", 4) % BAG_FEATURES


class Brain:
    """Нейросеть одного Помощника."""

    def __init__(self, level: int) -> None:
        self.level = level
        self.spec = SPECS[level]
        self.lock = threading.RLock()
        self.ready = False
        self.training = False
        self.progress = 0.0
        self.memory: list[Example] = []
        self.stats = Stats()
        self.backup: dict[str, np.ndarray] | None = None
        self._build()
        corpus = generate_corpus(1000 + level, level, self.spec["per"])
        hold = len(corpus) // 12
        self.holdout = corpus[-hold:]
        self.corpus = corpus[:-hold]
        self.stats.corpus_size = len(self.corpus)
        self.vocab = {stem(t) for ex in self.corpus for t in ex.tokens}
        self.random = random.Random(7919 * level + 17)

    # ------------------------------------------------------------------ #
    # Архитектура
    # ------------------------------------------------------------------ #
    def _build(self) -> None:
        rng = np.random.default_rng(7919 * self.level + 17)
        p: dict[str, nn.Tensor] = {}

        def add(name: str, shape: tuple[int, ...], std: float | None = None, value: float = 0.0) -> nn.Tensor:
            data = rng.normal(0.0, std, shape) if std is not None else np.full(shape, value)
            p[name] = nn.parameter(data.astype(np.float32), name)
            return p[name]

        arch = self.spec["arch"]
        if arch == "linear":
            width = BAG_FEATURES
            std = 0.02
        elif arch == "mlp":
            h = self.spec["hidden"]
            add("w1", (BAG_FEATURES, h), 0.05)
            add("b1", (h,))
            width, std = h, 1.0 / np.sqrt(h)
        else:
            d, f = self.spec["d"], self.spec["ffn"]
            s = 1.0 / np.sqrt(d)
            add("emb", (VOCAB_BUCKETS, d), 0.3)
            add("pos", (MAX_TOKENS, d), 0.05)
            for layer in range(self.spec["layers"]):
                k = f"l{layer}."
                add(k + "g1", (d,), value=1.0)
                add(k + "wq", (d, d), s)
                add(k + "wk", (d, d), s)
                add(k + "wv", (d, d), s)
                add(k + "wo", (d, d), s * 0.5)
                add(k + "g2", (d,), value=1.0)
                add(k + "f1", (d, f), s)
                add(k + "fb1", (f,))
                add(k + "f2", (f, d), 0.5 / np.sqrt(f))
                add(k + "fb2", (d,))
            add("gf", (d,), value=1.0)
            width, std = d, s
        for head, size in (("a", len(ACTIONS)), ("o", len(OBJECTS)), ("g", len(GOALS))):
            add("w" + head, (width, size), std)
            add("b" + head, (size,))
        self.params = p
        self.optimizer = nn.Adam(p)

    def describe(self) -> str:
        """Описание архитектуры для экрана «Нейросети»."""
        arch = self.spec["arch"]
        if arch == "linear":
            return f"Линейная модель: {BAG_FEATURES} признаков → 3 головы"
        if arch == "mlp":
            return f"Перцептрон: {BAG_FEATURES} признаков → {self.spec['hidden']} нейронов ReLU → 3 головы"
        return (
            f"Трансформер: {self.spec['layers']} блок(а), d={self.spec['d']}, "
            f"self-attention + FFN {self.spec['ffn']}"
        )

    def param_count(self) -> int:
        return int(sum(t.data.size for t in self.params.values()))

    # ------------------------------------------------------------------ #
    # Прямой проход
    # ------------------------------------------------------------------ #
    def _bag(self, batch: list[list[str]]) -> np.ndarray:
        x = np.zeros((len(batch), BAG_FEATURES), dtype=np.float32)
        for row, tokens in enumerate(batch):
            for i, word in enumerate(tokens):
                for fid in _features(word, BAG_FEATURES):
                    x[row, fid] += 1.0
                if i + 1 < len(tokens):
                    x[row, _pair_feature(word, tokens[i + 1])] += 1.0
            norm = float(np.sqrt((x[row] ** 2).sum()))
            if norm > 0:
                x[row] *= 3.0 / norm
        return x

    def forward(self, batch: list[list[str]]) -> tuple[nn.Tensor, nn.Tensor, nn.Tensor, np.ndarray | None]:
        """Логиты трёх голов для батча фраз и (для трансформера) карта внимания."""
        batch = [tokens[:MAX_TOKENS] or [""] for tokens in batch]
        p = self.params
        arch = self.spec["arch"]
        attention = None
        if arch == "linear":
            h = nn.constant(self._bag(batch))
        elif arch == "mlp":
            h = nn.relu(nn.add(nn.matmul(nn.constant(self._bag(batch)), p["w1"]), p["b1"]))
        else:
            b, t, d = len(batch), max(len(tokens) for tokens in batch), self.spec["d"]
            bag = np.zeros((b * t, VOCAB_BUCKETS), dtype=np.float32)
            mask = np.zeros((b, t), dtype=np.float32)
            for row, tokens in enumerate(batch):
                for j, word in enumerate(tokens):
                    ids = _features(word, VOCAB_BUCKETS)
                    np.add.at(bag[row * t + j], list(ids), 1.0 / len(ids))
                    mask[row, j] = 1.0
            x = nn.reshape(nn.matmul(nn.constant(bag), p["emb"]), (b, t, d))  # мешок признаков слова -> эмбеддинг
            x = nn.add(x, nn.first_rows(p["pos"], t))
            key_mask = np.where(mask[:, None, :] > 0, 0.0, -1e9).astype(np.float32)
            for layer in range(self.spec["layers"]):
                k = f"l{layer}."
                xn = nn.rmsnorm(x, p[k + "g1"])
                q, kk, v = nn.matmul(xn, p[k + "wq"]), nn.matmul(xn, p[k + "wk"]), nn.matmul(xn, p[k + "wv"])
                scores = nn.mul(nn.matmul(q, nn.transpose(kk)), 1.0 / np.sqrt(d))
                att = nn.softmax(scores, key_mask)
                x = nn.add(x, nn.matmul(nn.matmul(att, v), p[k + "wo"]))
                xn2 = nn.rmsnorm(x, p[k + "g2"])
                ff = nn.add(nn.matmul(nn.relu(nn.add(nn.matmul(xn2, p[k + "f1"]), p[k + "fb1"])), p[k + "f2"]), p[k + "fb2"])
                x = nn.add(x, ff)
                attention = att.data
            h = nn.masked_mean(nn.rmsnorm(x, p["gf"]), mask)
        heads = []
        for head in ("a", "o", "g"):
            heads.append(nn.add(nn.matmul(h, p["w" + head]), p["b" + head]))
        return heads[0], heads[1], heads[2], attention

    # ------------------------------------------------------------------ #
    # Обучение
    # ------------------------------------------------------------------ #
    def _loss(self, outs: tuple, targets: np.ndarray, weights: np.ndarray) -> nn.Tensor:
        la, lo, lg, _ = outs
        return nn.total(
            [
                nn.cross_entropy(la, targets[:, 0], weights),
                nn.cross_entropy(lo, targets[:, 1], weights),
                nn.cross_entropy(lg, targets[:, 2], weights * GOAL_WEIGHT),
            ]
        )

    def _train(self, examples: list[Example], lr: float) -> float:
        targets = np.array([ex.intent for ex in examples], dtype=np.int64)
        weights = np.array([ex.weight for ex in examples], dtype=np.float32) / len(examples)
        self.optimizer.zero_grad()
        loss = self._loss(self.forward([ex.tokens for ex in examples]), targets, weights)
        nn.backward(loss)
        self.optimizer.step(lr)
        value = float(loss.data)
        self.stats.online_loss.append(round(value, 4))
        del self.stats.online_loss[:-300]
        return value

    def _replay(self, count: int) -> list[Example]:
        out = []
        for _ in range(count):
            if self.memory and self.random.random() < 0.5:
                out.append(self.random.choice(self.memory))
            else:
                out.append(self.random.choice(self.corpus))
        return out

    def pretrain(self, progress: Callable[[float], None] | None = None) -> None:
        """Предобучение на синтетическом корпусе."""
        self.training = True
        self.stats.loss_history = []
        order = list(range(len(self.corpus)))
        epochs = self.spec["epochs"]
        for epoch in range(epochs):
            self.random.shuffle(order)
            lr = self.spec["lr"] * (1.0 if epoch < epochs - 3 else 0.4)
            losses = []
            for start in range(0, len(order), BATCH):
                batch = [self.corpus[i] for i in order[start : start + BATCH]]
                losses.append(self._train(batch, lr))
                self.progress = (epoch + start / len(order)) / epochs
                if progress:
                    progress(self.progress)
            self.stats.loss_history.append(round(float(np.mean(losses)), 4))
        self.stats.online_loss = []
        self.evaluate()
        self.progress = 1.0
        self.training = False
        self.ready = True

    def evaluate(self) -> None:
        """Точность на отложенной выборке (фразы, которых модель не видела)."""
        if not self.holdout:
            return
        la, lo, lg, _ = self.forward([ex.tokens for ex in self.holdout])
        targets = np.array([ex.intent for ex in self.holdout])
        self.stats.acc_action = float((la.data.argmax(1) == targets[:, 0]).mean())
        self.stats.acc_object = float((lo.data.argmax(1) == targets[:, 1]).mean())
        self.stats.acc_goal = float((lg.data.argmax(1) == targets[:, 2]).mean())

    # ------------------------------------------------------------------ #
    # Рассуждение
    # ------------------------------------------------------------------ #
    def think(self, text: str, objects: list[int] | None = None) -> dict[str, Any]:
        """Разбор фразы с объяснением: гипотезы, вклад слов, внимание, проверка по комнате."""
        tokens = tokenize(text)[:MAX_TOKENS]
        variants = [tokens] + ([tokens[:i] + tokens[i + 1 :] for i in range(len(tokens))] if len(tokens) > 1 else [])
        la, lo, lg, attention = self.forward(variants)
        pa, po, pg = nn.probabilities(la.data), nn.probabilities(lo.data), nn.probabilities(lg.data)
        action, obj, goal = int(pa[0].argmax()), int(po[0].argmax()), int(pg[0].argmax())
        needs_object = action not in NO_OBJECT_ACTIONS
        confidence = float(pa[0, action] * (po[0, obj] if needs_object else 1.0))

        # Окклюзия: убираем по слову и смотрим, насколько падает уверенность в решении.
        importance = [1.0] if len(tokens) == 1 else []
        if len(tokens) > 1:
            drops = (pa[0, action] - pa[1:, action]) + (po[0, obj] - po[1:, obj])
            drops = np.maximum(drops, 0.0)
            s = float(drops.sum())
            importance = [float(x / s) if s > 0 else 0.0 for x in drops]

        att_per_token: list[float] = []
        if attention is not None and tokens:
            n = len(tokens)
            att_per_token = [float(x) for x in attention[0, :n, :n].mean(axis=0)]

        context_object = obj
        object_present = True
        if needs_object and objects is not None and obj not in objects:
            object_present = False
            candidates = [o for o in objects if o != 0]
            if candidates:
                context_object = max(candidates, key=lambda o: po[0, o])

        top = lambda p: [[int(i), round(float(p[i]), 4)] for i in np.argsort(-p)[:3]]  # noqa: E731
        return {
            "tokens": tokens,
            "known": [stem(t) in self.vocab for t in tokens],
            "importance": [round(x, 4) for x in importance],
            "attention": [round(x, 4) for x in att_per_token],
            "intent": [action, obj, goal],
            "confidence": round(confidence, 4),
            "top_action": top(pa[0]),
            "top_object": top(po[0]),
            "top_goal": top(pg[0]),
            "goal_confidence": round(float(pg[0, goal]), 4),
            "context_object": int(context_object),
            "object_present": object_present,
            "floor": self.spec["floor"],
            "corrupted": self.backup is not None,
        }

    # ------------------------------------------------------------------ #
    # Дообучение в игре
    # ------------------------------------------------------------------ #
    def remember(self, tokens: list[str], intent: tuple[int, int, int], weight: float, source: str) -> Example | None:
        if not tokens:
            return None
        for ex in self.memory:
            if ex.tokens == tokens:  # тот же промпт — обновляем метку
                ex.intent, ex.weight, ex.source = intent, max(ex.weight, weight), source
                return ex
        ex = Example(tokens, intent, weight, source)
        self.memory.append(ex)
        del self.memory[:-MEMORY_LIMIT]
        self.vocab.update(stem(t) for t in tokens)
        return ex

    def reinforce(self, text: str, intent: tuple[int, int, int]) -> None:
        """Успех в игре: закрепить такое понимание."""
        if self.backup is not None:
            return
        ex = self.remember(tokenize(text)[:MAX_TOKENS], intent, 1.0, "успех")
        if ex is None:
            return
        for _ in range(3):
            self._train([ex] + self._replay(7), self.spec["lr"] * 0.5)
        self.stats.reinforcements += 1

    def correct(self, text: str, intent: tuple[int, int, int]) -> None:
        """Оператор объяснил, что имел в виду."""
        ex = self.remember(tokenize(text)[:MAX_TOKENS], intent, 2.0, "исправление")
        if ex is None:
            return
        for _ in range(12):
            self._train([ex, ex] + self._replay(6), self.spec["lr"])
        self.stats.corrections += 1

    def discourage(self, text: str, intent: tuple[int, int, int]) -> None:
        """Провал: так понимать эту фразу не стоит."""
        tokens = tokenize(text)[:MAX_TOKENS]
        if self.backup is not None or not tokens:
            return
        for _ in range(3):
            replay = self._replay(6)
            batch = [tokens] + [ex.tokens for ex in replay]
            targets = np.array([intent] + [ex.intent for ex in replay], dtype=np.int64)
            weights = np.array([0.0] + [ex.weight / 6 for ex in replay], dtype=np.float32)
            bad = np.zeros(len(batch), dtype=np.float32)
            bad[0] = 0.5
            self.optimizer.zero_grad()
            outs = self.forward(batch)
            parts = [self._loss(outs, targets, weights), nn.unlikelihood(outs[0], targets[:, 0], bad)]
            if intent[0] not in NO_OBJECT_ACTIONS:
                parts.append(nn.unlikelihood(outs[1], targets[:, 1], bad * 0.6))
            nn.backward(nn.total(parts))
            self.optimizer.step(self.spec["lr"] * 0.5)
        self.stats.discouragements += 1

    def consolidate(self, steps: int) -> None:
        """«Сон»: повторение памяти вперемешку с корпусом."""
        for _ in range(steps):
            self._train(self._replay(BATCH), self.spec["lr"] * 0.4)
        self.evaluate()

    # ------------------------------------------------------------------ #
    # Порча весов (фаза 3 босса)
    # ------------------------------------------------------------------ #
    def corrupt(self, amount: float) -> None:
        if self.backup is None:
            self.backup = {k: t.data.copy() for k, t in self.params.items()}
        rng = np.random.default_rng()
        for t in self.params.values():
            rms = float(np.sqrt((t.data**2).mean())) + 1e-3
            t.data += rng.normal(0.0, rms * amount, t.data.shape).astype(np.float32)

    def heal(self) -> None:
        if self.backup is not None:
            for k, t in self.params.items():
                t.data = self.backup[k]
            self.backup = None

    # ------------------------------------------------------------------ #
    # Сохранение
    # ------------------------------------------------------------------ #
    def _paths(self, directory: str) -> tuple[str, str]:
        base = os.path.join(directory, f"brain{self.level}")
        return base + ".npz", base + ".json"

    def save(self, directory: str) -> None:
        os.makedirs(directory, exist_ok=True)
        weights_path, meta_path = self._paths(directory)
        params = self.backup if self.backup is not None else {k: t.data for k, t in self.params.items()}
        tmp = weights_path + ".tmp.npz"
        np.savez(tmp, **params)
        os.replace(tmp, weights_path)
        meta = {
            "spec": self.spec,
            "stats": {
                "loss_history": self.stats.loss_history,
                "corrections": self.stats.corrections,
                "reinforcements": self.stats.reinforcements,
                "discouragements": self.stats.discouragements,
            },
            "memory": [{"tokens": ex.tokens, "intent": list(ex.intent), "weight": ex.weight, "source": ex.source} for ex in self.memory],
        }
        with open(meta_path + ".tmp", "w", encoding="utf-8") as f:
            json.dump(meta, f, ensure_ascii=False)
        os.replace(meta_path + ".tmp", meta_path)

    def load(self, directory: str) -> bool:
        weights_path, meta_path = self._paths(directory)
        if not os.path.exists(weights_path):
            return False
        try:
            with np.load(weights_path) as data:
                if set(data.files) != set(self.params):
                    return False
                for key, t in self.params.items():
                    if data[key].shape != t.data.shape:
                        return False
                for key, t in self.params.items():
                    t.data = data[key].astype(np.float32)
            if os.path.exists(meta_path):
                with open(meta_path, encoding="utf-8") as f:
                    meta = json.load(f)
                if meta.get("spec") != self.spec:
                    return False
                st = meta.get("stats", {})
                self.stats.loss_history = st.get("loss_history", [])
                self.stats.corrections = st.get("corrections", 0)
                self.stats.reinforcements = st.get("reinforcements", 0)
                self.stats.discouragements = st.get("discouragements", 0)
                for item in meta.get("memory", []):
                    self.remember(item["tokens"], tuple(item["intent"]), float(item["weight"]), item.get("source", "память"))
        except (OSError, ValueError, KeyError, json.JSONDecodeError):
            self._build()
            return False
        self.optimizer = nn.Adam(self.params)
        self.evaluate()
        self.ready = True
        self.progress = 1.0
        return True

    def reset(self) -> None:
        """Забыть всё: новые случайные веса и пустая память."""
        self._build()
        self.memory.clear()
        corpus_size = self.stats.corpus_size
        self.stats = Stats(corpus_size=corpus_size)
        self.backup = None
        self.ready = False
        self.progress = 0.0

    def status(self) -> dict[str, Any]:
        return {
            "level": self.level,
            "arch": self.describe(),
            "params": self.param_count(),
            "ready": self.ready,
            "training": self.training,
            "progress": round(self.progress, 3),
            "corpus": self.stats.corpus_size,
            "memory": len(self.memory),
            "vocab": len(self.vocab),
            "acc": [round(self.stats.acc_action, 4), round(self.stats.acc_object, 4), round(self.stats.acc_goal, 4)],
            "loss_history": self.stats.loss_history,
            "online_loss": self.stats.online_loss[-80:],
            "corrections": self.stats.corrections,
            "reinforcements": self.stats.reinforcements,
            "discouragements": self.stats.discouragements,
            "corrupted": self.backup is not None,
        }
