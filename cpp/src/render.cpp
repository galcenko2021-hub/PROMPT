#include "render.h"

#include "settings.h"

using Microsoft::WRL::ComPtr;

namespace {

D2D1_COLOR_F toD2D(Color c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }
D2D1_POINT_2F pt(Vec2 v) { return D2D1::Point2F(v.x, v.y); }
D2D1_RECT_F rc(const RectF& r) { return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h); }

}  // namespace

bool Renderer::init(HWND hwnd, float scale) {
    hwnd_ = hwnd;
    scale_ = scale;
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 reinterpret_cast<void**>(factory_.GetAddressOf()))))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    return createTarget();
}

bool Renderer::createTarget() {
    if (wicBitmap_) {
        const float dpi = 96.f * scale_;
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
        if (FAILED(factory_->CreateWicBitmapRenderTarget(wicBitmap_.Get(), props, rt_.GetAddressOf()))) return false;
        rt_.As(&dc_);
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), brush_.GetAddressOf());
        return true;
    }
    RECT client;
    GetClientRect(hwnd_, &client);
    const float dpi = 96.f * scale_;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    auto hwndProps = D2D1::HwndRenderTargetProperties(
        hwnd_, D2D1::SizeU(static_cast<UINT32>(client.right - client.left), static_cast<UINT32>(client.bottom - client.top)));
    ComPtr<ID2D1HwndRenderTarget> hwndTarget;
    if (FAILED(factory_->CreateHwndRenderTarget(props, hwndProps, hwndTarget.GetAddressOf()))) return false;
    hwndTarget.As(&rt_);
    rt_.As(&dc_);  // Direct2D 1.1: нужен для аддитивного смешивания (Windows 8+)
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), brush_.GetAddressOf());
    return true;
}

void Renderer::discardTarget() {
    glows_.clear();
    brush_.Reset();
    dc_.Reset();
    rt_.Reset();
}

bool Renderer::initOffscreen(float scale) {
    scale_ = scale;
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 reinterpret_cast<void**>(factory_.GetAddressOf()))))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.GetAddressOf())))) return false;
    if (FAILED(wic_->CreateBitmap(static_cast<UINT>(cfg::SCREEN_W * scale), static_cast<UINT>(cfg::SCREEN_H * scale),
                                  GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, wicBitmap_.GetAddressOf())))
        return false;
    return createTarget();
}

bool Renderer::savePng(const std::wstring& path) {
    if (!wic_ || !wicBitmap_) return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic_->CreateStream(stream.GetAddressOf())) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    if (FAILED(wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()))) return false;
    encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    encoder->CreateNewFrame(frame.GetAddressOf(), nullptr);
    frame->Initialize(nullptr);
    frame->WriteSource(wicBitmap_.Get(), nullptr);
    frame->Commit();
    return SUCCEEDED(encoder->Commit());
}

bool Renderer::begin() {
    if (!rt_ && !createTarget()) return false;
    rt_->BeginDraw();
    resetTransform();
    return true;
}

void Renderer::end() {
    if (!rt_) return;
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) discardTarget();
}

void Renderer::clear(Color c) { rt_->Clear(toD2D(c)); }

void Renderer::setFlip(bool flipX, bool flipY) {
    transform_ = D2D1::Matrix3x2F::Scale(flipX ? -1.f : 1.f, flipY ? -1.f : 1.f, pt(cfg::SCREEN_CENTER));
    rt_->SetTransform(transform_);
}

void Renderer::resetTransform() {
    transform_ = D2D1::Matrix3x2F::Identity();
    rt_->SetTransform(transform_);
}

void Renderer::setAdditive(bool on) {
    if (dc_) dc_->SetPrimitiveBlend(on ? D2D1_PRIMITIVE_BLEND_ADD : D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
}

void Renderer::pushClip(const RectF& r) { rt_->PushAxisAlignedClip(rc(r), D2D1_ANTIALIAS_MODE_ALIASED); }
void Renderer::popClip() { rt_->PopAxisAlignedClip(); }

ID2D1SolidColorBrush* Renderer::brush(Color c) {
    brush_->SetColor(toD2D(c));
    return brush_.Get();
}

// --------------------------------------------------------------------------
// Примитивы
// --------------------------------------------------------------------------
void Renderer::fillRect(const RectF& r, Color c) { rt_->FillRectangle(rc(r), brush(c)); }

void Renderer::drawRect(const RectF& r, Color c, float width) {
    const float h = width / 2;
    rt_->DrawRectangle(D2D1::RectF(r.x + h, r.y + h, r.x + r.w - h, r.y + r.h - h), brush(c), width);
}

void Renderer::line(Vec2 a, Vec2 b, Color c, float width) { rt_->DrawLine(pt(a), pt(b), brush(c), width); }

void Renderer::fillCircle(Vec2 center, float radius, Color c) {
    rt_->FillEllipse(D2D1::Ellipse(pt(center), radius, radius), brush(c));
}

void Renderer::drawCircle(Vec2 center, float radius, Color c, float width) {
    rt_->DrawEllipse(D2D1::Ellipse(pt(center), radius, radius), brush(c), width);
}

void Renderer::fillPolygon(const std::vector<Vec2>& pts, Color c) {
    if (pts.size() < 3) return;
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(factory_->CreatePathGeometry(geo.GetAddressOf()))) return;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(sink.GetAddressOf());
    sink->BeginFigure(pt(pts[0]), D2D1_FIGURE_BEGIN_FILLED);
    for (size_t i = 1; i < pts.size(); ++i) sink->AddLine(pt(pts[i]));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    rt_->FillGeometry(geo.Get(), brush(c));
}

void Renderer::drawPolygon(const std::vector<Vec2>& pts, Color c, float width) {
    if (pts.size() < 2) return;
    for (size_t i = 0; i < pts.size(); ++i) line(pts[i], pts[(i + 1) % pts.size()], c, width);
}

void Renderer::polyline(const std::vector<Vec2>& pts, Color c, float width) {
    for (size_t i = 1; i < pts.size(); ++i) line(pts[i - 1], pts[i], c, width);
}

void Renderer::glow(Vec2 center, float radius, Color c, float strength) {
    if (strength <= 0.f || radius < 2.f) return;
    const uint32_t key = (static_cast<uint32_t>(c.r * 31) << 10) | (static_cast<uint32_t>(c.g * 31) << 5) | static_cast<uint32_t>(c.b * 31);
    auto& gb = glows_[key];
    if (!gb) {
        const Color q{(key >> 10 & 31) / 31.f, (key >> 5 & 31) / 31.f, (key & 31) / 31.f, 1.f};
        D2D1_GRADIENT_STOP stops[3] = {
            {0.f, D2D1::ColorF(q.r, q.g, q.b, 0.55f)},
            {0.45f, D2D1::ColorF(q.r, q.g, q.b, 0.16f)},
            {1.f, D2D1::ColorF(q.r, q.g, q.b, 0.f)},
        };
        ComPtr<ID2D1GradientStopCollection> collection;
        rt_->CreateGradientStopCollection(stops, 3, collection.GetAddressOf());
        rt_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 0), 1, 1),
                                       collection.Get(), gb.GetAddressOf());
    }
    gb->SetCenter(pt(center));
    gb->SetRadiusX(radius);
    gb->SetRadiusY(radius);
    gb->SetOpacity(clampf(strength, 0.f, 2.f) * 0.8f);
    setAdditive(true);
    rt_->FillEllipse(D2D1::Ellipse(pt(center), radius, radius), gb.Get());
    setAdditive(false);
}

// --------------------------------------------------------------------------
// Текст
// --------------------------------------------------------------------------
IDWriteTextFormat* Renderer::format(float size, bool bold, bool wrap) {
    const uint32_t key = (static_cast<uint32_t>(size * 4) << 2) | (bold ? 1u : 0u) | (wrap ? 2u : 0u);
    auto& f = formats_[key];
    if (!f) {
        dwrite_->CreateTextFormat(L"Consolas", nullptr, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ru-ru", f.GetAddressOf());
        if (f) f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return f.Get();
}

IDWriteTextLayout* Renderer::layout(const std::wstring& text, float size, bool bold, float maxWidth) {
    std::wstring key = text;
    key += L'\x01';
    key += std::to_wstring(static_cast<int>(size * 4)) + (bold ? L"b" : L"n") + std::to_wstring(static_cast<int>(maxWidth));
    auto it = layouts_.find(key);
    if (it != layouts_.end()) return it->second.Get();
    if (layouts_.size() > 900) layouts_.clear();
    ComPtr<IDWriteTextLayout> lay;
    const bool wrap = maxWidth < 9000.f;
    dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format(size, bold, wrap), maxWidth, 10000.f,
                              lay.GetAddressOf());
    auto* raw = lay.Get();
    layouts_[key] = lay;
    return raw;
}

Vec2 Renderer::measure(const std::wstring& text, float size, bool bold) {
    auto* lay = layout(text, size, bold, 10000.f);
    if (!lay) return {};
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    return {m.widthIncludingTrailingWhitespace, m.height};
}

RectF Renderer::text(const std::wstring& str, Vec2 pos, float size, Color c, Align align, bool bold) {
    if (str.empty()) return {pos.x, pos.y, 0, 0};
    auto* lay = layout(str, size, bold, 10000.f);
    if (!lay) return {};
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    const float w = m.widthIncludingTrailingWhitespace, h = m.height;
    float x = pos.x, y = pos.y;
    switch (align) {
        case Align::TopCenter: x -= w / 2; break;
        case Align::TopRight: x -= w; break;
        case Align::MidLeft: y -= h / 2; break;
        case Align::Center: x -= w / 2; y -= h / 2; break;
        case Align::MidRight: x -= w; y -= h / 2; break;
        case Align::BottomLeft: y -= h; break;
        case Align::BottomCenter: x -= w / 2; y -= h; break;
        case Align::BottomRight: x -= w; y -= h; break;
        default: break;
    }
    rt_->DrawTextLayout(D2D1::Point2F(x, y), lay, brush(c), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    return {x, y, w, h};
}

float Renderer::wrappedHeight(const std::wstring& str, float width, float size, bool bold) {
    if (str.empty()) return 0.f;
    auto* lay = layout(str, size, bold, width);
    if (!lay) return 0.f;
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    return m.height;
}

float Renderer::textWrapped(const std::wstring& str, const RectF& box, float size, Color c, bool bold, Align align) {
    if (str.empty()) return 0.f;
    auto* lay = layout(str, size, bold, box.w);
    if (!lay) return 0.f;
    lay->SetTextAlignment(align == Align::TopCenter || align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                                               : DWRITE_TEXT_ALIGNMENT_LEADING);
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    rt_->DrawTextLayout(D2D1::Point2F(box.x, box.y), lay, brush(c));
    return m.height;
}
