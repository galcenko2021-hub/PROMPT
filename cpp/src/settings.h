// Все константы игры: экран, цвета, скорости, Помощники, головоломки, босс.
#pragma once

#include <string>

#include "core.h"

namespace cfg {

// --- экран -------------------------------------------------------------------
constexpr int SCREEN_W = 1280;
constexpr int SCREEN_H = 720;
inline const Vec2 SCREEN_CENTER{SCREEN_W / 2.f, SCREEN_H / 2.f};
constexpr float MAX_FRAME_TIME = 1.f / 30.f;

// Псевдо-3D: доля, на которую «крыша» блока смещается от центра экрана.
constexpr float WALL_HEIGHT = 0.075f;
constexpr float GATE_HEIGHT = 0.075f;
constexpr float GLASS_HEIGHT = 0.045f;
constexpr float DEVICE_HEIGHT = 0.035f;
constexpr float BOX_HEIGHT = 0.04f;

// --- палитра -----------------------------------------------------------------
inline const Color BG = Color::hex(0x0d0d0d);
inline const Color WALL = Color::hex(0x1a1a2e);
inline const Color WALL_SIDE = Color::rgb(24, 24, 44);
inline const Color WALL_EDGE = Color::rgb(88, 96, 255);
inline const Color FLOOR = Color::rgb(17, 17, 24);
inline const Color FLOOR_ALT = Color::rgb(19, 19, 28);
inline const Color GRID = Color::rgb(30, 32, 52);
inline const Color FIELD = Color::rgb(24, 14, 22);
inline const Color FIELD_GRID = Color::rgb(60, 26, 48);
inline const Color PLAYER = Color::hex(0x4d6bfe);
inline const Color TEXT = Color::rgb(225, 230, 245);
inline const Color DIM = Color::rgb(120, 126, 158);
inline const Color ACCENT = Color::rgb(0, 255, 200);
inline const Color SYSTEM = Color::rgb(140, 160, 255);
inline const Color DANGER = Color::rgb(255, 60, 90);
inline const Color SUCCESS = Color::rgb(60, 255, 140);
inline const Color WARNING = Color::rgb(255, 205, 60);
inline const Color GLASS = Color::rgb(90, 200, 255);
inline const Color TERMINAL = Color::rgb(0, 230, 255);
inline const Color BOX = Color::rgb(255, 150, 60);
inline const Color SENTINEL = Color::rgb(255, 215, 70);
inline const Color BOSS = Color::rgb(255, 70, 200);
inline const Color THOUGHT = Color::rgb(170, 140, 255);

// --- движение и взаимодействие -------------------------------------------------
constexpr float PLAYER_SPEED = 230.f;
constexpr float PLAYER_SIZE = 26.f;
constexpr float ASSISTANT_SPEED = 265.f;
constexpr float ASSISTANT_TASK_SPEED = 210.f;
constexpr float PUSH_SPEED = 150.f;
constexpr float INTERACT_RANGE = TILE * 1.35f;
constexpr float KEY_INTERACT_RANGE = TILE * 2.4f;
constexpr float FOLLOW_DISTANCE = TILE * 1.3f;
constexpr float REPATH_INTERVAL = 0.35f;
constexpr float CATCHUP_DISTANCE = TILE * 11.f;
constexpr float CATCHUP_TIME = 2.5f;
constexpr float RESPAWN_INVULNERABILITY = 1.4f;

// --- стелс и опасности ---------------------------------------------------------
constexpr float SENTINEL_SPEED = 85.f;
constexpr float SENTINEL_TURN_SPEED = 3.2f;
constexpr float SENTINEL_VIEW_RANGE = TILE * 4.6f;
constexpr float SENTINEL_FOV = 64.f * PI / 180.f;
constexpr float SENTINEL_PAUSE = 1.1f;
constexpr float DETECTION_TIME = 0.5f;
constexpr float MINE_REVEAL_RADIUS = TILE * 1.15f;
constexpr float SHADOW_RADIUS = TILE * 2.8f;
constexpr float WIND_SPEED = 330.f;

// --- босс ----------------------------------------------------------------------
constexpr int BOSS_MAX_HP = 3;
constexpr float BOSS_RADIUS = 66.f;
constexpr float CONTROL_SWAP_DURATION = 5.f;
constexpr float CONTROL_SWAP_INTERVAL = 7.f;
constexpr float FLIP_ON_DURATION = 6.f;
constexpr float FLIP_OFF_DURATION = 3.5f;
constexpr float CORRUPTION_INTERVAL = 9.f;
constexpr float BUG_SPEED = 62.f;

constexpr float ORACLE_REFUSAL_CHANCE = 0.2f;
constexpr float MESSAGE_DURATION = 6.f;

}  // namespace cfg

// --- головоломки ----------------------------------------------------------------
enum class PuzzleKind { PushBox, ButtonSequence, RouteMemory, LinkObjects, ContextTask };

inline const wchar_t* puzzleTitle(PuzzleKind k) {
    switch (k) {
        case PuzzleKind::PushBox: return L"Толкни ящик";
        case PuzzleKind::ButtonSequence: return L"Последовательность кнопок";
        case PuzzleKind::RouteMemory: return L"Память маршрута";
        case PuzzleKind::LinkObjects: return L"Связь объектов";
        default: return L"Контекстная задача";
    }
}

inline int puzzleMinLevel(PuzzleKind k) {
    switch (k) {
        case PuzzleKind::PushBox:
        case PuzzleKind::ButtonSequence: return 1;
        case PuzzleKind::RouteMemory: return 2;
        case PuzzleKind::LinkObjects: return 3;
        default: return 4;
    }
}

// --- Помощники -------------------------------------------------------------------
inline const wchar_t* const ASSISTANT_NAMES[6] = {L"", L"Пиксель", L"Эхо", L"Вектор", L"Трансформер", L"Оракул"};
inline const Color ASSISTANT_COLORS[6] = {Color::hex(0xffffff), Color::hex(0x00ff88), Color::hex(0xffaa00),
                                          Color::hex(0xff00aa), Color::hex(0x00aaff), Color::hex(0xffffff)};
inline const wchar_t* const ASSISTANT_SHAPES[6] = {L"", L"квадрат", L"круг", L"треугольник", L"шестиугольник", L"звезда"};
inline const wchar_t* const ASSISTANT_DESCRIPTIONS[6] = {
    L"",
    L"Простые действия: толкает ящики и нажимает кнопки. Линейная нейросеть.",
    L"Запоминает маршрут, который ты ему показал. Перцептрон на 24 нейрона.",
    L"Связывает объекты в разных комнатах. Перцептрон на 48 нейронов.",
    L"Понимает контекст: что сделать и зачем. Трансформер с self-attention.",
    L"Решает всё, но иногда считает задачу плохо сформулированной. Двухслойный трансформер.",
};
inline const wchar_t* const ASSISTANT_REFUSALS[6] = {
    L"",
    L"Я понял задачу, но умею только толкать и нажимать.",
    L"Понял, но я могу лишь повторять то, что видел.",
    L"Понял, но смысл таких задач мне недоступен.",
    L"Понимаю, но это мне не по силам.",
    L"Даже Оракул не всесилен.",
};
