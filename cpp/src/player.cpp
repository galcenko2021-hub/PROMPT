#include "player.h"

#include "gfx.h"
#include "room.h"
#include "settings.h"

namespace {
const Vec2 DIRECTIONS[4] = {{0, -1}, {-1, 0}, {0, 1}, {1, 0}};  // W, A, S, D
}

void Player::respawn(Vec2 p) {
    pos = p;
    externalPush = {};
    trail_.clear();
    invulnerable = cfg::RESPAWN_INVULNERABILITY;
}

Vec2 Player::readDirection(const Input& input, const Scramble* scramble) {
    const bool keys[4] = {input.held(K_W) || input.held(K_UP), input.held(K_A) || input.held(K_LEFT),
                          input.held(K_S) || input.held(K_DOWN), input.held(K_D) || input.held(K_RIGHT)};
    Vec2 dir;
    for (int i = 0; i < 4; ++i)
        if (keys[i]) dir += DIRECTIONS[scramble ? (*scramble)[static_cast<size_t>(i)] : i];
    return dir.normalized();
}

void Player::update(float dt, const Input& input, const Room& room, const Scramble* scramble) {
    const Vec2 dir = readDirection(input, scramble);
    if (dir.lengthSq() > 0) facing = dir;
    const Vec2 delta = (dir * cfg::PLAYER_SPEED + externalPush) * dt;
    externalPush = {};
    if (delta.x != 0) {
        pos.x += delta.x;
        resolve(room, true, delta.x > 0);
    }
    if (delta.y != 0) {
        pos.y += delta.y;
        resolve(room, false, delta.y > 0);
    }
    if (invulnerable > 0) invulnerable -= dt;
    trailTimer_ -= dt;
    if (trailTimer_ <= 0) {
        trailTimer_ = 0.05f;
        if (dir.lengthSq() > 0) {
            trail_.push_back(pos);
            if (trail_.size() > 8) trail_.pop_front();
        } else if (!trail_.empty()) {
            trail_.pop_front();
        }
    }
}

void Player::resolve(const Room& room, bool horizontal, bool positive) {
    const float half = cfg::PLAYER_SIZE / 2;
    const float left = pos.x - half, right = pos.x + half, top = pos.y - half, bottom = pos.y + half;
    for (int ty = static_cast<int>(std::floor(top / TILE)); ty <= static_cast<int>(std::floor((bottom - 0.001f) / TILE)); ++ty)
        for (int tx = static_cast<int>(std::floor(left / TILE)); tx <= static_cast<int>(std::floor((right - 0.001f) / TILE)); ++tx) {
            if (!room.blocksPlayer({tx, ty})) continue;
            if (horizontal)
                pos.x = positive ? tx * TILE - half - 0.01f : (tx + 1) * TILE + half + 0.01f;
            else
                pos.y = positive ? ty * TILE - half - 0.01f : (ty + 1) * TILE + half + 0.01f;
            return;
        }
}

void Player::draw(Renderer& r, Vec2 cam, float t) const {
    if (invulnerable > 0 && static_cast<int>(t * 16) % 2 == 0) return;
    for (size_t i = 0; i < trail_.size(); ++i) {
        const float k = static_cast<float>(i + 1) / (trail_.size() + 1);
        r.fillCircle(trail_[i] - cam, 3 + 4 * k, cfg::PLAYER.scaled(0.35f * k));
    }
    const Vec2 c = pos - cam;
    const float pulse = 1.f + 0.08f * std::sin(t * 5);
    r.glow(c, 40 * pulse, cfg::PLAYER, 0.9f);
    gfx::neonPolygon(r, cfg::PLAYER, gfx::regularPolygon(c, 15 * pulse, 4, PI / 4 + t * 0.6f), 2, 0.3f);
    r.fillCircle(c, 5, cfg::PLAYER.scaled(1.6f));
    r.line(c + facing * 11, c + facing * 21, cfg::PLAYER.scaled(1.5f), 3);
}
