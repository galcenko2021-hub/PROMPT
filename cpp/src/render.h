// Рендерер на Direct2D + DirectWrite: примитивы, неоновое свечение, текст, переворот мира.
// Логические координаты всегда 1280x720 — масштаб под экран и DPI делает Direct2D.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "core.h"

enum class Align { TopLeft, TopCenter, TopRight, MidLeft, Center, MidRight, BottomLeft, BottomCenter, BottomRight };

class Renderer {
public:
    bool init(HWND hwnd, float scale);
    bool initOffscreen(float scale);              // рисование в память (скриншоты, тесты)
    bool savePng(const std::wstring& path);
    bool begin();
    void end();

    void clear(Color c);
    void setFlip(bool flipX, bool flipY);
    void resetTransform();
    void setAdditive(bool on);
    void pushClip(const RectF& r);
    void popClip();

    void fillRect(const RectF& r, Color c);
    void drawRect(const RectF& r, Color c, float width = 1.f);
    void line(Vec2 a, Vec2 b, Color c, float width = 1.f);
    void fillCircle(Vec2 center, float radius, Color c);
    void drawCircle(Vec2 center, float radius, Color c, float width = 1.f);
    void fillPolygon(const std::vector<Vec2>& pts, Color c);
    void drawPolygon(const std::vector<Vec2>& pts, Color c, float width = 1.f);
    void polyline(const std::vector<Vec2>& pts, Color c, float width = 1.f);
    void glow(Vec2 center, float radius, Color c, float strength = 1.f);

    Vec2 measure(const std::wstring& text, float size, bool bold = false);
    RectF text(const std::wstring& text, Vec2 pos, float size, Color c, Align align = Align::TopLeft, bool bold = false);
    float textWrapped(const std::wstring& text, const RectF& box, float size, Color c, bool bold = false, Align align = Align::TopLeft);
    float wrappedHeight(const std::wstring& text, float width, float size, bool bold = false);

private:
    bool createTarget();
    void discardTarget();
    ID2D1SolidColorBrush* brush(Color c);
    IDWriteTextFormat* format(float size, bool bold, bool wrap);
    IDWriteTextLayout* layout(const std::wstring& text, float size, bool bold, float maxWidth);

    HWND hwnd_ = nullptr;
    float scale_ = 1.f;
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> rt_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    Microsoft::WRL::ComPtr<IWICBitmap> wicBitmap_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush>> glows_;
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<IDWriteTextFormat>> formats_;
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<IDWriteTextLayout>> layouts_;
    D2D1_MATRIX_3X2_F transform_ = D2D1::Matrix3x2F::Identity();
};
