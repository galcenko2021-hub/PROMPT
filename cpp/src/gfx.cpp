#include "gfx.h"

#include <algorithm>
#include <set>

#include "settings.h"

namespace gfx {

std::vector<Vec2> regularPolygon(Vec2 center, float radius, int sides, float rotation) {
    std::vector<Vec2> pts;
    pts.reserve(static_cast<size_t>(sides));
    for (int i = 0; i < sides; ++i) pts.push_back(center + Vec2::polar(rotation + i * TAU / sides, radius));
    return pts;
}

void neonPolygon(Renderer& r, Color c, const std::vector<Vec2>& pts, float width, float fillK) {
    if (pts.size() < 3) return;
    if (fillK > 0.f) r.fillPolygon(pts, c.scaled(fillK));
    r.drawPolygon(pts, c, width);
}

void neonCircle(Renderer& r, Color c, Vec2 center, float radius, float width, float fillK) {
    if (fillK > 0.f) r.fillCircle(center, radius, c.scaled(fillK));
    r.drawCircle(center, radius, c, width);
}

void dashedLine(Renderer& r, Color c, Vec2 a, Vec2 b, float dash, float gap, float offset, float width) {
    const Vec2 d = b - a;
    const float len = d.length();
    if (len < 1.f) return;
    const Vec2 dir = d / len;
    const float period = dash + gap;
    float pos = -std::fmod(std::fmod(offset, period) + period, period);
    while (pos < len) {
        const float s = std::max(pos, 0.f), e = std::min(pos + dash, len);
        if (e > s) r.line(a + dir * s, a + dir * e, c, width);
        pos += period;
    }
}

void starLines(Renderer& r, Color c, Vec2 center, float radius, float rotation, float width) {
    auto pts = regularPolygon(center, radius, 5, rotation - PI / 2);
    for (int i = 0; i < 5; ++i) r.line(pts[static_cast<size_t>(i)], pts[static_cast<size_t>((i + 2) % 5)], c, width);
}

void assistantIcon(Renderer& r, int level, Vec2 center, float size, Color c, float rotation, float width) {
    switch (level) {
        case 1: {
            const float h = size * 0.72f;
            const RectF rect = RectF::centered(center, h * 2, h * 2);
            r.fillRect(rect, c.scaled(0.25f));
            r.drawRect(rect, c, width);
            r.fillRect(RectF::centered(center, h, h), c);
            break;
        }
        case 2:
            neonCircle(r, c, center, size, width);
            r.fillCircle(center, std::max(2.f, size * 0.35f), c);
            break;
        case 3:
            neonPolygon(r, c, regularPolygon(center, size * 1.15f, 3, rotation), width);
            r.fillCircle(center, std::max(2.f, size * 0.22f), c);
            break;
        case 4:
            neonPolygon(r, c, regularPolygon(center, size * 1.05f, 6, rotation), width);
            r.drawPolygon(regularPolygon(center, size * 0.5f, 6, -rotation * 1.6f), c, std::max(1.f, width - 1));
            break;
        default:
            starLines(r, c, center, size * 1.2f, rotation, width);
            r.fillCircle(center, std::max(2.f, size * 0.18f), c);
            break;
    }
}

void panel(Renderer& r, const RectF& rect, Color border, float alpha, float width) {
    r.fillRect(rect, Color::rgb(8, 8, 18, alpha));
    r.drawRect(rect, border, width);
    const Color bright = border.scaled(1.4f);
    const float k = 8.f;
    r.line({rect.x, rect.y}, {rect.x + k, rect.y}, bright, 2);
    r.line({rect.x, rect.y}, {rect.x, rect.y + k}, bright, 2);
    r.line({rect.right(), rect.y}, {rect.right() - k, rect.y}, bright, 2);
    r.line({rect.right(), rect.y}, {rect.right(), rect.y + k}, bright, 2);
    r.line({rect.x, rect.bottom()}, {rect.x + k, rect.bottom()}, bright, 2);
    r.line({rect.x, rect.bottom()}, {rect.x, rect.bottom() - k}, bright, 2);
    r.line({rect.right(), rect.bottom()}, {rect.right() - k, rect.bottom()}, bright, 2);
    r.line({rect.right(), rect.bottom()}, {rect.right(), rect.bottom() - k}, bright, 2);
}

// --------------------------------------------------------------------------
// Псевдо-3D блоки
// --------------------------------------------------------------------------
Vec2 project(Vec2 s, float height) {
    const Vec2 c = cfg::SCREEN_CENTER;
    return c + (s - c) * (1.f + height);
}

void drawBlocks(Renderer& r, std::vector<Block>& blocks, Vec2 cam) {
    const Vec2 c = cfg::SCREEN_CENTER;
    std::stable_sort(blocks.begin(), blocks.end(), [](const Block& a, const Block& b) { return a.height < b.height; });
    size_t start = 0;
    while (start < blocks.size()) {
        size_t end = start;
        while (end < blocks.size() && blocks[end].height == blocks[start].height) ++end;
        // Боковые грани видны только с той стороны, что обращена к центру экрана.
        for (size_t i = start; i < end; ++i) {
            const Block& b = blocks[i];
            const float x0 = b.rect.x - cam.x, y0 = b.rect.y - cam.y, x1 = x0 + b.rect.w, y1 = y0 + b.rect.h;
            const float k = b.height;
            const Color light = b.side, dark = b.side.scaled(0.7f);
            if (b.exposed[0] && y0 > c.y) r.fillPolygon({{x0, y0}, {x1, y0}, project({x1, y0}, k), project({x0, y0}, k)}, light);
            if (b.exposed[2] && y1 < c.y) r.fillPolygon({{x0, y1}, {x1, y1}, project({x1, y1}, k), project({x0, y1}, k)}, light);
            if (b.exposed[3] && x0 > c.x) r.fillPolygon({{x0, y0}, {x0, y1}, project({x0, y1}, k), project({x0, y0}, k)}, dark);
            if (b.exposed[1] && x1 < c.x) r.fillPolygon({{x1, y0}, {x1, y1}, project({x1, y1}, k), project({x1, y0}, k)}, dark);
        }
        for (size_t i = start; i < end; ++i) {
            const Block& b = blocks[i];
            const Vec2 tl = project({b.rect.x - cam.x, b.rect.y - cam.y}, b.height);
            const RectF top{tl.x, tl.y, b.rect.w * (1.f + b.height) + 0.5f, b.rect.h * (1.f + b.height) + 0.5f};
            r.fillRect(top, b.top);
            const float ew = b.edgeWidth;
            if (b.exposed[0]) r.line({top.x, top.y}, {top.right(), top.y}, b.edge, ew);
            if (b.exposed[2]) r.line({top.x, top.bottom()}, {top.right(), top.bottom()}, b.edge, ew);
            if (b.exposed[3]) r.line({top.x, top.y}, {top.x, top.bottom()}, b.edge, ew);
            if (b.exposed[1]) r.line({top.right(), top.y}, {top.right(), top.bottom()}, b.edge, ew);
        }
        start = end;
    }
}

// --------------------------------------------------------------------------
// Частицы
// --------------------------------------------------------------------------
void Particles::burst(Vec2 pos, Color c, int count, float speed, float life, float size) {
    Rng& g = rng();
    for (int i = 0; i < count; ++i) {
        const float angle = g.uniform(0, TAU);
        const float l = g.uniform(life * 0.5f, life);
        items_.push_back({pos, Vec2::polar(angle, g.uniform(speed * 0.3f, speed)), l, l, c, g.uniform(size * 0.6f, size * 1.3f)});
    }
}

void Particles::stream(Vec2 pos, Vec2 vel, Color c, float life, float size) { items_.push_back({pos, vel, life, life, c, size}); }

void Particles::update(float dt) {
    for (auto& p : items_) {
        p.life -= dt;
        p.pos += p.vel * dt;
        p.vel *= 0.93f;
    }
    items_.erase(std::remove_if(items_.begin(), items_.end(), [](const Particle& p) { return p.life <= 0; }), items_.end());
}

void Particles::draw(Renderer& r, Vec2 cam) const {
    r.setAdditive(true);
    for (const auto& p : items_) {
        const float k = p.life / p.maxLife;
        const float s = std::max(1.f, p.size * (0.5f + k * 0.5f));
        r.fillRect(RectF::centered(p.pos - cam, s, s), p.color.scaled(k));
    }
    r.setAdditive(false);
}

}  // namespace gfx
