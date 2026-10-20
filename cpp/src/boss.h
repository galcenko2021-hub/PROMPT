// Босс «Вайбкодер»: три фазы ломают правила игры.
//   Фаза 1 — путает клавиши WASD на 5 секунд.
//   Фаза 2 — переворачивает мир (как pygame.transform.flip).
//   Фаза 3 — подменяет Помощников: портит веса нейросети случайного Помощника.
// Каждая решённая в бою головоломка снимает 1/3 HP. Помощника, нанёсшего урон,
// босс «копирует» — в следующей фазе нужен другой.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core.h"
#include "player.h"
#include "render.h"

class Assistant;
class GameAPI;
class Puzzle;
class Room;

class GlitchBug {
public:
    explicit GlitchBug(Vec2 p) : pos(p), seed_(rng().uniform(0, 100)) {}
    Vec2 pos;
    bool update(float dt, const Room& room, Vec2 target);
    void draw(Renderer& r, Vec2 cam) const;

private:
    bool blocked(const Room& room, Vec2 p) const;
    Vec2 velocity_;
    float seed_;
};

class Vibecoder {
public:
    Vibecoder(Vec2 pos, std::vector<std::string> phases, Room& room);

    Vec2 pos;
    int hp;
    int copied = 0;     // уровень «скопированного» Помощника
    int corrupted = 0;  // уровень Помощника с испорченными весами
    bool defeated = false;

    int phase() const;
    std::pair<bool, bool> flipFlags() const { return defeated ? std::pair{false, false} : flip_; }
    const Scramble* controlScramble() const { return defeated || !swapActive_ ? nullptr : &swap_; }
    std::wstring statusText() const;
    std::wstring veto(const Assistant& a, const Puzzle& p) const;

    void update(float dt, GameAPI& g);
    void onPuzzleSolved(Puzzle& p, GameAPI& g);
    void releaseCorruption(GameAPI& g);
    void drawGround(Renderer& r, Vec2 cam, float t) const;
    void draw(Renderer& r, Vec2 cam, float t) const;

private:
    void activatePhase();
    void spawnBugs(GameAPI& g);
    void say(const std::wstring& text) const;

    std::vector<std::string> phases_;
    Room& room_;
    float time_ = 0.f, hurt_ = 0.f, defeatTimer_ = 0.f, tauntTimer_ = 12.f;
    bool started_ = false, finished_ = false;
    bool swapActive_ = false;
    Scramble swap_{0, 1, 2, 3};
    float swapTimer_ = 0.f, swapCooldown_ = 3.f;
    std::pair<bool, bool> flip_{false, false};
    float flipTimer_ = 0.f, flipCooldown_ = 2.5f;
    float corruptTimer_ = 1.5f;
    std::vector<GlitchBug> bugs_;
};
