// Неоновые фигуры, фигуры Помощников, псевдо-3D блоки и частицы.
#pragma once

#include <vector>

#include "core.h"
#include "render.h"

namespace gfx {

std::vector<Vec2> regularPolygon(Vec2 center, float radius, int sides, float rotation = 0.f);
void neonPolygon(Renderer& r, Color c, const std::vector<Vec2>& pts, float width = 2.f, float fillK = 0.22f);
void neonCircle(Renderer& r, Color c, Vec2 center, float radius, float width = 2.f, float fillK = 0.22f);
void dashedLine(Renderer& r, Color c, Vec2 a, Vec2 b, float dash = 10.f, float gap = 7.f, float offset = 0.f, float width = 2.f);
void starLines(Renderer& r, Color c, Vec2 center, float radius, float rotation, float width = 2.f);
void assistantIcon(Renderer& r, int level, Vec2 center, float size, Color c, float rotation = 0.f, float width = 2.f);
void panel(Renderer& r, const RectF& rect, Color border, float alpha = 0.8f, float width = 2.f);

// Выдавленный вверх прямоугольник (стена, ящик, консоль) в мировых координатах.
struct Block {
    RectF rect;
    float height;
    Color top, side, edge;
    bool exposed[4] = {true, true, true, true};  // N, E, S, W
    float edgeWidth = 2.f;
};

Vec2 project(Vec2 screen, float height);
void drawBlocks(Renderer& r, std::vector<Block>& blocks, Vec2 cam);

struct Particle {
    Vec2 pos, vel;
    float life, maxLife;
    Color color;
    float size;
};

class Particles {
public:
    void burst(Vec2 pos, Color c, int count = 18, float speed = 160.f, float life = 0.7f, float size = 3.f);
    void stream(Vec2 pos, Vec2 vel, Color c, float life = 0.6f, float size = 2.f);
    void update(float dt);
    void draw(Renderer& r, Vec2 cam) const;
    void clear() { items_.clear(); }

private:
    std::vector<Particle> items_;
};

}  // namespace gfx
