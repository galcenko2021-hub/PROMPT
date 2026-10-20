// Оператор: движение WASD, коллизии со стенами, отрисовка.
// Оператор — проекция: проходит сквозь ящики, но упирается в стены и двери.
#pragma once

#include <array>
#include <deque>

#include "core.h"
#include "input.h"
#include "render.h"

class Room;

// Подмена клавиш боссом: scramble[i] — какое направление даёт клавиша i (0=W, 1=A, 2=S, 3=D).
using Scramble = std::array<int, 4>;

class Player {
public:
    Vec2 pos;
    Vec2 facing{0, 1};
    Vec2 externalPush;
    float invulnerable = 0.f;

    Tile tile() const { return tileOf(pos); }
    void respawn(Vec2 p);
    static Vec2 readDirection(const Input& input, const Scramble* scramble);
    void update(float dt, const Input& input, const Room& room, const Scramble* scramble);
    void draw(Renderer& r, Vec2 cam, float t) const;

private:
    void resolve(const Room& room, bool horizontal, bool positive);

    std::deque<Vec2> trail_;
    float trailTimer_ = 0.f;
};
