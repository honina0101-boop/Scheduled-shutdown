#pragma once
#include "platform.hpp"
#include <cmath>
#include <d2d1.h>
#include <dwrite.h>
#include <functional>
namespace evening {
template <class T> void release(T *&value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}
inline D2D1_COLOR_F color(unsigned rgb, float alpha = 1) {
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, alpha);
}
inline D2D1_RECT_F rect(float x, float y, float w, float h) {
    return D2D1::RectF(x, y, x + w, y + h);
}
struct Theme {
    unsigned bg, card, field, text, muted, border, accent, good;
    static Theme dark() {
        return {0x101422, 0x1B2133, 0x242B40, 0xF2F3FF, 0x939CB9, 0x30384F, 0x9B90FF, 0x67DFBD};
    }
    static Theme light() {
        return {0xF5F6FC, 0xFFFFFF, 0xF0F2FA, 0x24283D, 0x7D849D, 0xE5E8F2, 0x6455D8, 0x179D80};
    }
};
struct Hit {
    D2D1_RECT_F bounds;
    int id;
};
struct Surface {
    HWND window = nullptr;
    ID2D1HwndRenderTarget *target = nullptr;
    std::vector<Hit> hits;
    int focus = 0, hover = 0;
    float scale = 1, scroll = 0, maxScroll = 0;
    void discard() { release(target); }
    ~Surface() { discard(); }
    float width() const {
        RECT r{};
        GetClientRect(window, &r);
        return (r.right - r.left) / scale;
    }
    float height() const {
        RECT r{};
        GetClientRect(window, &r);
        return (r.bottom - r.top) / scale;
    }
    int hit(float x, float y) const {
        for (auto it = hits.rbegin(); it != hits.rend(); ++it)
            if (x >= it->bounds.left && x <= it->bounds.right && y >= it->bounds.top &&
                y <= it->bounds.bottom)
                return it->id;
        return 0;
    }
};
class Painter {
  public:
    ID2D1RenderTarget *rt;
    ID2D1Factory *factory;
    IDWriteFactory *writer;
    Surface &surface;
    Theme theme;
    bool capture = false;
    D2D1_RECT_F clipBounds = rect(-10000, -10000, 20000, 20000);
    Painter(ID2D1RenderTarget *r, ID2D1Factory *f, IDWriteFactory *w, Surface &s, Theme t, bool c = false)
        : rt(r), factory(f), writer(w), surface(s), theme(t), capture(c) {}
    void fill(D2D1_RECT_F r, unsigned c, float radius = 0, float alpha = 1) {
        ID2D1SolidColorBrush *b = nullptr;
        if (FAILED(rt->CreateSolidColorBrush(color(c, alpha), &b)))
            return;
        if (radius > 0)
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b);
        else
            rt->FillRectangle(r, b);
        release(b);
    }
    void border(D2D1_RECT_F r, unsigned c, float radius = 0, float thickness = 1, float alpha = 1) {
        ID2D1SolidColorBrush *b = nullptr;
        rt->CreateSolidColorBrush(color(c, alpha), &b);
        if (b) {
            if (radius > 0)
                rt->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b, thickness);
            else
                rt->DrawRectangle(r, b, thickness);
            release(b);
        }
    }
    void line(float x, float y, float x2, float y2, unsigned c, float thickness = 1, float alpha = 1) {
        ID2D1SolidColorBrush *b = nullptr;
        rt->CreateSolidColorBrush(color(c, alpha), &b);
        if (b) {
            rt->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x2, y2), b, thickness);
            release(b);
        }
    }
    void circle(float x, float y, float radius, unsigned c, float alpha = 1) {
        ID2D1SolidColorBrush *b = nullptr;
        rt->CreateSolidColorBrush(color(c, alpha), &b);
        if (b) {
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius), b);
            release(b);
        }
    }
    void text(const std::wstring &value, D2D1_RECT_F r, float size, unsigned c, int weight = 400,
              DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING, bool number = false,
              float alpha = 1) {
        IDWriteTextFormat *f = nullptr;
        ID2D1SolidColorBrush *b = nullptr;
        writer->CreateTextFormat(number ? L"Segoe UI" : L"Microsoft YaHei UI", nullptr,
                                 static_cast<DWRITE_FONT_WEIGHT>(weight), DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &f);
        rt->CreateSolidColorBrush(color(c, alpha), &b);
        if (f && b) {
            f->SetTextAlignment(alignment);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            IDWriteInlineObject *ellipsis = nullptr;
            writer->CreateEllipsisTrimmingSign(f, &ellipsis);
            f->SetTrimming(&trim, ellipsis);
            rt->DrawText(value.c_str(), static_cast<UINT32>(value.size()), f, r, b,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
            release(ellipsis);
        }
        release(f);
        release(b);
    }
    void gradient(D2D1_RECT_F r, unsigned a, unsigned b, float radius = 16) {
        D2D1_GRADIENT_STOP stops[] = {{0, color(a)}, {1, color(b)}};
        ID2D1GradientStopCollection *collection = nullptr;
        ID2D1LinearGradientBrush *brush = nullptr;
        rt->CreateGradientStopCollection(stops, 2, &collection);
        if (collection)
            rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                                              D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom)),
                                          collection, &brush);
        if (brush)
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush);
        release(brush);
        release(collection);
    }
    void ring(float x, float y, float radius, float fraction, unsigned c, float thickness = 5,
              float alpha = 1) {
        ID2D1SolidColorBrush *b = nullptr;
        rt->CreateSolidColorBrush(color(c, alpha), &b);
        if (!b)
            return;
        if (fraction >= .9999f)
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius), b, thickness);
        else if (fraction > .001f) {
            constexpr float pi = 3.14159265359f;
            float angle = fraction * 2 * pi;
            ID2D1PathGeometry *geometry = nullptr;
            ID2D1GeometrySink *sink = nullptr;
            factory->CreatePathGeometry(&geometry);
            if (geometry)
                geometry->Open(&sink);
            if (sink) {
                sink->BeginFigure(D2D1::Point2F(x, y - radius), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(x + std::sin(angle) * radius, y - std::cos(angle) * radius),
                    D2D1::SizeF(radius, radius), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    angle > pi ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                rt->DrawGeometry(geometry, b, thickness);
            }
            release(sink);
            release(geometry);
        }
        release(b);
    }
    void hit(D2D1_RECT_F r, int id) {
        r.left = std::max(r.left, clipBounds.left);
        r.top = std::max(r.top, clipBounds.top);
        r.right = std::min(r.right, clipBounds.right);
        r.bottom = std::min(r.bottom, clipBounds.bottom);
        if (r.right > r.left && r.bottom > r.top)
            surface.hits.push_back({r, id});
    }
    void button(D2D1_RECT_F r, int id, const std::wstring &label, bool primary = false,
                bool selected = false) {
        bool hover = surface.hover == id;
        unsigned bg = primary ? 0x7161E8 : selected ? theme.accent : theme.field;
        fill(r, bg, 9, primary || selected ? 1 : hover ? .9f : 1);
        if (hover)
            border(r, primary ? 0xB9AEFF : theme.accent, 9, 1, 0.65f);
        text(label, r, 13, primary || selected ? 0xFFFFFF : theme.text, primary ? 600 : 400,
             DWRITE_TEXT_ALIGNMENT_CENTER);
        if (surface.focus == id)
            border(rect(r.left - 3, r.top - 3, r.right - r.left + 6, r.bottom - r.top + 6), theme.accent, 11,
                   2);
        hit(r, id);
    }
    void toggle(D2D1_RECT_F r, int id, bool on) {
        fill(r, on ? 0x7663E9 : theme.border, 11);
        circle(on ? r.right - 11 : r.left + 11, (r.top + r.bottom) / 2, 8, 0xFFFFFF);
        if (surface.focus == id)
            border(rect(r.left - 3, r.top - 3, r.right - r.left + 6, r.bottom - r.top + 6), theme.accent, 13,
                   2);
        hit(r, id);
    }
    void clip(D2D1_RECT_F r) {
        clipBounds = r;
        rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }
    void unclip() {
        rt->PopAxisAlignedClip();
        clipBounds = rect(-10000, -10000, 20000, 20000);
    }
};
} // namespace evening
