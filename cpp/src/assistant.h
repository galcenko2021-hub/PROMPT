// Помощники пяти уровней: следуют за Оператором, выполняют сценарии (идти, ждать,
// нажать, луч) и решают головоломки, если хватает уровня интеллекта.
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core.h"
#include "intent.h"
#include "render.h"
#include "settings.h"

class Assistant;
class Player;
class Puzzle;
class Room;

// --- шаги сценария ------------------------------------------------------------
struct Step {
    virtual ~Step() = default;
    virtual void start(Assistant&) {}
    virtual bool update(Assistant&, float) { return true; }  // true — шаг завершён
};
using Script = std::vector<std::unique_ptr<Step>>;

struct WalkStep : Step {
    std::vector<Vec2> points;
    float speed;
    bool routeCheck;  // проверять повреждённые сектора (маршрут Эхо)
    WalkStep(std::vector<Vec2> p, float s = cfg::ASSISTANT_TASK_SPEED, bool check = false)
        : points(std::move(p)), speed(s), routeCheck(check) {}
    bool update(Assistant& a, float dt) override;
};

struct GoToStep : Step {
    Tile tile;
    bool adjacent;
    float speed;
    std::unique_ptr<WalkStep> walk;
    GoToStep(Tile t, bool n = false, float s = cfg::ASSISTANT_TASK_SPEED) : tile(t), adjacent(n), speed(s) {}
    void start(Assistant& a) override;
    bool update(Assistant& a, float dt) override { return walk ? walk->update(a, dt) : true; }
};

struct WaitStep : Step {
    float left;
    explicit WaitStep(float s) : left(s) {}
    bool update(Assistant&, float dt) override { return (left -= dt) <= 0; }
};

struct CallStep : Step {
    std::function<void()> fn;
    explicit CallStep(std::function<void()> f) : fn(std::move(f)) {}
    void start(Assistant&) override { fn(); }
};

struct BeamStep : Step {
    Vec2 target;
    float left;
    BeamStep(Vec2 t, float s) : target(t), left(s) {}
    void start(Assistant& a) override;
    bool update(Assistant& a, float dt) override;
};

// --- Помощник -----------------------------------------------------------------
class Assistant {
public:
    using SayFn = std::function<void(Assistant&, const std::wstring&, Color)>;

    explicit Assistant(int level);

    int level;
    std::wstring name;
    Color color;
    Vec2 pos;
    Vec2 heading{1, 0};
    bool summoned = false;
    bool broken = false;
    bool staying = false;   // команда «стой»
    bool thinking = false;  // ждёт ответа нейросети
    Room* room = nullptr;
    std::optional<Vec2> beamTarget;
    SayFn onSay;

    Tile tile() const { return tileOf(pos); }
    bool busy() const { return current_ != nullptr || !script_.empty(); }
    bool canSolve(PuzzleKind kind) const { return level >= puzzleMinLevel(kind); }

    void say(const std::wstring& text, const std::wstring& mood = L"!", std::optional<Color> c = std::nullopt);
    // Пытается взяться за задачу. true — сценарий запущен.
    bool solve(Puzzle& puzzle, const Intent& intent);
    void run(Script steps, std::function<void()> onFail);
    void abort(const std::wstring& reason);
    void stop();
    void update(float dt, const Player& player);
    void teleportNear(Vec2 p);
    void draw(Renderer& r, Vec2 cam, float t) const;

private:
    void runScript(float dt);
    void follow(float dt, const Player& player);

    std::deque<std::unique_ptr<Step>> script_;
    std::unique_ptr<Step> current_;
    std::function<void()> onFail_;
    std::vector<Vec2> path_;
    float repath_ = 0.f;
    float farTimer_ = 0.f;
    std::wstring mood_;
    float moodTimer_ = 0.f;
    float anim_ = 0.f;
};

class AssistantRoster {
public:
    explicit AssistantRoster(Assistant::SayFn say);

    std::vector<int> available{1};
    int unlocked = 1;
    int selected = 1;
    bool summoned = false;

    Assistant& get(int level) { return *assistants_[static_cast<size_t>(level)]; }
    Assistant& current() { return get(selected); }
    Assistant* active() { return summoned ? &current() : nullptr; }

    void setupLevel(Room& room, const std::vector<int>& avail, int unlockedLevel);
    Assistant& summon(Vec2 where);
    std::pair<bool, std::wstring> select(int level);
    void setBroken(int level);  // 0 — починить всех
    void update(float dt, const Player& player);
    void draw(Renderer& r, Vec2 cam, float t) const;

private:
    std::vector<std::unique_ptr<Assistant>> assistants_;
    Room* room_ = nullptr;
};
