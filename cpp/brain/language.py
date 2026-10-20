"""Язык Помощников: токенизация русских промптов, намерения и генератор корпуса.

Намерение = (действие, объект, цель). Нумерация совпадает с ``cpp/src/intent.h``.
"""
from __future__ import annotations

import random
from dataclasses import dataclass, field

# --- намерения (порядок важен: C++ использует те же номера) ------------------
ACTIONS = ["NONE", "PUSH", "PRESS", "ROUTE", "LINK", "ON", "OFF", "EXPLAIN", "BREAK", "COME", "STAY"]
OBJECTS = ["NONE", "BOX", "BUTTONS", "ROUTE", "KEY", "FAN", "LIGHT", "SHADOW", "DOOR", "BOSS", "ME"]
GOALS = ["NONE", "PASS", "ENEMY", "UNDERSTAND", "BAD"]
A = {name: i for i, name in enumerate(ACTIONS)}
O = {name: i for i, name in enumerate(OBJECTS)}
G = {name: i for i, name in enumerate(GOALS)}

NO_OBJECT_ACTIONS = {A["NONE"], A["COME"], A["STAY"]}


@dataclass
class Example:
    """Обучающий пример: токены промпта и правильное намерение."""

    tokens: list[str]
    intent: tuple[int, int, int]
    weight: float = 1.0
    source: str = "corpus"


# --- словари перефразировок --------------------------------------------------
VERBS: dict[str, list[str]] = {
    "PUSH": ["толкни", "подвинь", "задвинь", "толкай", "пихни", "передвинь", "сдвинь", "толкнуть", "подвинуть", "задвинуть",
             "двигай", "переставь", "допихай"],
    "PRESS": ["нажми", "жми", "нажимай", "нажать", "прожми", "тыкни", "понажимай", "надави на"],
    "ROUTE": ["повтори", "пройди", "иди по", "следуй по", "пройди по", "проделай", "прошагай по", "повторить"],
    "LINK": ["свяжи", "соедини", "подключи", "привяжи", "связать", "сопоставь", "соединить"],
    "ON": ["включи", "зажги", "вруби", "включить", "запусти", "зажигай", "дай"],
    "OFF": ["выключи", "отключи", "выруби", "останови", "погаси", "выключить", "заглуши", "отключить", "тормозни"],
    "EXPLAIN": ["объясни", "научи", "растолкуй", "объяснить", "втолкуй", "разъясни"],
    "BREAK": ["сломай", "разбей", "уничтожь", "сломать", "взорви", "разнеси"],
    "COME": ["иди ко мне", "подойди", "иди сюда", "за мной", "следуй за мной", "ко мне", "вернись", "догоняй"],
    "STAY": ["стой", "жди", "замри", "останься тут", "подожди", "стоп", "не двигайся", "оставайся здесь"],
}
OBJECT_PHRASES: dict[str, list[str]] = {
    "BOX": ["ящик", "коробку", "куб", "ящик на плиту", "груз", "ящичек", "коробку на плиту", "ящик на кнопку"],
    "BUTTONS": ["кнопки", "кнопку", "кнопки по порядку", "кнопки в нужном порядке", "все кнопки", "клавиши"],
    "ROUTE": ["маршрут", "путь", "дорогу", "мой маршрут", "мой путь", "записанный путь", "маршруту", "моему маршруту",
              "моим следам", "пути"],
    "KEY": ["ключ с замком", "ключ и замок", "ключ", "замок", "ключ к замку", "ключ с дверью"],
    "FAN": ["вентилятор", "кулер", "пропеллер", "ветер", "вентиль", "большой вентилятор"],
    "LIGHT": ["свет", "лампы", "освещение", "лампу", "прожектор", "фонари"],
    "SHADOW": ["тень", "врага", "монстра", "чудище"],
    "DOOR": ["дверь", "выход", "проход", "ворота"],
    "BOSS": ["вайбкодеру", "боссу", "ему", "этому кодеру", "вайбкодеру как писать код"],
}
GOAL_PHRASES: dict[str, list[str]] = {
    "PASS": ["чтобы пройти", "чтобы пройти по коридору", "чтобы пройти мимо", "чтобы открыть путь", "чтобы я прошел",
             "для прохода", "чтобы попасть дальше", "чтобы мы прошли"],
    "ENEMY": ["чтобы враг исчез", "чтобы тень исчезла", "чтобы прогнать тень", "чтобы победить тень", "чтобы монстр пропал",
              "чтобы враг ушел"],
    "UNDERSTAND": ["чтобы он понимал что пишет", "чтобы он понимал код", "чтобы он научился", "чтобы он писал осмысленно",
                   "чтобы он понял свой код", "чтобы он разобрался"],
    "BAD": ["просто так", "чтобы было быстрее", "чтобы никто не заметил", "чтобы стало громче", "чтобы стало темнее",
            "ради шутки", "на всякий случай", "чтобы было весело"],
}
PREFIXES = ["пожалуйста", "давай", "эй", "слушай", "можешь", "нужно", "надо", "срочно", "ну",
            "пиксель", "эхо", "вектор", "трансформер", "оракул", "помощник"]
SUFFIXES = ["пожалуйста", "сейчас", "срочно", "ладно", "спасибо"]
CHITCHAT = ["привет", "как дела", "что ты умеешь", "ты кто", "спасибо", "молодец", "хм", "ок", "понятно", "отлично",
            "кто ты такой", "ничего", "забудь", "неважно", "ха ха", "сколько времени", "скучно", "я устал", "интересно", "где я"]

# (действие, объекты, цели — повтор цели значит «чаще встречается»)
SPECS: list[tuple[str, list[str], list[str]]] = [
    ("PUSH", ["BOX"], ["NONE", "NONE", "PASS"]),
    ("PRESS", ["BUTTONS"], ["NONE", "NONE", "PASS"]),
    ("ROUTE", ["ROUTE"], ["NONE", "NONE", "PASS"]),
    ("LINK", ["KEY"], ["NONE", "NONE", "PASS"]),
    ("OFF", ["FAN"], ["PASS", "PASS", "NONE", "BAD"]),
    ("ON", ["FAN"], ["BAD", "NONE", "PASS"]),
    ("ON", ["LIGHT"], ["ENEMY", "ENEMY", "PASS", "NONE", "BAD"]),
    ("OFF", ["LIGHT"], ["BAD", "NONE", "ENEMY"]),
    ("EXPLAIN", ["BOSS"], ["UNDERSTAND", "UNDERSTAND", "NONE", "BAD"]),
    ("BREAK", ["BOX", "DOOR", "SHADOW", "FAN", "LIGHT"], ["NONE", "BAD", "PASS"]),
    ("COME", ["ME"], ["NONE"]),
    ("STAY", ["NONE"], ["NONE"]),
]


# --- токенизация -------------------------------------------------------------
def normalize_char(c: str) -> str:
    c = c.lower()
    return "е" if c == "ё" else c


def tokenize(text: str) -> list[str]:
    """Строчные буквы, ё -> е; всё, кроме букв и цифр, — разделители."""
    tokens: list[str] = []
    current: list[str] = []
    for ch in text:
        c = normalize_char(ch)
        if ("a" <= c <= "z") or ("0" <= c <= "9") or ("а" <= c <= "я"):
            current.append(c)
        elif current:
            tokens.append("".join(current))
            current = []
    if current:
        tokens.append("".join(current))
    return tokens


def stem(word: str) -> str:
    """Грубая основа слова — первые 6 букв (хватает для русских окончаний команд)."""
    return word[:6]


def fnv1a(text: str, seed: int) -> int:
    h = (2166136261 ^ ((seed * 16777619) & 0xFFFFFFFF)) & 0xFFFFFFFF
    for ch in text:
        h ^= ord(ch)
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def word_feature_ids(word: str, buckets: int) -> list[int]:
    """Признаки слова: основа, само слово и символьные триграммы (устойчивость к опечаткам)."""
    ids = [fnv1a(stem(word), 1) % buckets, fnv1a(word, 3) % buckets]
    padded = f"^{word}$"
    ids.extend(fnv1a(padded[i : i + 3], 2) % buckets for i in range(len(padded) - 2))
    return ids


def action_in_curriculum(level: int, action: str) -> bool:
    """Какие действия Помощник уровня level изучает при предобучении."""
    if action == "ROUTE":
        return level >= 2
    if action == "LINK":
        return level >= 3
    if action in ("ON", "OFF", "EXPLAIN"):
        return level >= 4
    return True


# --- генератор корпуса -------------------------------------------------------
def _typo(word: str, rng: random.Random) -> str:
    if len(word) < 4:
        return word
    pos = rng.randint(1, len(word) - 2)
    kind = rng.randint(0, 2)
    if kind == 0:
        return word[:pos] + word[pos + 1 :]
    if kind == 1:
        return word[:pos] + word[pos + 1] + word[pos] + word[pos + 2 :]
    return word[:pos] + word[pos] + word[pos:]


def _example(action: str, obj: str, goal: str, rng: random.Random) -> Example:
    verb = tokenize(rng.choice(VERBS[action])) if action in VERBS else []
    noun = tokenize(rng.choice(OBJECT_PHRASES[obj])) if obj in OBJECT_PHRASES else []
    purpose = tokenize(rng.choice(GOAL_PHRASES[goal])) if goal in GOAL_PHRASES else []
    prefix = tokenize(rng.choice(PREFIXES)) if rng.random() < 0.35 else []
    suffix = tokenize(rng.choice(SUFFIXES)) if rng.random() < 0.2 else []
    order = rng.random()
    if order < 0.7 or not purpose:
        body = verb + noun + purpose
    elif order < 0.85:
        body = purpose + verb + noun
    else:
        body = noun + verb + purpose
    tokens = [(_typo(w, rng) if rng.random() < 0.07 else w) for w in prefix + body + suffix]
    return Example(tokens, (A[action], O[obj], G[goal]))


def generate_corpus(seed: int, level: int, per_combination: int) -> list[Example]:
    """Перефразировки команд с синонимами, перестановками и опечатками.

    Команды вне «учебной программы» уровня младшие модели учатся честно не понимать.
    """
    rng = random.Random(seed)
    out: list[Example] = []
    for action, objects, goals in SPECS:
        known = action_in_curriculum(level, action)
        for obj in objects:
            for goal in goals:
                count = per_combination if known else max(4, per_combination // 3)
                for _ in range(count):
                    ex = _example(action, obj, goal, rng)
                    if not known:
                        ex.intent = (0, 0, 0)
                        ex.weight = 0.6
                    out.append(ex)
    for _ in range(per_combination * 2):
        tokens = (tokenize(rng.choice(PREFIXES)) if rng.random() < 0.3 else []) + tokenize(rng.choice(CHITCHAT))
        out.append(Example(tokens, (0, 0, 0)))
    rng.shuffle(out)
    return out


@dataclass
class Stats:
    """Статистика обучения модели."""

    corpus_size: int = 0
    acc_action: float = 0.0
    acc_object: float = 0.0
    acc_goal: float = 0.0
    loss_history: list[float] = field(default_factory=list)
    online_loss: list[float] = field(default_factory=list)
    corrections: int = 0
    reinforcements: int = 0
    discouragements: int = 0
