#include "boss.h"

#include "assistant.h"
#include "audio.h"
#include "brain_client.h"
#include "game_api.h"
#include "gfx.h"
#include "puzzle.h"
#include "room.h"

namespace {

const wchar_t* const CODE_FRAGMENTS[] = {L"vibe()", L"// TODO", L"rm -rf /", L"while(true)", L"copy+paste",
                                         L"200 OK?", L"it works!", L"import *", L"LGTM", L"git push -f"};
const wchar_t* const TAUNTS[] = {
    L"Зачем понимать код, если можно его нагенерировать?",
    L"Тесты? Я просто чувствую, что всё работает.",
    L"Ещё один промпт — и всё точно заработает!",
    L"Мой код компилируется с третьего раза. Это успех.",
    L"Документация — для слабых.",
};
const wchar_t* const PHASE_LINES[4] = {
    L"",
    L"Сначала поиграем с управлением. Где тут W, а где S?",
    L"А теперь — вверх ногами! Мир — это просто массив, его можно развернуть.",
    L"Твои Помощники — мои Помощники. Я подменю веса их нейросетей.",
};
const wchar_t* const KEY_NAMES[4] = {L"W", L"A", L"S", L"D"};

}  // namespace

// --------------------------------------------------------------------------
// Баг
// --------------------------------------------------------------------------
bool GlitchBug::blocked(const Room& room, Vec2 p) const {
    const Tile t = tileOf(p);
    return room.blocksPlayer(t) || room.field.count(t);
}

bool GlitchBug::update(float dt, const Room& room, Vec2 target) {
    seed_ += dt;
    const Vec2 desired = (target - pos).normalized();
    const Vec2 wobble{std::cos(seed_ * 3.1f) * 0.6f, std::sin(seed_ * 2.3f) * 0.6f};
    velocity_ = velocity_.lerp((desired + wobble) * cfg::BUG_SPEED, std::min(1.f, dt * 3));
    const float radius = 11.f;
    const Vec2 sx{velocity_.x * dt, 0}, sy{0, velocity_.y * dt};
    if (!blocked(room, pos + sx + Vec2{velocity_.x >= 0 ? radius : -radius, 0})) pos += sx;
    if (!blocked(room, pos + sy + Vec2{0, velocity_.y >= 0 ? radius : -radius})) pos += sy;
    return pos.distance(target) < radius + 12;
}

void GlitchBug::draw(Renderer& r, Vec2 cam) const {
    const Vec2 c = pos - cam;
    r.glow(c, 28, cfg::DANGER, 0.8f);
    for (int i = 0; i < 3; ++i) {
        const float s = rng().uniform(6, 14);
        const RectF rect = RectF::centered(c + Vec2{rng().uniform(-5, 5), rng().uniform(-5, 5)}, s, s);
        if (i == 0)
            r.fillRect(rect, cfg::DANGER);
        else
            r.drawRect(rect, Color::rgb(255, 160, 200), 1);
    }
}

// --------------------------------------------------------------------------
// Вайбкодер
// --------------------------------------------------------------------------
Vibecoder::Vibecoder(Vec2 p, std::vector<std::string> phases, Room& room) : pos(p), hp(cfg::BOSS_MAX_HP), phases_(std::move(phases)), room_(room) {
    activatePhase();
}

int Vibecoder::phase() const { return std::min(cfg::BOSS_MAX_HP, cfg::BOSS_MAX_HP - hp + 1); }

void Vibecoder::activatePhase() {
    for (auto& p : room_.puzzles) {
        auto it = std::find(phases_.begin(), phases_.end(), p->def.id);
        if (it != phases_.end() && !p->solved) p->active = (it - phases_.begin()) == phase() - 1;
    }
}

void Vibecoder::say(const std::wstring& text) const {
    if (room_.notify) room_.notify(text, L"ВАЙБКОДЕР", cfg::BOSS);
}

void Vibecoder::spawnBugs(GameAPI& g) {
    bugs_.clear();
    std::vector<Tile> cand;
    for (int y = 0; y < room_.height; ++y)
        for (int x = 0; x < room_.width; ++x) {
            const Tile t{x, y};
            if (room_.blocksPlayer(t) || room_.field.count(t)) continue;
            if (tileCenter(t).distance(g.player().pos) > TILE * 7 && tileCenter(t).distance(pos) > TILE * 3) cand.push_back(t);
        }
    rng().shuffle(cand);
    const int counts[4] = {0, 2, 1, 3};
    for (int i = 0; i < counts[phase()] && i < static_cast<int>(cand.size()); ++i) bugs_.emplace_back(tileCenter(cand[static_cast<size_t>(i)]));
}

std::wstring Vibecoder::statusText() const {
    if (defeated) return L"Вайбкодер переобучен…";
    if (swapActive_) {
        std::wstring s = L"Управление сломано (" + formatFloat(swapTimer_) + L" с): ";
        for (int i = 0; i < 4; ++i) s += std::wstring(KEY_NAMES[i]) + L"→" + KEY_NAMES[swap_[static_cast<size_t>(i)]] + (i < 3 ? L", " : L"");
        return s;
    }
    if (flip_.first || flip_.second) return L"Мир перевёрнут (" + formatFloat(flipTimer_) + L" с)";
    if (corrupted) return L"Веса испорчены: " + std::wstring(ASSISTANT_NAMES[corrupted]) + L" (смена через " + formatFloat(corruptTimer_) + L" с)";
    if (copied) return L"Скопирован: " + std::wstring(ASSISTANT_NAMES[copied]) + L" — нужен другой Помощник";
    return L"";
}

std::wstring Vibecoder::veto(const Assistant& a, const Puzzle& p) const {
    if (std::find(phases_.begin(), phases_.end(), p.def.id) != phases_.end() && a.level == copied)
        return L"Вайбкодер уже скопировал мои приёмы — со мной он не сработает. Позови другого Помощника!";
    return L"";
}

void Vibecoder::releaseCorruption(GameAPI& g) {
    if (corrupted) g.brain().heal(corrupted);
    corrupted = 0;
    g.roster().setBroken(0);
}

void Vibecoder::update(float dt, GameAPI& g) {
    time_ += dt;
    if (!started_) {
        started_ = true;
        spawnBugs(g);
        say(PHASE_LINES[1]);
    }
    if (defeated) {
        defeatTimer_ += dt;
        if (rng().chance(0.3f)) room_.particles.burst(pos, Color::hsv(rng().uniform()), 8, 260, 0.9f);
        if (defeatTimer_ > 3.4f && !finished_) {
            finished_ = true;
            g.onVictory();
        }
        return;
    }
    if (hurt_ > 0) hurt_ -= dt;
    const int ph = phase();
    if (ph == 1) {
        if (swapActive_) {
            swapTimer_ -= dt;
            if (swapTimer_ <= 0) {
                swapActive_ = false;
                swapCooldown_ = cfg::CONTROL_SWAP_INTERVAL;
                g.notify(L"Управление восстановлено.");
            }
        } else if ((swapCooldown_ -= dt) <= 0) {
            std::vector<int> perm{0, 1, 2, 3};
            do rng().shuffle(perm);
            while (perm[0] == 0 || perm[1] == 1 || perm[2] == 2 || perm[3] == 3);
            for (int i = 0; i < 4; ++i) swap_[static_cast<size_t>(i)] = perm[static_cast<size_t>(i)];
            swapActive_ = true;
            swapTimer_ = cfg::CONTROL_SWAP_DURATION;
            audio::play("glitch");
            room_.shake = std::max(room_.shake, 0.3f);
            say(L"WASD — это просто буквы. Перемешаю-ка их на 5 секунд!");
        }
    } else if (ph == 2) {
        if (flip_.first || flip_.second) {
            if ((flipTimer_ -= dt) <= 0) {
                flip_ = {false, false};
                flipCooldown_ = cfg::FLIP_OFF_DURATION;
            }
        } else if ((flipCooldown_ -= dt) <= 0) {
            const std::pair<bool, bool> modes[3] = {{false, true}, {true, false}, {true, true}};
            flip_ = modes[rng().range(0, 2)];
            flipTimer_ = cfg::FLIP_ON_DURATION;
            audio::play("glitch");
            room_.shake = std::max(room_.shake, 0.35f);
            say(L"Переворачиваю экран! transform.flip(world)!");
        }
    } else if ((corruptTimer_ -= dt) <= 0) {
        corruptTimer_ = cfg::CORRUPTION_INTERVAL;
        std::vector<int> options;
        for (int l : g.roster().available)
            if (l != corrupted) options.push_back(l);
        if (!options.empty()) {
            if (corrupted) g.brain().heal(corrupted);
            corrupted = rng().pick(options);
            g.roster().setBroken(corrupted);
            g.brain().corrupt(corrupted, 1.3f);
            audio::play("glitch");
            say(L"Подменяю веса нейросети «" + std::wstring(ASSISTANT_NAMES[corrupted]) + L"». Удачи с ним!");
        }
    }
    for (auto& bug : bugs_)
        if (bug.update(dt, room_, g.player().pos) && g.player().invulnerable <= 0) {
            g.playerCaught(L"Баг Вайбкодера задел тебя! Откат к точке входа.");
            break;
        }
    if ((tauntTimer_ -= dt) <= 0) {
        tauntTimer_ = rng().uniform(13, 18);
        say(TAUNTS[rng().range(0, 4)]);
    }
}

void Vibecoder::onPuzzleSolved(Puzzle& p, GameAPI& g) {
    if (defeated || std::find(phases_.begin(), phases_.end(), p.def.id) == phases_.end()) return;
    --hp;
    copied = p.solverLevel;
    hurt_ = 1.4f;
    swapActive_ = false;
    flip_ = {false, false};
    releaseCorruption(g);
    room_.shake = 0.8f;
    audio::play("hit");
    room_.particles.burst(pos, cfg::BOSS, 60, 380, 1.2f);
    if (hp <= 0) {
        defeated = true;
        bugs_.clear();
        say(L"Я… понимаю… что пишу?.. Это… лучше, чем вайб…");
        return;
    }
    say(std::wstring(L"Ай! Минус треть HP. ") + PHASE_LINES[phase()]);
    if (copied)
        g.notify(L"Вайбкодер скопировал приёмы «" + std::wstring(ASSISTANT_NAMES[copied]) + L"» — в следующей фазе нужен другой Помощник.",
                 L"СИСТЕМА", cfg::WARNING);
    activatePhase();
    spawnBugs(g);
    tauntTimer_ = 14.f;
}

void Vibecoder::drawGround(Renderer& r, Vec2 cam, float t) const {
    const Vec2 c = pos - cam;
    if (!defeated)
        for (int i = 0; i < 3; ++i)
            r.drawCircle(c, 90 + i * 22 + 6 * std::sin(t * 2 + i), Color::hsv(t * 0.1f + i * 0.12f, 0.8f, 0.35f), 1);
    for (const auto& b : bugs_) b.draw(r, cam);
}

void Vibecoder::draw(Renderer& r, Vec2 cam, float t) const {
    const float scale = defeated ? std::max(0.f, 1.f - defeatTimer_ / 3.2f) : 1.f;
    if (scale <= 0) return;
    const Vec2 jitter = hurt_ > 0 ? Vec2{rng().uniform(-4, 4), rng().uniform(-4, 4)} : Vec2{};
    const Vec2 c = pos - cam + jitter;
    const float hue = std::fmod(t * 0.15f, 1.f);
    Color col = Color::hsv(hue, 0.75f, 1.f);
    if (hurt_ > 0 && static_cast<int>(t * 20) % 2 == 0) col = Color::rgb(255, 255, 255);
    const float radius = cfg::BOSS_RADIUS * (1 + 0.08f * std::sin(t * 3)) * scale;
    const int sides = 3 + static_cast<int>(t * 0.8f) % 6;
    std::vector<Vec2> pts;
    for (int i = 0; i < sides; ++i) {
        const float a = t * 0.7f + i * TAU / sides;
        pts.push_back(c + Vec2::polar(a, radius * (1 + 0.16f * std::sin(t * 5 + i * 2.1f) + rng().uniform(-0.05f, 0.05f))));
    }
    r.glow(c, radius * 2.4f, col, 1.1f);
    gfx::neonPolygon(r, col, pts, 3, 0.2f);
    r.drawPolygon(gfx::regularPolygon(c, radius * 0.55f, sides + 2, -t * 1.3f), Color::hsv(hue + 0.5f, 0.8f, 1.f), 2);
    r.fillCircle(c, std::max(3.f, radius * 0.16f), Color::rgb(255, 255, 255));
    if (!defeated) {
        constexpr int n = static_cast<int>(sizeof(CODE_FRAGMENTS) / sizeof(CODE_FRAGMENTS[0]));
        for (int i = 0; i < n; ++i) {
            const float a = t * 0.5f + i * TAU / n;
            const Vec2 p = c + Vec2::polar(a, radius + 52 + 8 * std::sin(t * 2 + i));
            r.text(CODE_FRAGMENTS[i], p, 13, Color::hsv(hue + i * 0.07f, 0.6f, 0.95f), Align::Center);
        }
    }
}
