// Данные уровней: карты, легенды, головоломки, Помощники, апгрейды и тексты.
//
// Общие символы карты: '#' стена, ' ' пропасть, '.' пол, '=' стекло,
// ',' повреждённый сектор, 'x' сектор с «миной», 'P' старт, 'T' терминал, 'D' выход.
// Остальные символы описывает legend уровня: символ -> (вид, id головоломки, индекс).
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core.h"
#include "intent.h"
#include "settings.h"

using Dialog = std::vector<std::pair<std::wstring, std::wstring>>;

struct LegendEntry {
    std::string kind;  // box, plate, button, panel, pad, goal, key, lock, console, switch, switch_alt, fan, wind,
                       // shadow, dark, gate, waypoint, boss
    std::string id;
    int index = 0;
};

struct Zone {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    bool valid() const { return x1 >= x0 && y1 >= y0; }
    bool contains(Tile t) const { return t.x >= x0 && t.x <= x1 && t.y >= y0 && t.y <= y1; }
};

struct ContextDef {
    std::string scenario;  // fan, shadow, boss
    Vec2 windDir{-1, 0};
    std::wstring description;
    std::wstring hint;
    std::wstring success;
    int action = A_NONE;
    int object = O_NONE;
    std::vector<int> goals;
};

struct PuzzleDef {
    std::string id;
    PuzzleKind kind = PuzzleKind::PushBox;
    std::vector<int> order;       // ButtonSequence
    std::string shape;            // LinkObjects: triangle, diamond, circle
    Color color = cfg::WARNING;   // LinkObjects
    std::wstring symbol;          // LinkObjects
    ContextDef ctx;               // ContextTask
    Zone zone;                    // где можно переставлять объекты после смерти
};

struct LevelDef {
    int number = 1;
    std::wstring title, subtitle;
    std::vector<int> available;
    int unlocked = 1;
    int upgrade = 0;
    std::wstring upgradeText;
    Dialog intro;
    std::vector<std::string> map;
    std::map<char, LegendEntry> legend;
    std::vector<PuzzleDef> puzzles;
    std::vector<std::string> bossPhases;
};

const std::vector<LevelDef>& levels();
const Dialog& introStory();
const std::wstring& finalText();
const std::vector<std::wstring>& credits();
