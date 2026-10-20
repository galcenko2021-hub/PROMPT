"""Мини-движок нейросетей: тензоры с автоматическим дифференцированием, операции и Adam.

numpy используется только для хранения массивов и арифметики над ними. Слои,
обратное распространение ошибки и оптимизатор написаны здесь вручную.
Все операции работают с батчами (ведущие оси — примеры и позиции токенов).
"""
from __future__ import annotations

from typing import Callable, Iterable

import numpy as np

Array = np.ndarray
DTYPE = np.float32


class Tensor:
    """Массив значений + градиент + способ передать градиент родителям."""

    __slots__ = ("data", "grad", "parents", "backward_fn", "requires_grad", "name")

    def __init__(
        self,
        data: Array | float,
        parents: tuple["Tensor", ...] = (),
        backward_fn: Callable[[Array], None] | None = None,
        requires_grad: bool = False,
        name: str = "",
    ) -> None:
        self.data = np.asarray(data, dtype=DTYPE)
        self.grad: Array | None = None
        self.parents = parents
        self.backward_fn = backward_fn
        self.requires_grad = requires_grad or any(p.requires_grad for p in parents)
        self.name = name

    @property
    def shape(self) -> tuple[int, ...]:
        return self.data.shape

    def accumulate(self, grad: Array) -> None:
        """Добавляет градиент (с учётом broadcasting)."""
        if not self.requires_grad:
            return
        grad = _unbroadcast(grad, self.data.shape)
        self.grad = grad.astype(DTYPE, copy=False) if self.grad is None else self.grad + grad


def parameter(data: Array, name: str) -> Tensor:
    """Обучаемый параметр."""
    return Tensor(data, requires_grad=True, name=name)


def constant(data: Array) -> Tensor:
    """Тензор без градиента (входные данные, маски)."""
    return Tensor(data)


def _unbroadcast(grad: Array, shape: tuple[int, ...]) -> Array:
    """Сворачивает градиент обратно к форме операнда после broadcasting."""
    while grad.ndim > len(shape):
        grad = grad.sum(axis=0)
    for axis, size in enumerate(shape):
        if size == 1 and grad.shape[axis] != 1:
            grad = grad.sum(axis=axis, keepdims=True)
    return grad


def _node(data: Array, parents: tuple[Tensor, ...], backward_fn: Callable[[Array], None]) -> Tensor:
    return Tensor(data, parents, backward_fn)


# --------------------------------------------------------------------------- #
# Операции
# --------------------------------------------------------------------------- #
def matmul(a: Tensor, b: Tensor) -> Tensor:
    """Матричное (в том числе батчевое) произведение."""
    out = a.data @ b.data

    def backward(g: Array) -> None:
        if a.requires_grad:
            a.accumulate(g @ np.swapaxes(b.data, -1, -2))
        if b.requires_grad:
            b.accumulate(np.swapaxes(a.data, -1, -2) @ g)

    return _node(out, (a, b), backward)


def add(a: Tensor, b: Tensor) -> Tensor:
    """Сумма с broadcasting."""

    def backward(g: Array) -> None:
        a.accumulate(g)
        b.accumulate(g)

    return _node(a.data + b.data, (a, b), backward)


def mul(a: Tensor, k: Array | float) -> Tensor:
    """Умножение на константу (число или массив без градиента)."""
    k_arr = np.asarray(k, dtype=DTYPE)

    def backward(g: Array) -> None:
        a.accumulate(g * k_arr)

    return _node(a.data * k_arr, (a,), backward)


def relu(a: Tensor) -> Tensor:
    mask = a.data > 0

    def backward(g: Array) -> None:
        a.accumulate(g * mask)

    return _node(a.data * mask, (a,), backward)


def transpose(a: Tensor) -> Tensor:
    """Меняет местами две последние оси."""

    def backward(g: Array) -> None:
        a.accumulate(np.swapaxes(g, -1, -2))

    return _node(np.swapaxes(a.data, -1, -2), (a,), backward)


def softmax(a: Tensor, additive_mask: Array | None = None) -> Tensor:
    """Softmax по последней оси; маска добавляется к логитам (например, -1e9 для паддинга)."""
    x = a.data if additive_mask is None else a.data + additive_mask
    x = x - x.max(axis=-1, keepdims=True)
    e = np.exp(x)
    y = e / e.sum(axis=-1, keepdims=True)

    def backward(g: Array) -> None:
        dot = (g * y).sum(axis=-1, keepdims=True)
        a.accumulate(y * (g - dot))

    return _node(y, (a,), backward)


def rmsnorm(a: Tensor, gain: Tensor, eps: float = 1e-5) -> Tensor:
    """Нормировка по последней оси: x / rms(x) * gain."""
    x = a.data
    d = x.shape[-1]
    inv = 1.0 / np.sqrt((x * x).mean(axis=-1, keepdims=True) + eps)
    out = x * inv * gain.data

    def backward(g: Array) -> None:
        gg = g * gain.data
        dot = (gg * x).sum(axis=-1, keepdims=True)
        a.accumulate(gg * inv - x * inv**3 * dot / d)
        gain.accumulate(g * x * inv)

    return _node(out, (a, gain), backward)


def masked_mean(a: Tensor, mask: Array) -> Tensor:
    """Среднее по оси токенов (B, T, d) -> (B, d) только по реальным токенам."""
    m = mask[..., None].astype(DTYPE)
    count = np.maximum(m.sum(axis=1), 1.0)
    out = (a.data * m).sum(axis=1) / count

    def backward(g: Array) -> None:
        a.accumulate(g[:, None, :] * m / count[:, None, :])

    return _node(out, (a,), backward)


def first_rows(a: Tensor, n: int) -> Tensor:
    """Первые n строк параметра (позиционные эмбеддинги)."""

    def backward(g: Array) -> None:
        full = np.zeros_like(a.data)
        full[:n] = _unbroadcast(g, full[:n].shape)
        a.accumulate(full)

    return _node(a.data[:n], (a,), backward)


def reshape(a: Tensor, shape: tuple[int, ...]) -> Tensor:
    def backward(g: Array) -> None:
        a.accumulate(g.reshape(a.data.shape))

    return _node(a.data.reshape(shape), (a,), backward)


def total(parts: Iterable[Tensor]) -> Tensor:
    """Сумма скалярных потерь."""
    parts = tuple(parts)

    def backward(g: Array) -> None:
        for p in parts:
            p.accumulate(g)

    return _node(np.asarray(sum(float(p.data) for p in parts), dtype=DTYPE), parts, backward)


# --------------------------------------------------------------------------- #
# Потери
# --------------------------------------------------------------------------- #
def probabilities(logits: Array) -> Array:
    """Softmax по последней оси для обычного массива."""
    x = logits - logits.max(axis=-1, keepdims=True)
    e = np.exp(x)
    return e / e.sum(axis=-1, keepdims=True)


def cross_entropy(logits: Tensor, targets: Array, weights: Array) -> Tensor:
    """Взвешенная кросс-энтропия по батчу (сумма, веса уже нормированы)."""
    p = probabilities(logits.data)
    rows = np.arange(len(targets))
    loss = -(weights * np.log(np.maximum(p[rows, targets], 1e-8))).sum()

    def backward(g: Array) -> None:
        grad = p.copy()
        grad[rows, targets] -= 1.0
        logits.accumulate(g * grad * weights[:, None])

    return _node(np.asarray(loss, dtype=DTYPE), (logits,), backward)


def unlikelihood(logits: Tensor, index: Array, weights: Array) -> Tensor:
    """-log(1 - p[index]): «так отвечать не надо»."""
    p = probabilities(logits.data)
    rows = np.arange(len(index))
    q = np.minimum(p[rows, index], 0.999)
    loss = -(weights * np.log(1.0 - q)).sum()

    def backward(g: Array) -> None:
        delta = np.zeros_like(p)
        delta[rows, index] = 1.0
        coef = (weights * q / (1.0 - q))[:, None]
        logits.accumulate(g * coef * (delta - p))

    return _node(np.asarray(loss, dtype=DTYPE), (logits,), backward)


def backward(loss: Tensor) -> None:
    """Обратный проход: топологическая сортировка графа и вызов функций в обратном порядке."""
    order: list[Tensor] = []
    seen: set[int] = set()
    stack: list[tuple[Tensor, int]] = [(loss, 0)]
    seen.add(id(loss))
    while stack:
        node, index = stack.pop()
        if index < len(node.parents):
            stack.append((node, index + 1))
            parent = node.parents[index]
            if id(parent) not in seen and parent.requires_grad:
                seen.add(id(parent))
                stack.append((parent, 0))
        else:
            order.append(node)
    loss.grad = np.ones_like(loss.data)
    for node in reversed(order):
        if node.backward_fn is not None and node.grad is not None:
            node.backward_fn(node.grad)


# --------------------------------------------------------------------------- #
# Оптимизатор
# --------------------------------------------------------------------------- #
class Adam:
    """Adam с ограничением общей нормы градиента."""

    def __init__(self, params: dict[str, Tensor]) -> None:
        self.params = params
        self.m = {k: np.zeros_like(p.data) for k, p in params.items()}
        self.v = {k: np.zeros_like(p.data) for k, p in params.items()}
        self.step_count = 0

    def zero_grad(self) -> None:
        for p in self.params.values():
            p.grad = None

    def step(self, lr: float, clip: float = 1.0, beta1: float = 0.9, beta2: float = 0.999, eps: float = 1e-8) -> None:
        grads = {k: p.grad for k, p in self.params.items() if p.grad is not None}
        norm = float(np.sqrt(sum(float((g * g).sum()) for g in grads.values()))) if grads else 0.0
        scale = clip / norm if norm > clip else 1.0
        self.step_count += 1
        c1 = 1.0 - beta1**self.step_count
        c2 = 1.0 - beta2**self.step_count
        for key, g in grads.items():
            g = g * scale
            self.m[key] = beta1 * self.m[key] + (1 - beta1) * g
            self.v[key] = beta2 * self.v[key] + (1 - beta2) * g * g
            self.params[key].data -= lr * (self.m[key] / c1) / (np.sqrt(self.v[key] / c2) + eps)
