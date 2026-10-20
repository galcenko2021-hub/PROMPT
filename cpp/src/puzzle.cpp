#include "puzzle.h"

#include <deque>
#include <unordered_map>

#include "audio.h"
#include "boss.h"
#include "game_api.h"
#include "player.h"
#include "room.h"

namespace {

const Color BUTTON_COLORS[4] = {Color::rgb(255, 80, 90), Color::rgb(70, 255, 130), Color::rgb(80, 160, 255), Color::rgb(255, 220, 60)};

int manhattan(Tile a, Tile b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y); }

}  // namespace

// ==========================================================================
// Базовый класс
// ==========================================================================
Puzzle::Puzzle(PuzzleKind k, Room& r, const PuzzleDef& d, const Elements& el, int idx)
    : kind(k), room(r), def(d), index(idx), elements_(el) {}

Tile Puzzle::element(const std::string& name) const {
    auto it = elements_.find(name);
    if (it == elements_.end() || it->second.empty()) throw std::runtime_error("puzzle " + def.id + ": no element " + name);
    return it->second.front().first;
}

std::vector<Tile> Puzzle::elements(const std::string& name) const {
    std::vector<Tile> out;
    auto it = elements_.find(name);
    if (it == elements_.end()) return out;
    auto items = it->second;
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    for (const auto& [t, i] : items) out.push_back(t);
    return out;
}

void Puzzle::accept(Assistant& a) {
    inProgress = true;
    solverLevel = a.level;
}

void Puzzle::notify(const std::wstring& text, const std::wstring& speaker, Color c) const {
    if (room.notify) room.notify(text, speaker, c);
}

gfx::Block Puzzle::deviceBlock(Tile t, Color c, float height, float shrink) {
    gfx::Block b;
    b.rect = tileRect(t).inflate(-shrink, -shrink);
    b.height = height;
    b.top = c.scaled(0.18f);
    b.side = c.scaled(0.12f);
    b.edge = c;
    return b;
}

std::function<void(GameAPI&)> Puzzle::promptAction() const {
    const int idx = index;
    const std::wstring hint = promptHint();
    return [idx, hint](GameAPI& g) { g.openPrompt(idx, hint); };
}

// ==========================================================================
// 1. PushBox
// ==========================================================================
PushBox::PushBox(Room& r, const PuzzleDef& d, const Elements& el, int idx) : Puzzle(PuzzleKind::PushBox, r, d, el, idx) {
    box = element("box");
    target = element("plate");
    boxPos = tileCenter(box);
}

bool PushBox::checkSolved() const { return box == target && boxPos.distance(tileCenter(target)) < 1.f; }

std::vector<Interactable> PushBox::interactables() {
    if (solved) return {};
    return {{boxPos, inProgress ? L"Помощник толкает ящик…" : L"Дать задачу Помощнику: ящик", promptAction(), cfg::INTERACT_RANGE * 1.15f,
             cfg::BOX}};
}

bool PushBox::planMoves(Tile start, Tile box0, Tile goal, std::vector<std::pair<Tile, bool>>& moves) const {
    TileSet others = room.boxTiles();
    others.erase(box);
    auto free = [&](Tile t) { return !others.count(t) && room.assistantCanWalk(t, false, true); };
    moves.clear();
    if (box0 == goal) return true;

    struct Parent {
        int prev;
        Tile boxBefore, stand;
        int dir;
    };
    auto keyOf = [](Tile b, int dir) { return ((b.y * 512) + b.x) * 5 + dir; };
    std::unordered_map<int, Parent> parents;
    struct Item {
        Tile box, pusher;
        int key;
    };
    std::deque<Item> queue;
    const int startKey = keyOf(box0, 4);
    parents[startKey] = {-1, box0, start, -1};
    queue.push_back({box0, start, startKey});
    auto flood = [&](Tile from, Tile b) {
        TileSet seen{from};
        std::deque<Tile> q{from};
        while (!q.empty()) {
            const Tile c = q.front();
            q.pop_front();
            for (const Tile& d : DIRS4) {
                const Tile n = c + d;
                if (!seen.count(n) && n != b && free(n)) {
                    seen.insert(n);
                    q.push_back(n);
                }
            }
        }
        return seen;
    };
    while (!queue.empty()) {
        const Item it = queue.front();
        queue.pop_front();
        const TileSet area = flood(it.pusher, it.box);
        for (int d = 0; d < 4; ++d) {
            const Tile stand = it.box - DIRS4[d];
            const Tile dest = it.box + DIRS4[d];
            if (!area.count(stand) || !free(dest)) continue;
            const int k = keyOf(dest, d);
            if (parents.count(k)) continue;
            parents[k] = {it.key, it.box, stand, d};
            if (dest == goal) {
                std::vector<Parent> pushes;
                int cur = k;
                while (parents[cur].prev != -1) {
                    pushes.push_back(parents[cur]);
                    cur = parents[cur].prev;
                }
                std::reverse(pushes.begin(), pushes.end());
                Tile pos = start;
                for (const Parent& p : pushes) {
                    const Tile before = p.boxBefore;
                    auto path = room.findPath(pos, TileSet{p.stand}, [&](Tile t) { return free(t) && t != before; });
                    if (path.empty()) return false;
                    for (size_t i = 1; i < path.size(); ++i) moves.push_back({path[i] - path[i - 1], false});
                    moves.push_back({DIRS4[p.dir], true});
                    pos = before;
                }
                return true;
            }
            queue.push_back({dest, it.box, k});
        }
    }
    return false;
}

bool PushBox::buildPlan(Assistant& a, const Intent&, Script& out) {
    std::vector<std::pair<Tile, bool>> moves;
    if (!planMoves(a.tile(), box, target, moves)) {
        lastError = L"Не вижу решения: до ящика не добраться или его некуда толкать.";
        return false;
    }
    out.push_back(std::make_unique<WalkStep>(std::vector<Vec2>{tileCenter(a.tile())}));
    Tile cur = a.tile();
    for (const auto& [dir, push] : moves) {
        const Tile next = cur + dir;
        if (push) {
            const Tile d = dir;
            out.push_back(std::make_unique<CallStep>([this, d] {
                box = box + d;
                audio::play("push");
            }));
        }
        out.push_back(std::make_unique<WalkStep>(std::vector<Vec2>{tileCenter(next)}, push ? cfg::PUSH_SPEED : cfg::ASSISTANT_TASK_SPEED));
        cur = next;
    }
    out.push_back(std::make_unique<CallStep>([this] { inProgress = false; }));
    accept(a);
    return true;
}

void PushBox::update(float dt, GameAPI&) {
    const Vec2 goal = tileCenter(box);
    const Vec2 delta = goal - boxPos;
    const float step = cfg::PUSH_SPEED * dt;
    boxPos = delta.length() <= step ? goal : boxPos + delta.normalized() * step;
}

void PushBox::drawFloor(Renderer& r, Vec2 cam, float t) {
    const RectF rect = tileRect(target).inflate(-8, -8).moved(-cam);
    const Color c = box == target ? cfg::SUCCESS : cfg::WARNING;
    r.glow(rect.center(), 30, c, 0.35f + 0.3f * (0.5f + 0.5f * std::sin(t * 3)));
    r.fillRect(rect, c.scaled(0.25f));
    r.drawRect(rect, c, 2);
    r.drawRect(rect.inflate(-18, -18), c, 1);
}

void PushBox::blocks(std::vector<gfx::Block>& out) const {
    const Color c = solved ? cfg::SUCCESS : cfg::BOX;
    gfx::Block b;
    b.rect = RectF::centered(boxPos, TILE - 10, TILE - 10);
    b.height = cfg::BOX_HEIGHT;
    b.top = c.scaled(0.22f);
    b.side = c.scaled(0.14f);
    b.edge = c;
    out.push_back(b);
}

bool PushBox::randomize(Rng& g, const TileSet& avoid) {
    if (!def.zone.valid()) return false;
    std::vector<Tile> cand;
    for (int y = def.zone.y0; y <= def.zone.y1; ++y)
        for (int x = def.zone.x0; x <= def.zone.x1; ++x) {
            const Tile t{x, y};
            if (room.isFloorFree(t) && !avoid.count(t)) cand.push_back(t);
        }
    if (cand.size() < 6) return false;
    for (int attempt = 0; attempt < 60; ++attempt) {
        const Tile b = g.pick(cand), p = g.pick(cand);
        if (b == p || manhattan(b, p) < 2) continue;
        int open = 0;
        for (const Tile& d : DIRS4) open += room.isFloorFree(b + d) ? 1 : 0;
        if (open < 3) continue;  // не прижимаем ящик к стене — меньше тупиков
        bool ok = true;
        int tested = 0;
        for (int s = 0; s < 10 && tested < 3; ++s) {
            const Tile start = g.pick(cand);
            if (start == b) continue;
            ++tested;
            std::vector<std::pair<Tile, bool>> moves;
            if (!planMoves(start, b, p, moves)) {
                ok = false;
                break;
            }
        }
        if (!ok || tested == 0) continue;
        box = b;
        target = p;
        boxPos = tileCenter(box);
        return true;
    }
    return false;
}

// ==========================================================================
// 2. ButtonSequence
// ==========================================================================
ButtonSequence::ButtonSequence(Room& r, const PuzzleDef& d, const Elements& el, int idx)
    : Puzzle(PuzzleKind::ButtonSequence, r, d, el, idx) {
    buttons = elements("button");
    panel = element("panel");
    order = d.order;
    if (order.empty())
        for (int i = 0; i < static_cast<int>(buttons.size()); ++i) order.push_back(i);
}

std::vector<Interactable> ButtonSequence::interactables() {
    if (solved) return {};
    std::vector<Interactable> items;
    const int n = static_cast<int>(buttons.size());
    if (static_cast<int>(marks.size()) == n) {
        items.push_back({tileCenter(panel), L"Дать задачу Помощнику: нажать кнопки", promptAction(), cfg::INTERACT_RANGE, cfg::SYSTEM});
    } else {
        items.push_back({tileCenter(panel), L"Показать порядок кнопок",
                         [this](GameAPI& g) {
                             if (inProgress) return;
                             demo = order;
                             demoTimer = 0.5f;
                             g.notify(L"Запомни порядок мигания и отметь кнопки по очереди (E у каждой).");
                         },
                         cfg::INTERACT_RANGE, cfg::SYSTEM});
    }
    for (int i = 0; i < n; ++i) {
        const bool markedHere = std::find(marks.begin(), marks.end(), i) != marks.end();
        std::wstring label = markedHere ? L"Сбросить метки"
                                        : L"Отметить кнопку · шаг " + std::to_wstring(marks.size() + 1) + L"/" + std::to_wstring(n);
        items.push_back({tileCenter(buttons[static_cast<size_t>(i)]), label, [this, i](GameAPI& g) { onButton(g, i); }, cfg::INTERACT_RANGE,
                         BUTTON_COLORS[i % 4]});
    }
    return items;
}

void ButtonSequence::onButton(GameAPI& g, int i) {
    if (inProgress) {
        g.notify(L"Помощник уже нажимает кнопки.");
        return;
    }
    if (std::find(marks.begin(), marks.end(), i) != marks.end()) {
        marks.clear();
        audio::play("mark");
        g.notify(L"Метки сброшены.");
        return;
    }
    marks.push_back(i);
    flash[i] = 0.25f;
    audio::play("tone" + std::to_string(i % 4));
    if (marks.size() == buttons.size()) g.openPrompt(index, L"Последовательность готова. " + promptHint());
}

bool ButtonSequence::buildPlan(Assistant& a, const Intent&, Script& out) {
    if (marks.size() < buttons.size()) {
        lastError = L"Сначала отметь все кнопки в правильном порядке (E у каждой кнопки).";
        return false;
    }
    for (int i : marks) {
        out.push_back(std::make_unique<GoToStep>(buttons[static_cast<size_t>(i)]));
        out.push_back(std::make_unique<CallStep>([this, i] {
            pressed.push_back(i);
            flash[i] = 0.6f;
            audio::play("tone" + std::to_string(i % 4));
        }));
        out.push_back(std::make_unique<WaitStep>(0.25f));
    }
    out.push_back(std::make_unique<CallStep>([this] { evaluate(); }));
    pressed.clear();
    accept(a);
    return true;
}

void ButtonSequence::evaluate() {
    inProgress = false;
    if (pressed == order) {
        complete = true;
        return;
    }
    failTimer = 1.2f;
    marks.clear();
    pressed.clear();
    audio::play("error");
    notify(L"Неверный порядок! Посмотри мигание ещё раз (E у панели).", L"СИСТЕМА", cfg::DANGER);
}

void ButtonSequence::cancel() {
    inProgress = false;
    pressed.clear();
}

void ButtonSequence::update(float dt, GameAPI&) {
    for (auto it = flash.begin(); it != flash.end();) {
        it->second -= dt;
        it = it->second <= 0 ? flash.erase(it) : std::next(it);
    }
    if (failTimer > 0) failTimer -= dt;
    if (!demo.empty()) {
        demoTimer -= dt;
        if (demoTimer <= 0) {
            const int i = demo.front();
            demo.erase(demo.begin());
            flash[i] = 0.45f;
            audio::play("tone" + std::to_string(i % 4));
            demoTimer = 0.7f;
        }
    }
}

void ButtonSequence::drawFloor(Renderer& r, Vec2 cam, float t) {
    for (int i = 0; i < static_cast<int>(buttons.size()); ++i) {
        const Vec2 c = tileCenter(buttons[static_cast<size_t>(i)]) - cam;
        const bool lit = flash.count(i) || (inProgress && std::find(pressed.begin(), pressed.end(), i) != pressed.end());
        Color col = BUTTON_COLORS[i % 4];
        float k = lit ? 1.f : 0.35f;
        if (solved) {
            col = cfg::SUCCESS;
            k = 0.8f;
        } else if (failTimer > 0) {
            col = cfg::DANGER;
            k = 0.5f + 0.5f * std::abs(std::sin(t * 12));
        }
        if (lit || solved) r.glow(c, 46, col, 1.f);
        r.fillCircle(c, 17, col.scaled(0.18f * k + 0.05f));
        r.drawCircle(c, 17, col.scaled(k), 3);
        r.fillCircle(c, 7, col.scaled(k));
        auto it = std::find(marks.begin(), marks.end(), i);
        if (it != marks.end())
            r.text(std::to_wstring(it - marks.begin() + 1), c + Vec2{15, -18}, 16, cfg::WARNING, Align::Center, true);
    }
}

void ButtonSequence::blocks(std::vector<gfx::Block>& out) const { out.push_back(deviceBlock(panel, solved ? cfg::SUCCESS : cfg::SYSTEM)); }

void ButtonSequence::drawOverlay(Renderer& r, Vec2 cam, float) {
    const Vec2 c = gfx::project(tileCenter(panel) - cam, cfg::DEVICE_HEIGHT);
    const int n = static_cast<int>(buttons.size());
    for (int i = 0; i < n; ++i) {
        const Vec2 led = c + Vec2{(i - (n - 1) / 2.f) * 8.f, 0};
        r.fillCircle(led, 3, flash.count(i) && !inProgress ? BUTTON_COLORS[i % 4] : cfg::SYSTEM.scaled(0.4f));
    }
}

bool ButtonSequence::randomize(Rng& g, const TileSet& avoid) {
    if (!def.zone.valid() || inProgress) return false;
    std::vector<Tile> cand;
    for (int y = def.zone.y0; y <= def.zone.y1; ++y)
        for (int x = def.zone.x0; x <= def.zone.x1; ++x) {
            const Tile t{x, y};
            if (room.isFloorFree(t) && !avoid.count(t) && std::max(std::abs(t.x - panel.x), std::abs(t.y - panel.y)) >= 2) cand.push_back(t);
        }
    const size_t n = buttons.size();
    if (cand.size() < n * 3) return false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        std::vector<Tile> chosen;
        for (int k = 0; k < 40 && chosen.size() < n; ++k) {
            const Tile t = g.pick(cand);
            bool spaced = true;
            for (const Tile& c : chosen) spaced = spaced && std::max(std::abs(c.x - t.x), std::abs(c.y - t.y)) >= 2;
            if (spaced) chosen.push_back(t);
        }
        if (chosen.size() != n) continue;
        buttons = chosen;
        std::vector<int> fresh = order;
        for (int k = 0; k < 10 && fresh == order; ++k) g.shuffle(fresh);
        order = fresh;
        marks.clear();
        pressed.clear();
        demo.clear();
        return true;
    }
    return false;
}

// ==========================================================================
// 3. RouteMemory
// ==========================================================================
RouteMemory::RouteMemory(Room& r, const PuzzleDef& d, const Elements& el, int idx) : Puzzle(PuzzleKind::RouteMemory, r, d, el, idx) {
    pad = element("pad");
    goal = element("goal");
}

std::vector<Interactable> RouteMemory::interactables() {
    if (solved) return {};
    std::vector<Interactable> items;
    items.push_back({tileCenter(pad), (ready || recording) ? L"Перезаписать маршрут" : L"Начать запись маршрута",
                     [this](GameAPI& g) {
                         if (inProgress) {
                             g.notify(L"Помощник уже идёт по маршруту.");
                             return;
                         }
                         recording = true;
                         ready = false;
                         route = {pad};
                         if (g.player().tile() != pad) route.push_back(g.player().tile());
                         audio::play("rec");
                         g.notify(L"● REC: дойди до цели, не наступая на повреждённые сектора.", L"СИСТЕМА", cfg::DANGER);
                     },
                     cfg::INTERACT_RANGE, cfg::DANGER});
    if (ready)
        items.push_back({tileCenter(goal), L"Дать задачу Помощнику: пройти маршрут", promptAction(), cfg::INTERACT_RANGE, cfg::WARNING});
    else
        items.push_back({tileCenter(goal), L"Цель: сначала запиши маршрут",
                         [](GameAPI& g) { g.notify(L"Маршрут не записан. Встань на площадку REC и нажми E."); }, cfg::INTERACT_RANGE,
                         cfg::WARNING});
    return items;
}

void RouteMemory::stopRecording() {
    recording = false;
    if (!ready) route.clear();
}

void RouteMemory::update(float, GameAPI& g) {
    if (!recording || !active) return;
    const Tile t = g.player().tile();
    if (t != route.back()) {
        auto it = std::find(route.begin(), route.end(), t);
        if (it != route.end())
            route.erase(it + 1, route.end());  // петли вырезаем
        else
            route.push_back(t);
    }
    if (t == goal) {
        recording = false;
        ready = true;
        audio::play("success");
        g.notify(L"Маршрут записан: " + std::to_wstring(route.size() - 1) + L" шагов. Нажми E и дай задачу Помощнику.", L"СИСТЕМА",
                 cfg::SUCCESS);
    }
}

bool RouteMemory::buildPlan(Assistant& a, const Intent&, Script& out) {
    if (!ready || route.size() < 2) {
        lastError = L"Маршрут не записан. Встань на площадку REC и нажми E.";
        return false;
    }
    std::vector<Vec2> pts;
    for (size_t i = 1; i < route.size(); ++i) pts.push_back(tileCenter(route[i]));
    out.push_back(std::make_unique<GoToStep>(route.front()));
    out.push_back(std::make_unique<WalkStep>(std::move(pts), cfg::ASSISTANT_TASK_SPEED, true));
    out.push_back(std::make_unique<CallStep>([this] {
        inProgress = false;
        complete = true;
        room.particles.burst(tileCenter(goal), cfg::SUCCESS, 30, 220, 0.9f);
    }));
    accept(a);
    return true;
}

void RouteMemory::drawFloor(Renderer& r, Vec2 cam, float t) {
    const Vec2 p = tileCenter(pad) - cam;
    const Color rec = recording ? cfg::DANGER : cfg::DANGER.scaled(0.6f);
    const float pulse = 0.5f + 0.5f * std::sin(t * (recording ? 8.f : 2.f));
    r.glow(p, 40, rec, 0.4f + 0.5f * pulse);
    r.fillCircle(p, 19, rec.scaled(0.2f));
    r.drawCircle(p, 19, rec, 2);
    r.fillCircle(p, 6 + 3 * pulse, rec);
    r.text(L"REC", p + Vec2{0, 26}, 12, rec, Align::Center, true);

    const Vec2 gpos = tileCenter(goal) - cam;
    const Color gc = complete ? cfg::SUCCESS : cfg::WARNING;
    r.glow(gpos, 40, gc, 0.6f);
    gfx::neonPolygon(r, gc, gfx::regularPolygon(gpos, 17, 4, t * 1.5f), 2, 0.3f);

    if (route.size() >= 2 && (recording || ready)) {
        const Color c = cfg::WARNING.scaled(0.9f);
        for (size_t i = 1; i < route.size(); ++i) {
            const Vec2 a = tileCenter(route[i - 1]) - cam, b = tileCenter(route[i]) - cam;
            gfx::dashedLine(r, c, a, b, 8, 6, t * 30, 2);
            r.fillCircle(b, 3, c);
        }
    }
}

bool RouteMemory::randomize(Rng& g, const TileSet&) {
    if (inProgress) return false;
    const TileSet& field = room.field;
    std::vector<Tile> exits;
    TileSet entries;
    for (const Tile& t : field) {
        for (const Tile& d : DIRS4) {
            const Tile n = t + d;
            if (n == goal) exits.push_back(t);
            else if (!field.count(n) && !room.blocksPlayer(n)) entries.insert(t);
        }
    }
    if (exits.empty() || entries.empty()) return false;
    // Случайный поиск в глубину от цели до любого входа — это и есть безопасная тропа.
    const Tile start = g.pick(exits);
    TileMap<Tile> parent;
    std::vector<Tile> stack{start};
    parent[start] = start;
    std::optional<Tile> found;
    while (!stack.empty() && !found) {
        const Tile cur = stack.back();
        stack.pop_back();
        if (entries.count(cur) && cur != start) {
            found = cur;
            break;
        }
        std::vector<Tile> next;
        for (const Tile& d : DIRS4)
            if (field.count(cur + d) && !parent.count(cur + d)) next.push_back(cur + d);
        g.shuffle(next);
        for (const Tile& n : next) {
            parent[n] = cur;
            stack.push_back(n);
        }
    }
    if (!found) return false;
    TileSet path;
    for (Tile t = *found;; t = parent[t]) {
        path.insert(t);
        if (t == start) break;
    }
    room.mines.clear();
    for (const Tile& t : field)
        if (!path.count(t) && g.chance(0.62f)) room.mines.insert(t);
    room.revealedMines.clear();
    route.clear();
    ready = recording = false;
    return true;
}

// ==========================================================================
// 4. LinkObjects
// ==========================================================================
LinkObjects::LinkObjects(Room& r, const PuzzleDef& d, const Elements& el, int idx) : Puzzle(PuzzleKind::LinkObjects, r, d, el, idx) {
    key = element("key");
    lock = element("lock");
}

bool LinkObjects::marked() const { return room.linkSource == this; }

std::vector<Interactable> LinkObjects::interactables() {
    if (solved || linked) return {};
    return {
        {tileCenter(key), marked() ? L"Ключ " + def.symbol + L" отмечен" : L"Отметить ключ " + def.symbol,
         [this](GameAPI& g) {
             room.linkSource = this;
             audio::play("mark");
             g.notify(L"Ключ " + def.symbol + L" отмечен. Подойди к замку и дай задачу Помощнику.");
         },
         cfg::KEY_INTERACT_RANGE, def.color},
        {tileCenter(lock), L"Дать задачу Помощнику: замок " + def.symbol, promptAction(), cfg::INTERACT_RANGE, def.color},
    };
}

bool LinkObjects::buildPlan(Assistant& a, const Intent&, Script& out) {
    LinkObjects* source = room.linkSource;
    if (!source) {
        lastError = L"Сначала отметь ключ: подойди к нему и нажми E.";
        return false;
    }
    if (source != this) {
        lastError = L"Ключ " + source->def.symbol + L" не подходит к замку " + def.symbol + L". Нужна пара одной формы.";
        return false;
    }
    out.push_back(std::make_unique<GoToStep>(lock, true));
    out.push_back(std::make_unique<WaitStep>(0.2f));
    out.push_back(std::make_unique<BeamStep>(tileCenter(key), 0.9f));
    out.push_back(std::make_unique<CallStep>([this] {
        linked = true;
        inProgress = false;
        if (room.linkSource == this) room.linkSource = nullptr;
    }));
    accept(a);
    return true;
}

void LinkObjects::update(float dt, GameAPI&) {
    if (linked && !complete) {
        progress = std::min(1.f, progress + dt / 0.9f);
        if (progress >= 1.f) {
            complete = true;
            room.particles.burst(tileCenter(lock), def.color, 30, 200, 0.8f);
        }
    }
}

void LinkObjects::drawShape(Renderer& r, Vec2 c, float size, float rotation, Color col) const {
    if (def.shape == "diamond")
        gfx::neonPolygon(r, col, gfx::regularPolygon(c, size, 4, rotation), 2, 0.3f);
    else if (def.shape == "circle")
        gfx::neonCircle(r, col, c, size * 0.8f, 2, 0.3f);
    else
        gfx::neonPolygon(r, col, gfx::regularPolygon(c, size, 3, rotation - PI / 2), 2, 0.3f);
}

void LinkObjects::drawFloor(Renderer& r, Vec2 cam, float t) {
    const Vec2 k = tileCenter(key) - cam;
    r.glow(k, 46, def.color, 0.9f);
    if (marked()) r.drawCircle(k, 26 + 3 * std::sin(t * 6), def.color, 2);
}

void LinkObjects::blocks(std::vector<gfx::Block>& out) const {
    out.push_back(deviceBlock(lock, complete ? cfg::SUCCESS : def.color, cfg::DEVICE_HEIGHT, 6));
    out.push_back(deviceBlock(key, def.color, 0.02f, 16));
}

void LinkObjects::drawOverlay(Renderer& r, Vec2 cam, float t) {
    const Vec2 k = gfx::project(tileCenter(key) - cam, 0.02f);
    const Vec2 l = gfx::project(tileCenter(lock) - cam, cfg::DEVICE_HEIGHT);
    drawShape(r, k, 13, t * 1.4f, def.color);
    drawShape(r, l, 10, 0, complete ? cfg::SUCCESS : def.color);
    if (linked) {
        const Vec2 end = k.lerp(l, progress);
        r.line(k, end, def.color.scaled(0.35f), 7);
        gfx::dashedLine(r, def.color, k, end, 12, 6, -t * 60, 3);
        r.glow(end, 26, def.color, 0.8f);
    }
}

// ==========================================================================
// 5. ContextTask
// ==========================================================================
ContextTask::ContextTask(Room& r, const PuzzleDef& d, const Elements& el, int idx) : Puzzle(PuzzleKind::ContextTask, r, d, el, idx) {
    console = element("console");
    sw = element("switch");
    switchSpots = {sw};
    for (const Tile& t : elements("switch_alt")) switchSpots.push_back(t);
    auto sh = elements("shadow");
    if (!sh.empty()) shadow = sh.front();
    auto fa = elements("fan");
    if (!fa.empty()) fan = fa.front();
    for (const Tile& t : elements("dark")) dark.insert(t);
    if (shadow) dark.insert(*shadow);
    for (const Tile& t : elements("wind")) wind.insert(t);
}

std::vector<int> ContextTask::objects() const {
    if (def.ctx.scenario == "fan") return {O_FAN};
    if (def.ctx.scenario == "shadow") return {O_LIGHT, O_SHADOW};
    return {O_BOSS};
}

bool ContextTask::matches(const Intent& in) const {
    const auto objs = objects();
    return std::find(objs.begin(), objs.end(), in.object) != objs.end();
}

void ContextTask::solidTiles(TileSet& out) const {
    out.insert(console);
    out.insert(sw);
    if (fan) out.insert(*fan);
}

void ContextTask::occupiedTiles(TileSet& out) const {
    out.insert(console);
    for (const Tile& t : switchSpots) out.insert(t);
    if (fan) out.insert(*fan);
    if (shadow) out.insert(*shadow);
}

std::vector<Interactable> ContextTask::interactables() {
    if (solved) return {};
    return {{tileCenter(console), inProgress ? L"Помощник выполняет задачу…" : L"Сформулировать задачу (промпт)", promptAction(),
             cfg::INTERACT_RANGE, cfg::SYSTEM}};
}

bool ContextTask::buildPlan(Assistant& a, const Intent& intent, Script& out) {
    const auto& ctx = def.ctx;
    // 0 — верно, 1 — нет цели, 2 — цель не та, 3 — буквальное (неверное) действие
    int verdict = 3;
    if (intent.action == ctx.action && intent.object == ctx.object) {
        if (std::find(ctx.goals.begin(), ctx.goals.end(), intent.goal) != ctx.goals.end())
            verdict = 0;
        else
            verdict = intent.goal == G_NONE ? 1 : 2;
    }
    if (a.level >= 5 && matches(intent)) verdict = 0;  // Оракул угадывает намерение
    Vec2 beamTarget = tileCenter(sw);
    float beamTime = 0.5f;
    if (ctx.scenario == "boss" && room.boss) {
        beamTarget = room.boss->pos;
        beamTime = 1.4f;
    }
    out.push_back(std::make_unique<GoToStep>(sw, true));
    out.push_back(std::make_unique<WaitStep>(0.3f));
    out.push_back(std::make_unique<BeamStep>(beamTarget, beamTime));
    Assistant* who = &a;
    out.push_back(std::make_unique<CallStep>([this, intent, verdict, who] { apply(intent, verdict, *who); }));
    accept(a);
    return true;
}

void ContextTask::apply(const Intent& intent, int verdict, Assistant& a) {
    inProgress = false;
    if (verdict == 0) {
        effectOn = true;
        audio::play("success");
        notify(def.ctx.success, a.name, a.color);
        return;
    }
    audio::play("error");
    if (verdict == 1) {
        a.say(L"Сделал бы «" + intentPhrase(intent) + L"», но ты не сказал зачем. Без цели не рискну — уточни задачу.", L"?");
    } else if (verdict == 2) {
        a.say(L"Цель «" + std::wstring(goalName(intent.goal)) + L"» не решает задачу — откатываю изменения.", L"?");
    } else if (def.ctx.scenario == "fan" && intent.action == A_ON) {
        a.say(L"Разогнал вентилятор! Ветер стал ещё сильнее. Наверное, ты хотел другого?", L"?");
    } else if (def.ctx.scenario == "shadow" && intent.action == A_OFF) {
        a.say(L"Выключил свет — стало ещё темнее. Тень довольна.", L"?");
    } else {
        a.say(L"Выполнил буквально: «" + intentPhrase(intent) + L"». Задачу это не решает.", L"?");
    }
}

void ContextTask::update(float dt, GameAPI& g) {
    if (effectOn && !complete) {
        effect = std::min(1.f, effect + dt / 1.4f);
        if (effect >= 1.f) complete = true;
    }
    if (def.ctx.scenario == "fan" && fan) {
        fanAngle += dt * 14 * (1.f - effect);
        if (hazardActive() && active) {
            if (wind.count(g.player().tile())) g.player().externalPush += def.ctx.windDir * cfg::WIND_SPEED;
            windTimer -= dt;
            if (windTimer <= 0 && !wind.empty()) {
                windTimer = 0.03f;
                auto it = wind.begin();
                std::advance(it, rng().range(0, static_cast<int>(wind.size()) - 1));
                const Vec2 p = tileCenter(*it) + Vec2{rng().uniform(-TILE / 2, TILE / 2), rng().uniform(-TILE / 2, TILE / 2)};
                room.particles.stream(p, def.ctx.windDir * 420.f, Color::rgb(90, 140, 200), 0.5f, 2);
            }
        }
    }
    if (def.ctx.scenario == "shadow" && shadow && hazardActive() && active) {
        if (g.player().pos.distance(tileCenter(*shadow)) < cfg::SHADOW_RADIUS)
            g.playerCaught(L"Тень поглотила тебя. Тени боятся света…");
    }
}

void ContextTask::drawFloor(Renderer& r, Vec2 cam, float t) {
    if (!dark.empty()) {
        const float alpha = 0.88f * (1.f - effect);
        for (const Tile& d : dark) {
            r.fillRect(tileRect(d).moved(-cam), Color::rgb(0, 0, 0, alpha));
            if (effect > 0) r.glow(tileCenter(d) - cam, 40, Color::rgb(255, 230, 170), 0.5f * effect);
        }
    }
    if (shadow && effect < 1.f) {
        const Vec2 c = tileCenter(*shadow) - cam;
        const float fade = 1.f - effect;
        const Color ring = cfg::DANGER.scaled(0.45f * fade);
        for (int i = 0; i < 24; ++i) {
            const float a0 = i / 24.f * TAU + t * 0.3f, a1 = a0 + TAU / 48;
            r.line(c + Vec2::polar(a0, cfg::SHADOW_RADIUS), c + Vec2::polar(a1, cfg::SHADOW_RADIUS), ring, 2);
        }
        std::vector<Vec2> pts;
        for (int i = 0; i < 14; ++i) {
            const float a = i / 14.f * TAU;
            pts.push_back(c + Vec2::polar(a, (26 + 7 * std::sin(t * 4 + i * 1.9f)) * (0.4f + 0.6f * fade)));
        }
        r.fillPolygon(pts, Color::rgb(4, 0, 8));
        r.drawPolygon(pts, Color::rgb(150, 40, 220).scaled(fade), 2);
        for (int side : {-1, 1}) {
            const Vec2 eye = c + Vec2{side * 8.f, -4};
            r.glow(eye, 12, cfg::DANGER, fade);
            r.fillCircle(eye, 3, cfg::DANGER.scaled(fade));
        }
    }
}

void ContextTask::blocks(std::vector<gfx::Block>& out) const {
    const Color c = (effectOn || solved) ? cfg::SUCCESS : cfg::SYSTEM;
    out.push_back(deviceBlock(console, c));
    out.push_back(deviceBlock(sw, c, cfg::DEVICE_HEIGHT, 14));
    if (fan) out.push_back(deviceBlock(*fan, Color::rgb(90, 140, 200), 0.02f, 2));
}

void ContextTask::drawOverlay(Renderer& r, Vec2 cam, float t) {
    const Vec2 c = gfx::project(tileCenter(console) - cam, cfg::DEVICE_HEIGHT);
    r.text(static_cast<int>(t * 2) % 2 ? L">_" : L">", c, 16, solved ? cfg::SUCCESS : cfg::SYSTEM, Align::Center, true);
    const Vec2 s = gfx::project(tileCenter(sw) - cam, cfg::DEVICE_HEIGHT);
    r.glow(s, 18, effectOn ? cfg::SUCCESS : cfg::WARNING, 0.5f);
    if (fan) {
        const Vec2 f = tileCenter(*fan) - cam;
        const Color fc = Color::mix(Color::rgb(120, 190, 255), cfg::DIM, effect);
        r.glow(f, 70, fc, 0.5f * (1 - effect) + 0.1f);
        r.drawCircle(f, 60, fc, 2);
        for (int i = 0; i < 4; ++i) {
            const float a = fanAngle + i * PI / 2;
            const Vec2 tip = f + Vec2::polar(a, 54), side = f + Vec2::polar(a + 0.45f, 30);
            r.fillPolygon({f, side, tip}, fc.scaled(0.35f));
            r.line(f, tip, fc, 2);
        }
        r.fillCircle(f, 8, fc);
    }
}

bool ContextTask::randomize(Rng& g, const TileSet&) {
    if (inProgress || switchSpots.size() < 2) return false;
    Tile next = sw;
    for (int k = 0; k < 10 && next == sw; ++k) next = g.pick(switchSpots);
    sw = next;
    return true;
}

std::unique_ptr<Puzzle> createPuzzle(Room& room, const PuzzleDef& def, const Elements& el, int index) {
    switch (def.kind) {
        case PuzzleKind::PushBox: return std::make_unique<PushBox>(room, def, el, index);
        case PuzzleKind::ButtonSequence: return std::make_unique<ButtonSequence>(room, def, el, index);
        case PuzzleKind::RouteMemory: return std::make_unique<RouteMemory>(room, def, el, index);
        case PuzzleKind::LinkObjects: return std::make_unique<LinkObjects>(room, def, el, index);
        default: return std::make_unique<ContextTask>(room, def, el, index);
    }
}
