// Комната-уровень: карта, стены (псевдо-3D), двери, терминал, стражи, поиск пути,
// перестановка объектов после смерти Оператора.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "gfx.h"
#include "levels.h"
#include "puzzle.h"

class Assistant;
class GameAPI;
class Vibecoder;

class Sentinel {
public:
    explicit Sentinel(std::vector<Vec2> waypoints);
    Vec2 pos;
    float angle = 0.f, alert = 0.f;

    bool update(float dt, const class Room& room, Vec2 playerPos, bool playerHidden);
    bool canSee(Vec2 target, const Room& room) const;
    void randomize(Rng& g);
    void drawCone(Renderer& r, Vec2 cam, const Room& room) const;
    void draw(Renderer& r, Vec2 cam, float t) const;

private:
    std::vector<Vec2> waypoints_;
    size_t index_ = 0;
    float baseAngle_ = 0.f, wait_ = 0.f, scan_ = 0.f;
};

class Room {
public:
    explicit Room(const LevelDef& def);
    ~Room();

    const LevelDef& def;
    int width = 0, height = 0;
    float time = 0.f, shake = 0.f;
    int reshuffles = 0;
    std::function<void(const std::wstring&, const std::wstring&, Color)> notify;
    std::function<void(Puzzle&)> onSolvedHook;
    gfx::Particles particles;

    TileSet walls, voids, glass, field, mines, revealedMines;
    std::vector<Tile> doors;
    bool doorOpen = false;
    TileMap<std::string> gates;
    std::set<std::string> opened;
    std::optional<Tile> terminal;
    Vec2 spawn;
    LinkObjects* linkSource = nullptr;
    std::vector<std::unique_ptr<Puzzle>> puzzles;
    std::vector<Sentinel> sentinels;
    std::unique_ptr<Vibecoder> boss;

    // --- клетки ---
    bool inBounds(Tile t) const { return t.x >= 0 && t.y >= 0 && t.x < width && t.y < height; }
    bool isWall(Tile t) const { return !inBounds(t) || walls.count(t) || voids.count(t); }
    bool gateClosed(Tile t) const;
    bool doorClosed(Tile t) const;
    bool blocksPlayer(Tile t) const;
    bool blocksSight(Tile t) const;
    TileSet boxTiles() const;
    bool assistantCanWalk(Tile t, bool allowField = false, bool ignoreBoxes = false) const;
    bool isFloorFree(Tile t) const;  // обычный пол без объектов (для перестановки)
    std::optional<Tile> freeTileNear(Tile t) const;

    std::vector<Tile> findPath(Tile start, const TileSet& goals, const std::function<bool(Tile)>& passable) const;
    std::vector<Tile> findPathNearest(Tile start, Tile target, const std::function<bool(Tile)>& passable) const;
    bool lineOfSight(Vec2 a, Vec2 b) const;
    Vec2 castRay(Vec2 origin, float angle, float maxDistance) const;

    // --- логика ---
    int solvedCount() const;
    bool allSolved() const;
    bool recording() const;
    std::wstring veto(const Assistant& a, const Puzzle& p) const;
    void revealMine(Tile t);
    void onPlayerCaught();
    std::vector<Interactable> interactables(GameAPI& g);
    std::vector<int> contextObjects() const;
    bool playerOnExit(Vec2 pos) const;
    void update(float dt, GameAPI& g);
    void reshuffle(Rng& g, Vec2 playerPos, Vec2 assistantPos);

    void draw(Renderer& r, Vec2 cam, float t, const std::function<void()>& drawEntities);

private:
    void refreshSolids();
    void buildWallBlocks();
    void drawFloor(Renderer& r, Vec2 cam) const;
    void onSolved(Puzzle& p);

    TileSet baseWalls_, staticSolid_;
    TileMap<gfx::Block> wallBlocks_;
};
