#include "assistant.h"

#include "audio.h"
#include "gfx.h"
#include "player.h"
#include "puzzle.h"
#include "room.h"

// --------------------------------------------------------------------------
// Шаги
// --------------------------------------------------------------------------
bool WalkStep::update(Assistant& a, float dt) {
    float remaining = speed * dt;
    while (remaining > 0 && !points.empty()) {
        const Vec2 target = points.front();
        const Vec2 delta = target - a.pos;
        const float dist = delta.length();
        if (dist <= remaining) {
            a.pos = target;
            remaining -= dist;
            points.erase(points.begin());
            if (routeCheck && a.room && a.room->mines.count(tileOf(target))) {
                a.room->revealMine(tileOf(target));
                a.abort(L"Повреждённый сектор! Маршрут был неверным.");
                return true;
            }
        } else {
            a.heading = delta / dist;
            a.pos += a.heading * remaining;
            remaining = 0;
        }
    }
    return points.empty();
}

void GoToStep::start(Assistant& a) {
    Room* room = a.room;
    if (!room) return;
    auto passable = [room](Tile t) { return room->assistantCanWalk(t); };
    TileSet goals;
    if (passable(tile)) goals.insert(tile);
    if (adjacent || goals.empty())
        for (const Tile& d : DIRS4)
            if (passable(tile + d)) goals.insert(tile + d);
    auto path = goals.empty() ? std::vector<Tile>{} : room->findPath(a.tile(), goals, passable);
    if (path.empty()) {
        a.abort(L"Не могу добраться туда — путь закрыт.");
        return;
    }
    std::vector<Vec2> pts;
    for (size_t i = 1; i < path.size(); ++i) pts.push_back(tileCenter(path[i]));
    if (pts.empty()) pts.push_back(tileCenter(path[0]));
    walk = std::make_unique<WalkStep>(std::move(pts), speed);
}

void BeamStep::start(Assistant& a) {
    a.beamTarget = target;
    audio::play("beam");
}

bool BeamStep::update(Assistant& a, float dt) {
    left -= dt;
    if (left <= 0) {
        a.beamTarget.reset();
        return true;
    }
    return false;
}

// --------------------------------------------------------------------------
// Помощник
// --------------------------------------------------------------------------
Assistant::Assistant(int lvl) : level(lvl), name(ASSISTANT_NAMES[lvl]), color(ASSISTANT_COLORS[lvl]) { anim_ = rng().uniform(0, 10); }

void Assistant::say(const std::wstring& text, const std::wstring& mood, std::optional<Color> c) {
    mood_ = mood;
    moodTimer_ = 2.2f;
    if (onSay) onSay(*this, text, c.value_or(color));
}

bool Assistant::solve(Puzzle& puzzle, const Intent& intent) {
    if (busy()) {
        say(L"Я ещё занят предыдущей задачей.", L"…");
        return false;
    }
    if (broken) {
        say(L"ERR#0xV1BE: веса повреждены Вайбкодером. Выбери другого Помощника!", L"×", cfg::DANGER);
        audio::play("glitch");
        return false;
    }
    if (!canSolve(puzzle.kind)) {
        const int need = puzzleMinLevel(puzzle.kind);
        say(std::wstring(ASSISTANT_REFUSALS[level]) + L" «" + puzzleTitle(puzzle.kind) + L"» — нужен уровень " + std::to_wstring(need) +
                L"+ (" + ASSISTANT_NAMES[need] + L").",
            L"?");
        audio::play("error");
        return false;
    }
    if (room) {
        const std::wstring veto = room->veto(*this, puzzle);
        if (!veto.empty()) {
            say(veto, L"×", cfg::DANGER);
            audio::play("error");
            return false;
        }
    }
    if (level == 5 && rng().chance(cfg::ORACLE_REFUSAL_CHANCE)) {
        say(L"Задача сформулирована плохо. Я отказываюсь — переформулируй.", L"?");
        audio::play("error");
        return false;
    }
    Script plan;
    if (!puzzle.buildPlan(*this, intent, plan)) {
        say(puzzle.lastError.empty() ? L"Не понимаю, что от меня нужно." : puzzle.lastError, L"?");
        audio::play("error");
        return false;
    }
    Puzzle* p = &puzzle;
    run(std::move(plan), [p] { p->cancel(); });
    say(L"Принято: " + intentPhrase(intent) + L".", L"!");
    audio::play("select");
    return true;
}

void Assistant::run(Script steps, std::function<void()> onFail) {
    script_.clear();
    for (auto& s : steps) script_.push_back(std::move(s));
    current_.reset();
    onFail_ = std::move(onFail);
    path_.clear();
    staying = false;
}

void Assistant::abort(const std::wstring& reason) {
    auto callback = std::move(onFail_);
    onFail_ = nullptr;
    script_.clear();
    current_.reset();
    beamTarget.reset();
    if (!reason.empty()) {
        say(reason, L"×", cfg::DANGER);
        audio::play("error");
    }
    if (callback) callback();
}

void Assistant::stop() {
    auto callback = std::move(onFail_);
    onFail_ = nullptr;
    script_.clear();
    current_.reset();
    beamTarget.reset();
    thinking = false;
    if (callback) callback();
}

void Assistant::update(float dt, const Player& player) {
    anim_ += dt;
    if (moodTimer_ > 0) moodTimer_ -= dt;
    if (!summoned || !room) return;
    if (busy())
        runScript(dt);
    else if (!staying)
        follow(dt, player);
}

void Assistant::runScript(float dt) {
    for (int i = 0; i < 12; ++i) {  // несколько мгновенных шагов за кадр
        if (!current_) {
            if (script_.empty()) {
                onFail_ = nullptr;
                return;
            }
            current_ = std::move(script_.front());
            script_.pop_front();
            Step* step = current_.get();
            step->start(*this);
            if (current_.get() != step) return;  // шаг прервал сценарий
        }
        Step* step = current_.get();
        const bool done = step->update(*this, dt);
        if (current_.get() != step) return;
        if (!done) return;
        current_.reset();
        dt = 0.f;
    }
}

void Assistant::follow(float dt, const Player& player) {
    const float distance = (player.pos - pos).length();
    if (distance > cfg::CATCHUP_DISTANCE) {
        farTimer_ += dt;
        if (farTimer_ > cfg::CATCHUP_TIME) {
            teleportNear(player.pos);
            return;
        }
    } else {
        farTimer_ = 0.f;
    }
    if (distance < cfg::FOLLOW_DISTANCE) {
        path_.clear();
        return;
    }
    repath_ -= dt;
    if (path_.empty() || repath_ <= 0) {
        repath_ = cfg::REPATH_INTERVAL;
        Room* r = room;
        auto path = r->findPathNearest(tile(), player.tile(), [r](Tile t) { return r->assistantCanWalk(t); });
        path_.clear();
        for (size_t i = 1; i < path.size(); ++i) path_.push_back(tileCenter(path[i]));
    }
    float remaining = cfg::ASSISTANT_SPEED * dt;
    while (remaining > 0 && !path_.empty()) {
        if ((player.pos - pos).length() < cfg::FOLLOW_DISTANCE) break;
        const Vec2 delta = path_.front() - pos;
        const float dist = delta.length();
        if (dist <= remaining) {
            pos = path_.front();
            remaining -= dist;
            path_.erase(path_.begin());
        } else {
            heading = delta / dist;
            pos += heading * remaining;
            remaining = 0;
        }
    }
}

void Assistant::teleportNear(Vec2 p) {
    if (!room) return;
    if (auto t = room->freeTileNear(tileOf(p))) {
        pos = tileCenter(*t);
        path_.clear();
        farTimer_ = 0.f;
        room->particles.burst(pos, color, 14, 120, 0.5f);
    }
}

void Assistant::draw(Renderer& r, Vec2 cam, float t) const {
    if (!summoned) return;
    const Vec2 center = pos - cam + Vec2{0, std::sin(anim_ * 3.2f) * 3};
    Color c = color;
    Vec2 jitter;
    if (broken) {
        c = static_cast<int>(t * 20) % 3 == 0 ? cfg::DANGER : color.scaled(0.45f);
        jitter = {rng().uniform(-3, 3), rng().uniform(-3, 3)};
    }
    if (beamTarget) {
        const Vec2 target = *beamTarget - cam;
        const float w = 3.f + 2.f * std::abs(std::sin(t * 20));
        r.line(center, target, c.scaled(0.5f), w + 4);
        r.line(center, target, c, w);
        r.glow(target, 30, c, 1.f);
    }
    r.glow(center, 34, c, 0.9f);
    if (level == 2) {
        const float ring = std::fmod(anim_ * 0.9f, 1.f);
        r.drawCircle(center, 12 + ring * 22, c.scaled(1 - ring), 1);
    }
    float rotation = level >= 4 ? anim_ * 1.5f : 0.f;
    if (level == 3) rotation = std::atan2(heading.y, heading.x);
    if (level == 5) rotation = anim_ * 0.8f;
    gfx::assistantIcon(r, level, center + jitter, 12, c, rotation);
    if (broken) {
        for (int i = 0; i < 2; ++i)
            r.fillRect(RectF::centered(center + Vec2{rng().uniform(-14, 14), rng().uniform(-14, 14)}, rng().uniform(10, 30), 3), cfg::DANGER);
        r.text(L"ERR", center + Vec2{0, -28}, 13, cfg::DANGER, Align::Center, true);
    }
    if (thinking) {
        for (int i = 0; i < 3; ++i) {
            const float k = 0.5f + 0.5f * std::sin(t * 8 - i * 0.9f);
            r.fillCircle(center + Vec2{-10.f + i * 10.f, -30}, 3 + k * 1.5f, cfg::THOUGHT.scaled(0.5f + k * 0.5f));
        }
    } else if (moodTimer_ > 0 && !mood_.empty()) {
        const Vec2 bubble = center + Vec2{16, -26};
        r.fillCircle(bubble, 10, Color::rgb(10, 10, 20));
        r.drawCircle(bubble, 10, c, 2);
        r.text(mood_, bubble, 14, c, Align::Center, true);
    }
    if (staying) r.text(L"ЖДУ", center + Vec2{0, 24}, 11, c.scaled(0.8f), Align::Center, true);
}

// --------------------------------------------------------------------------
// Набор Помощников
// --------------------------------------------------------------------------
AssistantRoster::AssistantRoster(Assistant::SayFn say) {
    assistants_.push_back(nullptr);
    for (int l = 1; l <= 5; ++l) {
        assistants_.push_back(std::make_unique<Assistant>(l));
        assistants_.back()->onSay = say;
    }
}

void AssistantRoster::setupLevel(Room& room, const std::vector<int>& avail, int unlockedLevel) {
    room_ = &room;
    available = avail;
    std::sort(available.begin(), available.end());
    unlocked = unlockedLevel;
    selected = available.front();
    summoned = false;
    for (int l = 1; l <= 5; ++l) {
        Assistant& a = get(l);
        a.stop();
        a.room = &room;
        a.summoned = false;
        a.broken = false;
        a.staying = false;
        a.beamTarget.reset();
    }
}

Assistant& AssistantRoster::summon(Vec2 where) {
    Assistant& a = current();
    a.summoned = true;
    summoned = true;
    a.staying = false;
    a.teleportNear(where);
    if (room_) room_->particles.burst(a.pos, a.color, 28, 200, 0.8f);
    audio::play("summon");
    return a;
}

std::pair<bool, std::wstring> AssistantRoster::select(int level) {
    if (level > unlocked) return {false, L"Помощник уровня " + std::to_wstring(level) + L" ещё не разблокирован."};
    if (std::find(available.begin(), available.end(), level) == available.end())
        return {false, std::wstring(ASSISTANT_NAMES[level]) + L" недоступен на этом уровне."};
    if (level == selected) return {false, L""};
    Assistant& old = current();
    if (summoned && (old.busy() || old.thinking)) return {false, old.name + L" занят — дождись окончания."};
    selected = level;
    Assistant& now = current();
    if (summoned) {
        old.summoned = false;
        now.summoned = true;
        now.pos = old.pos;
        now.staying = false;
        if (room_) room_->particles.burst(now.pos, now.color, 22, 170, 0.6f);
        audio::play("summon");
    } else {
        audio::play("select");
    }
    return {true, L"Активный Помощник: " + now.name + L" (интеллект " + std::to_wstring(now.level) + L"/5)."};
}

void AssistantRoster::setBroken(int level) {
    for (int l = 1; l <= 5; ++l) get(l).broken = l == level;
}

void AssistantRoster::update(float dt, const Player& player) {
    for (int l = 1; l <= 5; ++l) get(l).update(dt, player);
}

void AssistantRoster::draw(Renderer& r, Vec2 cam, float t) const {
    for (int l = 1; l <= 5; ++l) assistants_[static_cast<size_t>(l)]->draw(r, cam, t);
}
