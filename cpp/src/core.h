// Базовые типы: векторы, клетки, цвета, прямоугольники, генератор случайных чисел.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

constexpr float PI = 3.14159265358979f;
constexpr float TAU = 6.28318530717958f;
constexpr float TILE = 48.0f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// --------------------------------------------------------------------------
// Вектор
// --------------------------------------------------------------------------
struct Vec2 {
    float x = 0, y = 0;
    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float k) const { return {x * k, y * k}; }
    Vec2 operator/(float k) const { return {x / k, y / k}; }
    Vec2 operator-() const { return {-x, -y}; }
    Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(float k) { x *= k; y *= k; return *this; }
    float length() const { return std::sqrt(x * x + y * y); }
    float lengthSq() const { return x * x + y * y; }
    float dot(Vec2 o) const { return x * o.x + y * o.y; }
    float distance(Vec2 o) const { return (*this - o).length(); }
    Vec2 normalized() const {
        float l = length();
        return l > 1e-6f ? Vec2{x / l, y / l} : Vec2{};
    }
    Vec2 lerp(Vec2 o, float t) const { return {lerpf(x, o.x, t), lerpf(y, o.y, t)}; }
    static Vec2 polar(float angle, float radius = 1.f) { return {std::cos(angle) * radius, std::sin(angle) * radius}; }
};

// --------------------------------------------------------------------------
// Клетка сетки
// --------------------------------------------------------------------------
struct Tile {
    int x = 0, y = 0;
    bool operator==(const Tile& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Tile& o) const { return !(*this == o); }
    bool operator<(const Tile& o) const { return y != o.y ? y < o.y : x < o.x; }
    Tile operator+(Tile o) const { return {x + o.x, y + o.y}; }
    Tile operator-(Tile o) const { return {x - o.x, y - o.y}; }
};

struct TileHash {
    size_t operator()(const Tile& t) const { return std::hash<long long>()((static_cast<long long>(t.x) << 32) ^ static_cast<unsigned>(t.y)); }
};
using TileSet = std::unordered_set<Tile, TileHash>;
template <class V>
using TileMap = std::unordered_map<Tile, V, TileHash>;

constexpr Tile DIRS4[4] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

inline Vec2 tileCenter(Tile t) { return {t.x * TILE + TILE / 2, t.y * TILE + TILE / 2}; }
inline Tile tileOf(Vec2 p) { return {static_cast<int>(std::floor(p.x / TILE)), static_cast<int>(std::floor(p.y / TILE))}; }
inline float tileDist(Tile a, Tile b) { return std::sqrt(float((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y))); }

// --------------------------------------------------------------------------
// Прямоугольник
// --------------------------------------------------------------------------
struct RectF {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    Vec2 center() const { return {x + w / 2, y + h / 2}; }
    bool contains(Vec2 p) const { return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h; }
    RectF inflate(float dx, float dy) const { return {x - dx / 2, y - dy / 2, w + dx, h + dy}; }
    RectF moved(Vec2 d) const { return {x + d.x, y + d.y, w, h}; }
    static RectF centered(Vec2 c, float w, float h) { return {c.x - w / 2, c.y - h / 2, w, h}; }
};

inline RectF tileRect(Tile t) { return {t.x * TILE, t.y * TILE, TILE, TILE}; }

// --------------------------------------------------------------------------
// Цвет
// --------------------------------------------------------------------------
struct Color {
    float r = 1, g = 1, b = 1, a = 1;
    static constexpr Color rgb(int r_, int g_, int b_, float a_ = 1.f) { return {r_ / 255.f, g_ / 255.f, b_ / 255.f, a_}; }
    static constexpr Color hex(uint32_t v) { return rgb((v >> 16) & 255, (v >> 8) & 255, v & 255); }
    Color scaled(float k) const { return {clampf(r * k, 0, 1), clampf(g * k, 0, 1), clampf(b * k, 0, 1), a}; }
    Color alpha(float a_) const { return {r, g, b, clampf(a_, 0, 1)}; }
    static Color mix(Color p, Color q, float t) {
        t = clampf(t, 0, 1);
        return {lerpf(p.r, q.r, t), lerpf(p.g, q.g, t), lerpf(p.b, q.b, t), lerpf(p.a, q.a, t)};
    }
    static Color hsv(float h, float s = 1.f, float v = 1.f) {
        h = h - std::floor(h);
        float i = std::floor(h * 6.f), f = h * 6.f - i;
        float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
        switch (static_cast<int>(i) % 6) {
            case 0: return {v, t, p, 1};
            case 1: return {q, v, p, 1};
            case 2: return {p, v, t, 1};
            case 3: return {p, q, v, 1};
            case 4: return {t, p, v, 1};
            default: return {v, p, q, 1};
        }
    }
};

// --------------------------------------------------------------------------
// Случайные числа
// --------------------------------------------------------------------------
class Rng {
public:
    explicit Rng(uint32_t seed = std::random_device{}()) : gen_(seed) {}
    float uniform(float a = 0.f, float b = 1.f) { return std::uniform_real_distribution<float>(a, b)(gen_); }
    int range(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(gen_); }  // включительно
    bool chance(float p) { return uniform() < p; }
    float normal(float mean = 0.f, float sd = 1.f) { return std::normal_distribution<float>(mean, sd)(gen_); }
    template <class T>
    void shuffle(std::vector<T>& v) { std::shuffle(v.begin(), v.end(), gen_); }
    template <class T>
    const T& pick(const std::vector<T>& v) { return v[static_cast<size_t>(range(0, static_cast<int>(v.size()) - 1))]; }
    std::mt19937& engine() { return gen_; }

private:
    std::mt19937 gen_;
};

Rng& rng();  // общий генератор игры (определён в core.cpp)

// --------------------------------------------------------------------------
// Строки
// --------------------------------------------------------------------------
std::string toUtf8(const std::wstring& w);
std::wstring fromUtf8(const std::string& s);
std::wstring formatPercent(float p);
std::wstring formatFloat(float v, int digits = 1);
