#include "room.h"

#include <deque>
#include <stdexcept>

#include "assistant.h"
#include "audio.h"
#include "boss.h"
#include "game_api.h"
#include "player.h"

namespace {

float angleDiff(float a, float b) {
    float d = std::fmod(a - b + PI, TAU);
    if (d < 0) d += TAU;
    return d - PI;
}

uint32_t tileHash(Tile t) {
    uint32_t h = static_cast<uint32_t>(t.x) * 73856093u ^ static_cast<uint32_t>(t.y) * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return h ^ (h >> 15);
}

}  // namespace

// ==========================================================================
// Страж-глитч
// ==========================================================================
Sentinel::Sentinel(std::vector<Vec2> wp) : waypoints_(std::move(wp)) {
    pos = waypoints_.front();
    index_ = waypoints_.size() > 1 ? 1 : 0;
    const Vec2 target = waypoints_.size() > 1 ? waypoints_[1] : pos + Vec2{1, 0};
    baseAngle_ = angle = std::atan2(target.y - pos.y, target.x - pos.x);
    scan_ = rng().uniform(0, TAU);
}

void Sentinel::randomize(Rng& g) {
    if (waypoints_.size() < 2) return;
    if (g.chance(0.5f)) std::reverse(waypoints_.begin(), waypoints_.end());
    const size_t start = static_cast<size_t>(g.range(0, static_cast<int>(waypoints_.size()) - 1));
    pos = waypoints_[start];
    index_ = (start + 1) % waypoints_.size();
    const Vec2 next = waypoints_[index_];
    baseAngle_ = angle = std::atan2(next.y - pos.y, next.x - pos.x);
    wait_ = g.uniform(0.f, 1.5f);
    alert = 0.f;
}

bool Sentinel::update(float dt, const Room& room, Vec2 playerPos, bool hidden) {
    scan_ += dt;
    float desired;
    if (waypoints_.size() > 1 && wait_ <= 0) {
        const Vec2 target = waypoints_[index_];
        const Vec2 delta = target - pos;
        const float dist = delta.length();
        const float step = cfg::SENTINEL_SPEED * dt;
        if (dist <= step) {
            pos = target;
            index_ = (index_ + 1) % waypoints_.size();
            wait_ = cfg::SENTINEL_PAUSE;
            const Vec2 next = waypoints_[index_];
            baseAngle_ = std::atan2(next.y - pos.y, next.x - pos.x);
        } else {
            pos += delta / dist * step;
            baseAngle_ = std::atan2(delta.y, delta.x);
        }
        desired = baseAngle_;
    } else {
        wait_ -= dt;
        desired = baseAngle_ + std::sin(scan_ * 2.2f) * 0.75f;  // осматривается
    }
    const float turn = cfg::SENTINEL_TURN_SPEED * dt;
    angle += clampf(angleDiff(desired, angle), -turn, turn);
    if (!hidden && canSee(playerPos, room))
        alert = std::min(1.f, alert + dt / cfg::DETECTION_TIME);
    else
        alert = std::max(0.f, alert - dt * 0.8f);
    return alert >= 1.f;
}

bool Sentinel::canSee(Vec2 target, const Room& room) const {
    const Vec2 d = target - pos;
    const float dist = d.length();
    if (dist < 1.f) return true;
    if (dist > cfg::SENTINEL_VIEW_RANGE) return false;
    if (std::abs(angleDiff(std::atan2(d.y, d.x), angle)) > cfg::SENTINEL_FOV / 2) return false;
    return room.lineOfSight(pos, target);
}

void Sentinel::drawCone(Renderer& r, Vec2 cam, const Room& room) const {
    constexpr int RAYS = 22;
    std::vector<Vec2> pts{pos - cam};
    for (int i = 0; i <= RAYS; ++i) {
        const float a = angle - cfg::SENTINEL_FOV / 2 + cfg::SENTINEL_FOV * i / RAYS;
        pts.push_back(room.castRay(pos, a, cfg::SENTINEL_VIEW_RANGE) - cam);
    }
    r.setAdditive(true);
    r.fillPolygon(pts, Color::mix(Color::rgb(70, 58, 12), Color::rgb(110, 18, 24), alert));
    r.setAdditive(false);
    const Color edge = Color::mix(cfg::SENTINEL, cfg::DANGER, alert).scaled(0.55f);
    r.polyline(std::vector<Vec2>(pts.begin() + 1, pts.end()), edge, 1);
    r.line(pts.front(), pts[1], edge, 1);
    r.line(pts.front(), pts.back(), edge, 1);
}

void Sentinel::draw(Renderer& r, Vec2 cam, float t) const {
    const Vec2 c = pos - cam;
    const Color col = Color::mix(cfg::SENTINEL, cfg::DANGER, alert);
    r.glow(c, 36, col, 0.8f);
    gfx::neonPolygon(r, col, gfx::regularPolygon(c, 17, 4, t * 2), 2, 0.25f);
    r.fillCircle(c + Vec2::polar(angle, 8), 4, col);
    if (alert > 0.15f) r.text(L"!", c + Vec2{0, -32}, 22, cfg::DANGER, Align::Center, true);
}

// ==========================================================================
// Комната
// ==========================================================================
Room::Room(const LevelDef& d) : def(d) {
    height = static_cast<int>(def.map.size());
    for (const auto& row : def.map) width = std::max(width, static_cast<int>(row.size()));
    std::map<std::string, Elements> elements;
    std::map<std::string, std::vector<std::pair<int, Tile>>> waypoints;
    std::optional<Tile> bossTile;
    for (int y = 0; y < height; ++y) {
        const std::string& row = def.map[static_cast<size_t>(y)];
        for (int x = 0; x < width; ++x) {
            const char ch = x < static_cast<int>(row.size()) ? row[static_cast<size_t>(x)] : ' ';
            const Tile t{x, y};
            switch (ch) {
                case '#': baseWalls_.insert(t); continue;
                case ' ': voids.insert(t); continue;
                case '=': glass.insert(t); continue;
                case ',': field.insert(t); continue;
                case 'x':
                    field.insert(t);
                    mines.insert(t);
                    continue;
                case 'P': spawn = tileCenter(t); continue;
                case 'T': terminal = t; continue;
                case 'D': doors.push_back(t); continue;
                case '.': continue;
                default: break;
            }
            auto it = def.legend.find(ch);
            if (it == def.legend.end())
                throw std::runtime_error(std::string("unknown map symbol '") + ch + "' at " + std::to_string(x) + "," + std::to_string(y));
            const LegendEntry& e = it->second;
            if (e.kind == "gate") {
                gates[t] = e.id;
            } else if (e.kind == "waypoint") {
                waypoints[e.id].push_back({e.index, t});
            } else if (e.kind == "boss") {
                bossTile = t;
                voids.insert(t);
            } else {
                if (e.kind == "switch_alt") baseWalls_.insert(t);  // запасное место выключателя — пока стена
                elements[e.id][e.kind].push_back({t, e.index});
            }
        }
    }
    for (size_t i = 0; i < def.puzzles.size(); ++i)
        puzzles.push_back(createPuzzle(*this, def.puzzles[i], elements[def.puzzles[i].id], static_cast<int>(i)));
    for (auto& [id, list] : waypoints) {
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<Vec2> pts;
        for (const auto& [i, t] : list) pts.push_back(tileCenter(t));
        sentinels.emplace_back(std::move(pts));
    }
    if (!def.bossPhases.empty() && bossTile) boss = std::make_unique<Vibecoder>(tileCenter(*bossTile), def.bossPhases, *this);
    refreshSolids();
}

Room::~Room() = default;

void Room::refreshSolids() {
    walls = baseWalls_;
    staticSolid_.clear();
    for (const auto& p : puzzles) {
        p->solidTiles(staticSolid_);
        if (auto* ct = dynamic_cast<ContextTask*>(p.get())) walls.erase(ct->sw);  // активный выключатель — не стена
    }
    if (terminal) staticSolid_.insert(*terminal);
    buildWallBlocks();
}

void Room::buildWallBlocks() {
    wallBlocks_.clear();
    const Tile dirs[4] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
    for (const Tile& t : walls) {
        gfx::Block b;
        bool any = false;
        for (int i = 0; i < 4; ++i) {
            b.exposed[i] = !walls.count(t + dirs[i]);
            any = any || b.exposed[i];
        }
        if (!any) continue;
        b.rect = tileRect(t);
        b.height = cfg::WALL_HEIGHT;
        b.top = cfg::WALL;
        b.side = cfg::WALL_SIDE;
        b.edge = cfg::WALL_EDGE;
        wallBlocks_[t] = b;
    }
    for (const Tile& t : glass) {
        gfx::Block b;
        for (int i = 0; i < 4; ++i) b.exposed[i] = !glass.count(t + dirs[i]);
        b.rect = tileRect(t);
        b.height = cfg::GLASS_HEIGHT;
        b.top = Color::rgb(16, 40, 52);
        b.side = Color::rgb(12, 30, 40);
        b.edge = cfg::GLASS;
        b.edgeWidth = 1.f;
        wallBlocks_[t] = b;
    }
}

// --------------------------------------------------------------------------
// Клетки
// --------------------------------------------------------------------------
bool Room::gateClosed(Tile t) const {
    auto it = gates.find(t);
    return it != gates.end() && !opened.count(it->second);
}

bool Room::doorClosed(Tile t) const { return !doorOpen && std::find(doors.begin(), doors.end(), t) != doors.end(); }

bool Room::blocksPlayer(Tile t) const { return isWall(t) || glass.count(t) || staticSolid_.count(t) || gateClosed(t) || doorClosed(t); }

bool Room::blocksSight(Tile t) const { return !inBounds(t) || walls.count(t) || gateClosed(t) || doorClosed(t); }

TileSet Room::boxTiles() const {
    TileSet out;
    for (const auto& p : puzzles) p->blockingTiles(out);
    return out;
}

bool Room::assistantCanWalk(Tile t, bool allowField, bool ignoreBoxes) const {
    if (blocksPlayer(t) || std::find(doors.begin(), doors.end(), t) != doors.end()) return false;
    if (!allowField && field.count(t)) return false;
    return ignoreBoxes || !boxTiles().count(t);
}

bool Room::isFloorFree(Tile t) const {
    if (!inBounds(t) || blocksPlayer(t) || field.count(t) || gates.count(t)) return false;
    if (std::find(doors.begin(), doors.end(), t) != doors.end()) return false;
    for (const auto& [gt, id] : gates)
        if (std::abs(gt.x - t.x) + std::abs(gt.y - t.y) <= 1) return false;  // не загораживать проходы
    for (const Tile& d : doors)
        if (std::abs(d.x - t.x) + std::abs(d.y - t.y) <= 1) return false;
    for (const auto& s : puzzles)
        if (auto* ct = dynamic_cast<ContextTask*>(s.get()))
            if (ct->dark.count(t) || ct->wind.count(t)) return false;
    return true;
}

std::optional<Tile> Room::freeTileNear(Tile start) const {
    if (assistantCanWalk(start)) return start;
    TileSet seen{start};
    std::deque<Tile> q{start};
    while (!q.empty()) {
        const Tile c = q.front();
        q.pop_front();
        for (const Tile& d : DIRS4) {
            const Tile n = c + d;
            if (seen.count(n) || !inBounds(n) || std::abs(n.x - start.x) + std::abs(n.y - start.y) > 8) continue;
            seen.insert(n);
            if (assistantCanWalk(n)) return n;
            if (!blocksPlayer(n)) q.push_back(n);
        }
    }
    return std::nullopt;
}

std::vector<Tile> Room::findPath(Tile start, const TileSet& goals, const std::function<bool(Tile)>& passable) const {
    if (goals.empty()) return {};
    if (goals.count(start)) return {start};
    TileMap<Tile> parent;
    parent[start] = start;
    std::deque<Tile> q{start};
    while (!q.empty()) {
        const Tile c = q.front();
        q.pop_front();
        for (const Tile& d : DIRS4) {
            const Tile n = c + d;
            if (parent.count(n) || !passable(n)) continue;
            parent[n] = c;
            if (goals.count(n)) {
                std::vector<Tile> path{n};
                for (Tile t = n; t != start;) {
                    t = parent[t];
                    path.push_back(t);
                }
                std::reverse(path.begin(), path.end());
                return path;
            }
            q.push_back(n);
        }
    }
    return {};
}

std::vector<Tile> Room::findPathNearest(Tile start, Tile target, const std::function<bool(Tile)>& passable) const {
    TileMap<Tile> parent;
    parent[start] = start;
    std::deque<Tile> q{start};
    Tile best = start;
    float bestDist = tileDist(start, target);
    while (!q.empty()) {
        const Tile c = q.front();
        q.pop_front();
        if (c == target) {
            best = c;
            break;
        }
        for (const Tile& d : DIRS4) {
            const Tile n = c + d;
            if (parent.count(n) || !passable(n)) continue;
            parent[n] = c;
            const float dist = tileDist(n, target);
            if (dist < bestDist) {
                best = n;
                bestDist = dist;
            }
            q.push_back(n);
        }
    }
    std::vector<Tile> path{best};
    for (Tile t = best; t != start;) {
        t = parent[t];
        path.push_back(t);
    }
    std::reverse(path.begin(), path.end());
    return path;
}

bool Room::lineOfSight(Vec2 a, Vec2 b) const {
    const Vec2 d = b - a;
    const int steps = std::max(1, static_cast<int>(d.length() / 8));
    for (int i = 1; i < steps; ++i)
        if (blocksSight(tileOf(a + d * (static_cast<float>(i) / steps)))) return false;
    return true;
}

Vec2 Room::castRay(Vec2 origin, float angle, float maxDistance) const {
    const Vec2 dir = Vec2::polar(angle);
    for (float dist = 8; dist < maxDistance; dist += 8)
        if (blocksSight(tileOf(origin + dir * dist))) return origin + dir * (dist - 4);
    return origin + dir * maxDistance;
}

// --------------------------------------------------------------------------
// Логика
// --------------------------------------------------------------------------
int Room::solvedCount() const {
    int n = 0;
    for (const auto& p : puzzles) n += p->solved ? 1 : 0;
    return n;
}

bool Room::allSolved() const { return solvedCount() == static_cast<int>(puzzles.size()); }

bool Room::recording() const {
    for (const auto& p : puzzles)
        if (auto* rm = dynamic_cast<RouteMemory*>(p.get()); rm && rm->recording) return true;
    return false;
}

std::wstring Room::veto(const Assistant& a, const Puzzle& p) const { return boss ? boss->veto(a, p) : L""; }

void Room::revealMine(Tile t) {
    if (mines.count(t) && !revealedMines.count(t)) {
        revealedMines.insert(t);
        particles.burst(tileCenter(t), cfg::DANGER, 22, 180, 0.6f);
        audio::play("glitch");
    }
}

void Room::onPlayerCaught() {
    for (auto& s : sentinels) s.alert = 0.f;
    for (auto& p : puzzles) p->resetTransient();
}

std::vector<int> Room::contextObjects() const {
    std::vector<int> out{O_ME};
    for (const auto& p : puzzles) {
        if (p->solved || !p->active) continue;
        for (int o : p->objects())
            if (std::find(out.begin(), out.end(), o) == out.end()) out.push_back(o);
    }
    return out;
}

std::vector<Interactable> Room::interactables(GameAPI& g) {
    std::vector<Interactable> items;
    if (terminal) {
        AssistantRoster& roster = g.roster();
        items.push_back({tileCenter(*terminal),
                         roster.summoned ? L"Перезагрузить Помощника рядом с собой" : L"Призвать Помощника: " + roster.current().name,
                         [](GameAPI& game) {
                             AssistantRoster& ro = game.roster();
                             if (!ro.summoned) {
                                 Assistant& a = ro.summon(game.player().pos);
                                 game.notify(L"На связи! Интеллект " + std::to_wstring(a.level) + L"/5. " + ASSISTANT_DESCRIPTIONS[a.level],
                                             a.name, a.color);
                                 return;
                             }
                             Assistant& a = ro.current();
                             if (a.busy()) {
                                 game.notify(a.name + L" занят задачей.");
                                 return;
                             }
                             a.staying = false;
                             a.teleportNear(game.player().pos);
                             audio::play("summon");
                             game.notify(L"Помощник перезагружен рядом с тобой.");
                         },
                         cfg::INTERACT_RANGE, cfg::TERMINAL});
    }
    for (auto& p : puzzles) {
        if (p->active) {
            for (auto& it : p->interactables()) items.push_back(std::move(it));
        } else if (!p->solved) {
            items.push_back({p->anchor(), L"Модуль зашифрован Вайбкодером",
                             [](GameAPI& game) {
                                 audio::play("error");
                                 game.notify(L"Вайбкодер шифрует этот модуль. Сначала пройди текущую фазу.", L"СИСТЕМА", cfg::DANGER);
                             },
                             cfg::INTERACT_RANGE, cfg::DANGER});
        }
    }
    if (!doors.empty() && !doorOpen) {
        const int solved = solvedCount(), total = static_cast<int>(puzzles.size());
        items.push_back({tileCenter(doors.front()), L"Выход заперт",
                         [solved, total](GameAPI& game) {
                             audio::play("error");
                             game.notify(L"Дверь откроется, когда все головоломки будут решены (" + std::to_wstring(solved) + L"/" +
                                         std::to_wstring(total) + L").");
                         },
                         cfg::INTERACT_RANGE, cfg::DANGER});
    }
    return items;
}

bool Room::playerOnExit(Vec2 pos) const { return doorOpen && std::find(doors.begin(), doors.end(), tileOf(pos)) != doors.end(); }

void Room::onSolved(Puzzle& p) {
    opened.insert(p.def.id);
    audio::play("success");
    particles.burst(p.anchor(), cfg::SUCCESS, 36, 240, 0.9f);
    if (notify) notify(std::wstring(L"Головоломка решена: ") + p.title() + L".", L"СИСТЕМА", cfg::SUCCESS);
    bool gate = false;
    for (const auto& [t, id] : gates)
        if (id == p.def.id) {
            gate = true;
            particles.burst(tileCenter(t), cfg::SUCCESS, 16, 160, 0.6f);
        }
    if (gate) {
        audio::play("door");
        if (notify) notify(L"Проход открыт.", L"СИСТЕМА", cfg::SUCCESS);
    }
    if (onSolvedHook) onSolvedHook(p);
}

void Room::update(float dt, GameAPI& g) {
    time += dt;
    shake = std::max(0.f, shake - dt);
    for (auto& p : puzzles) p->update(dt, g);
    for (auto& p : puzzles)
        if (!p->solved && p->checkSolved()) {
            p->solved = true;
            onSolved(*p);
            if (boss) boss->onPuzzleSolved(*p, g);
        }
    Player& player = g.player();
    const Tile pt = player.tile();
    if (mines.count(pt)) {
        revealMine(pt);
        std::optional<Vec2> pad;
        for (const auto& p : puzzles)
            if (auto* rm = dynamic_cast<RouteMemory*>(p.get())) pad = tileCenter(rm->pad);
        g.playerCaught(L"Повреждённый сектор! Откат к площадке записи.", pad ? &*pad : nullptr, true);
    }
    for (const Tile& m : mines)
        if (!revealedMines.count(m) && tileCenter(m).distance(player.pos) < cfg::MINE_REVEAL_RADIUS) revealedMines.insert(m);
    for (auto& s : sentinels)
        if (s.update(dt, *this, player.pos, player.invulnerable > 0)) {
            g.playerCaught(L"Страж-глитч заметил тебя! Откат к началу уровня.");
            break;
        }
    if (boss) boss->update(dt, g);
    if (!doors.empty() && !doorOpen && allSolved()) {
        doorOpen = true;
        audio::play("door");
        if (notify) notify(L"Все задачи решены. Выход открыт!", L"СИСТЕМА", cfg::SUCCESS);
        for (const Tile& d : doors) particles.burst(tileCenter(d), cfg::SUCCESS, 30, 200, 0.9f);
    }
    particles.update(dt);
}

void Room::reshuffle(Rng& g, Vec2 playerPos, Vec2 assistantPos) {
    TileSet avoid{tileOf(playerPos), tileOf(assistantPos), tileOf(spawn)};
    for (const auto& p : puzzles) p->occupiedTiles(avoid);
    int changed = 0;
    for (auto& p : puzzles) {
        if (p->solved || p->inProgress || p->kind == PuzzleKind::LinkObjects) continue;
        if (p->randomize(g, avoid)) ++changed;
        p->occupiedTiles(avoid);
    }
    // Ключи и замки меняются местами: пары формы теперь стоят иначе.
    std::vector<LinkObjects*> links;
    for (auto& p : puzzles)
        if (auto* l = dynamic_cast<LinkObjects*>(p.get()); l && !l->solved && !l->linked && !l->inProgress) links.push_back(l);
    if (links.size() >= 2) {
        std::vector<Tile> keys, locks;
        for (auto* l : links) {
            keys.push_back(l->key);
            locks.push_back(l->lock);
        }
        const auto oldKeys = keys;
        for (int k = 0; k < 10 && keys == oldKeys; ++k) g.shuffle(keys);
        g.shuffle(locks);
        for (size_t i = 0; i < links.size(); ++i) {
            links[i]->key = keys[i];
            links[i]->lock = locks[i];
        }
        linkSource = nullptr;
        ++changed;
    }
    for (auto& s : sentinels) s.randomize(g);
    refreshSolids();
    if (changed > 0 || !sentinels.empty()) ++reshuffles;
}

// --------------------------------------------------------------------------
// Отрисовка
// --------------------------------------------------------------------------
void Room::drawFloor(Renderer& r, Vec2 cam) const {
    const int x0 = static_cast<int>(std::floor(cam.x / TILE)) - 2, y0 = static_cast<int>(std::floor(cam.y / TILE)) - 2;
    const int x1 = x0 + cfg::SCREEN_W / static_cast<int>(TILE) + 5, y1 = y0 + cfg::SCREEN_H / static_cast<int>(TILE) + 5;
    const Color wallGlow = cfg::WALL_EDGE.scaled(0.3f), voidGlow = Color::rgb(120, 50, 190);
    for (int y = std::max(0, y0); y <= std::min(height - 1, y1); ++y)
        for (int x = std::max(0, x0); x <= std::min(width - 1, x1); ++x) {
            const Tile t{x, y};
            if (walls.count(t)) continue;
            const RectF rect = tileRect(t).moved(-cam);
            const uint32_t h = tileHash(t);
            if (voids.count(t)) {
                r.fillRect(RectF::centered(rect.center() + Vec2{float(h % 30) - 15, float((h >> 8) % 30) - 15}, 2, 2), Color::rgb(40, 30, 70));
                continue;
            }
            if (field.count(t)) {
                r.fillRect(rect, cfg::FIELD);
                r.drawRect(rect, cfg::FIELD_GRID, 1);
                for (int i = 0; i < 3; ++i) {
                    const float px = rect.x + 4 + float((h >> (i * 5)) % 36), py = rect.y + 6 + float((h >> (i * 7 + 3)) % 34);
                    r.line({px, py}, {px + 3 + float((h >> i) % 6), py}, Color::rgb(70, 30, 55), 1);
                }
                continue;
            }
            r.fillRect(rect, (x + y) % 2 == 0 ? cfg::FLOOR : cfg::FLOOR_ALT);
            r.drawRect(rect, cfg::GRID, 1);
            const Tile nb[4] = {{x, y - 1}, {x, y + 1}, {x - 1, y}, {x + 1, y}};
            const Vec2 a[4] = {{rect.x, rect.y}, {rect.x, rect.bottom() - 1}, {rect.x + 1, rect.y}, {rect.right() - 1, rect.y}};
            const Vec2 b[4] = {{rect.right(), rect.y}, {rect.right(), rect.bottom() - 1}, {rect.x + 1, rect.bottom()}, {rect.right() - 1, rect.bottom()}};
            for (int i = 0; i < 4; ++i) {
                if (walls.count(nb[i])) r.line(a[i], b[i], wallGlow, 2);
                else if (voids.count(nb[i])) r.line(a[i], b[i], voidGlow, 3);
            }
        }
}

void Room::draw(Renderer& r, Vec2 cam, float t, const std::function<void()>& drawEntities) {
    drawFloor(r, cam);
    for (const Tile& m : revealedMines) {
        const Vec2 c = tileCenter(m) - cam;
        r.glow(c, 26, cfg::DANGER, 0.6f * (0.6f + 0.4f * std::sin(t * 6 + m.x)));
        r.line(c + Vec2{-10, -10}, c + Vec2{10, 10}, cfg::DANGER, 3);
        r.line(c + Vec2{10, -10}, c + Vec2{-10, 10}, cfg::DANGER, 3);
    }
    for (const auto& [gt, id] : gates)
        if (opened.count(id)) r.drawRect(tileRect(gt).moved(-cam), cfg::SUCCESS.scaled(0.35f), 1);
    if (doorOpen)
        for (const Tile& d : doors) {
            const RectF rect = tileRect(d).moved(-cam);
            const float pulse = 0.5f + 0.5f * std::sin(t * 5);
            r.glow(rect.center(), 60, cfg::SUCCESS, 0.6f + 0.4f * pulse);
            r.fillRect(rect, cfg::SUCCESS.scaled(0.3f));
            r.drawRect(rect, cfg::SUCCESS, 2);
            r.drawRect(rect.inflate(-14 - 8 * pulse, -14 - 8 * pulse), cfg::SUCCESS, 1);
        }
    for (auto& p : puzzles) p->drawFloor(r, cam, t);
    for (const auto& s : sentinels) s.drawCone(r, cam, *this);
    if (boss) boss->drawGround(r, cam, t);
    drawEntities();
    for (const auto& s : sentinels) s.draw(r, cam, t);

    std::vector<gfx::Block> blocks;
    const int x0 = static_cast<int>(std::floor(cam.x / TILE)) - 2, y0 = static_cast<int>(std::floor(cam.y / TILE)) - 2;
    for (int y = y0; y <= y0 + cfg::SCREEN_H / static_cast<int>(TILE) + 5; ++y)
        for (int x = x0; x <= x0 + cfg::SCREEN_W / static_cast<int>(TILE) + 5; ++x) {
            auto it = wallBlocks_.find({x, y});
            if (it != wallBlocks_.end()) blocks.push_back(it->second);
        }
    for (const auto& [gt, id] : gates)
        if (!opened.count(id)) {
            gfx::Block b;
            b.rect = tileRect(gt);
            b.height = cfg::GATE_HEIGHT;
            b.top = Color::rgb(40, 10, 22);
            b.side = Color::rgb(30, 8, 16);
            b.edge = cfg::DANGER;
            blocks.push_back(b);
        }
    if (!doorOpen)
        for (const Tile& d : doors) {
            gfx::Block b;
            b.rect = tileRect(d);
            b.height = cfg::WALL_HEIGHT;
            b.top = Color::rgb(44, 12, 24);
            b.side = Color::rgb(32, 8, 18);
            b.edge = cfg::DANGER;
            blocks.push_back(b);
        }
    if (terminal) {
        gfx::Block b;
        b.rect = tileRect(*terminal).inflate(-8, -8);
        b.height = cfg::DEVICE_HEIGHT;
        b.top = Color::rgb(8, 40, 48);
        b.side = Color::rgb(6, 28, 34);
        b.edge = cfg::TERMINAL;
        blocks.push_back(b);
    }
    for (const auto& p : puzzles) p->blocks(blocks);
    gfx::drawBlocks(r, blocks, cam);

    if (terminal) {
        const Vec2 top = gfx::project(tileCenter(*terminal) - cam, cfg::DEVICE_HEIGHT);
        r.glow(top, 30, cfg::TERMINAL, 0.6f + 0.3f * std::sin(t * 4));
        for (int i = 0; i < 3; ++i) {
            const float w = 6 + 8 * std::abs(std::sin(t * 3 + i));
            r.line({top.x - 9, top.y - 6 + i * 6}, {top.x - 9 + w, top.y - 6 + i * 6}, cfg::TERMINAL, 2);
        }
    }
    for (auto& p : puzzles) {
        p->drawOverlay(r, cam, t);
        if (!p->active && !p->solved) {
            const Vec2 c = p->anchor() - cam;
            r.text(L"ЗАШИФРОВАНО", c + Vec2{0, -34}, 13, cfg::DANGER, Align::Center, true);
            for (int i = 0; i < 3; ++i)
                r.fillRect(RectF::centered(c + Vec2{rng().uniform(-20, 20), rng().uniform(-18, 18)}, rng().uniform(16, 46), 2), cfg::DANGER);
        }
    }
    if (boss) boss->draw(r, cam, t);
    particles.draw(r, cam);
}
