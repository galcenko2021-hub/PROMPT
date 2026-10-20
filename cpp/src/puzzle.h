// Пять типов головоломок. Оператор готовит задачу (отмечает кнопки, пишет маршрут,
// указывает ключ) и формулирует промпт, а выполняет её Помощник. После каждой
// смерти Оператора нерешённые головоломки переставляются (randomize).
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "assistant.h"
#include "gfx.h"
#include "intent.h"
#include "levels.h"

class GameAPI;
class Room;

struct Interactable {
    Vec2 pos;
    std::wstring label;
    std::function<void(GameAPI&)> action;
    float radius = cfg::INTERACT_RANGE;
    Color color = cfg::TEXT;
};

using Elements = std::map<std::string, std::vector<std::pair<Tile, int>>>;

class Puzzle {
public:
    Puzzle(PuzzleKind kind, Room& room, const PuzzleDef& def, const Elements& elements, int index);
    virtual ~Puzzle() = default;

    PuzzleKind kind;
    Room& room;
    const PuzzleDef& def;
    int index;
    bool solved = false;
    bool active = true;
    bool inProgress = false;
    std::wstring lastError;
    int solverLevel = 0;

    virtual Vec2 anchor() const = 0;
    virtual bool checkSolved() const = 0;
    virtual bool matches(const Intent& intent) const = 0;     // относится ли намерение к этой головоломке
    virtual std::vector<int> objects() const = 0;             // объекты для контекста нейросети
    virtual bool buildPlan(Assistant& a, const Intent& intent, Script& out) = 0;
    virtual std::vector<Interactable> interactables() = 0;
    virtual std::wstring promptHint() const = 0;
    virtual void solidTiles(TileSet&) const {}
    virtual void blockingTiles(TileSet&) const {}
    virtual void blocks(std::vector<gfx::Block>&) const {}
    virtual void update(float, GameAPI&) {}
    virtual void drawFloor(Renderer&, Vec2, float) {}
    virtual void drawOverlay(Renderer&, Vec2, float) {}
    virtual void cancel() { inProgress = false; }
    virtual void resetTransient() {}
    virtual bool randomize(Rng&, const TileSet&) { return false; }
    virtual void occupiedTiles(TileSet&) const {}  // важные клетки (не ставить сюда чужие объекты)

    const wchar_t* title() const { return puzzleTitle(kind); }

protected:
    Tile element(const std::string& kind) const;
    std::vector<Tile> elements(const std::string& kind) const;
    void accept(Assistant& a);
    void notify(const std::wstring& text, const std::wstring& speaker = L"СИСТЕМА", Color c = cfg::SYSTEM) const;
    static gfx::Block deviceBlock(Tile t, Color c, float height = cfg::DEVICE_HEIGHT, float shrink = 8.f);
    std::function<void(GameAPI&)> promptAction() const;

    Elements elements_;
};

// --- 1. PushBox ------------------------------------------------------------------
class PushBox : public Puzzle {
public:
    PushBox(Room& room, const PuzzleDef& def, const Elements& el, int index);
    Tile box, target;
    Vec2 boxPos;

    Vec2 anchor() const override { return boxPos; }
    bool checkSolved() const override;
    bool matches(const Intent& in) const override { return in.action == A_PUSH && in.object == O_BOX; }
    std::vector<int> objects() const override { return {O_BOX}; }
    bool buildPlan(Assistant& a, const Intent& intent, Script& out) override;
    std::vector<Interactable> interactables() override;
    std::wstring promptHint() const override { return L"например: «толкни ящик на плиту»"; }
    void blockingTiles(TileSet& out) const override { out.insert(box); }
    void blocks(std::vector<gfx::Block>& out) const override;
    void update(float dt, GameAPI&) override;
    void drawFloor(Renderer& r, Vec2 cam, float t) override;
    bool randomize(Rng& rng, const TileSet& avoid) override;
    void occupiedTiles(TileSet& out) const override {
        out.insert(box);
        out.insert(target);
    }

    // Решение «сокобана» для одного ящика: ходы (направление, толчок?) от клетки pusher.
    bool planMoves(Tile pusher, Tile boxTile, Tile goal, std::vector<std::pair<Tile, bool>>& moves) const;
};

// --- 2. ButtonSequence -------------------------------------------------------------
class ButtonSequence : public Puzzle {
public:
    ButtonSequence(Room& room, const PuzzleDef& def, const Elements& el, int index);
    std::vector<Tile> buttons;
    Tile panel;
    std::vector<int> order, marks, pressed, demo;
    std::map<int, float> flash;
    float demoTimer = 0.f, failTimer = 0.f;
    bool complete = false;

    Vec2 anchor() const override { return tileCenter(panel); }
    bool checkSolved() const override { return complete; }
    bool matches(const Intent& in) const override { return in.action == A_PRESS && in.object == O_BUTTONS; }
    std::vector<int> objects() const override { return {O_BUTTONS}; }
    bool buildPlan(Assistant& a, const Intent& intent, Script& out) override;
    std::vector<Interactable> interactables() override;
    std::wstring promptHint() const override { return L"например: «нажми кнопки по порядку»"; }
    void solidTiles(TileSet& out) const override { out.insert(panel); }
    void blocks(std::vector<gfx::Block>& out) const override;
    void update(float dt, GameAPI&) override;
    void drawFloor(Renderer& r, Vec2 cam, float t) override;
    void drawOverlay(Renderer& r, Vec2 cam, float t) override;
    void cancel() override;
    bool randomize(Rng& rng, const TileSet& avoid) override;
    void occupiedTiles(TileSet& out) const override {
        for (const Tile& b : buttons) out.insert(b);
        out.insert(panel);
    }

private:
    void onButton(GameAPI& g, int i);
    void evaluate();
};

// --- 3. RouteMemory ------------------------------------------------------------------
class RouteMemory : public Puzzle {
public:
    RouteMemory(Room& room, const PuzzleDef& def, const Elements& el, int index);
    Tile pad, goal;
    bool recording = false, ready = false, complete = false;
    std::vector<Tile> route;

    Vec2 anchor() const override { return tileCenter(pad); }
    bool checkSolved() const override { return complete; }
    bool matches(const Intent& in) const override { return in.action == A_ROUTE && in.object == O_ROUTE; }
    std::vector<int> objects() const override { return {O_ROUTE}; }
    bool buildPlan(Assistant& a, const Intent& intent, Script& out) override;
    std::vector<Interactable> interactables() override;
    std::wstring promptHint() const override { return L"например: «повтори мой маршрут»"; }
    void update(float dt, GameAPI&) override;
    void drawFloor(Renderer& r, Vec2 cam, float t) override;
    void resetTransient() override { stopRecording(); }
    bool randomize(Rng& rng, const TileSet& avoid) override;
    void occupiedTiles(TileSet& out) const override {
        out.insert(pad);
        out.insert(goal);
    }
    void stopRecording();
};

// --- 4. LinkObjects ------------------------------------------------------------------
class LinkObjects : public Puzzle {
public:
    LinkObjects(Room& room, const PuzzleDef& def, const Elements& el, int index);
    Tile key, lock;
    bool linked = false, complete = false;
    float progress = 0.f;

    Vec2 anchor() const override { return tileCenter(lock); }
    bool checkSolved() const override { return complete; }
    bool matches(const Intent& in) const override { return in.action == A_LINK && in.object == O_KEY; }
    std::vector<int> objects() const override { return {O_KEY}; }
    bool buildPlan(Assistant& a, const Intent& intent, Script& out) override;
    std::vector<Interactable> interactables() override;
    std::wstring promptHint() const override { return L"например: «свяжи ключ с замком»"; }
    void solidTiles(TileSet& out) const override {
        out.insert(key);
        out.insert(lock);
    }
    void blocks(std::vector<gfx::Block>& out) const override;
    void update(float dt, GameAPI&) override;
    void drawFloor(Renderer& r, Vec2 cam, float t) override;
    void drawOverlay(Renderer& r, Vec2 cam, float t) override;
    bool marked() const;
    void occupiedTiles(TileSet& out) const override {
        out.insert(key);
        out.insert(lock);
    }

private:
    void drawShape(Renderer& r, Vec2 c, float size, float rotation, Color col) const;
};

// --- 5. ContextTask ------------------------------------------------------------------
class ContextTask : public Puzzle {
public:
    ContextTask(Room& room, const PuzzleDef& def, const Elements& el, int index);
    Tile console, sw;
    std::vector<Tile> switchSpots;  // основной выключатель и запасные места
    std::optional<Tile> shadow, fan;
    TileSet dark, wind;
    bool effectOn = false, complete = false;
    float effect = 0.f, fanAngle = 0.f, windTimer = 0.f;

    Vec2 anchor() const override { return tileCenter(console); }
    bool checkSolved() const override { return complete; }
    bool matches(const Intent& in) const override;
    std::vector<int> objects() const override;
    bool buildPlan(Assistant& a, const Intent& intent, Script& out) override;
    std::vector<Interactable> interactables() override;
    std::wstring promptHint() const override { return def.ctx.hint; }
    void solidTiles(TileSet& out) const override;
    void blocks(std::vector<gfx::Block>& out) const override;
    void update(float dt, GameAPI& g) override;
    void drawFloor(Renderer& r, Vec2 cam, float t) override;
    void drawOverlay(Renderer& r, Vec2 cam, float t) override;
    bool randomize(Rng& rng, const TileSet& avoid) override;
    bool hazardActive() const { return effect < 0.35f; }
    void occupiedTiles(TileSet& out) const override;

private:
    void apply(const Intent& intent, int verdict, Assistant& a);
};

std::unique_ptr<Puzzle> createPuzzle(Room& room, const PuzzleDef& def, const Elements& el, int index);
