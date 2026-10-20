#include "display/PauseOverlay.h"

#include <windows.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace od {

namespace {

const wchar_t* ReasonEmoji(PauseReason reason)
{
    switch (reason) {
        case PauseReason::Locked:
            return L"\U0001F512";
        case PauseReason::Asleep:
            return L"\U0001F319";
        case PauseReason::Admin:
            return L"\U0001F6E1\uFE0F";
        default:
            return L"\u23F8\uFE0F";
    }
}

const wchar_t* ReasonTitle(PauseReason reason)
{
    switch (reason) {
        case PauseReason::Locked:
            return L"Locked";
        case PauseReason::Asleep:
            return L"Display asleep";
        case PauseReason::Admin:
            return L"Waiting for admin approval";
        default:
            return L"Paused";
    }
}

// Ink box of a single-line layout, relative to its origin.
D2D1_RECT_F InkBox(IDWriteTextLayout* layout)
{
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    layout->SetMaxWidth(m.width);
    layout->SetMaxHeight(m.height);
    DWRITE_OVERHANG_METRICS o{};
    layout->GetOverhangMetrics(&o);
    return D2D1::RectF(m.left - o.left, m.top - o.top, m.left + m.width + o.right, m.top + m.height + o.bottom);
}

// COM for WIC on whatever thread calls in; balanced only when we initialised it.
struct ComScope {
    bool owned = false;
    ComScope()
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned = SUCCEEDED(hr);
    }
    ~ComScope()
    {
        if (owned)
            CoUninitialize();
    }
};

bool DrawOverlay(IWICImagingFactory* wic, IWICBitmap* bitmap, uint32_t width, uint32_t height, PauseReason reason)
{
    ComPtr<ID2D1Factory1> d2d;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                 reinterpret_cast<void**>(d2d.GetAddressOf()))))
        return false;
    ComPtr<IDWriteFactory> dwrite;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()))))
        return false;
    (void)wic;

    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(d2d->CreateWicBitmapRenderTarget(bitmap, props, &rt)))
        return false;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const float base = (std::min)(w, h);
    const float titleSize = (std::max)(14.0f, std::round(base * 0.042f));
    const float subSize = (std::max)(10.0f, std::round(titleSize * 0.5f));

    ComPtr<IDWriteTextFormat> titleFmt, subFmt;
    if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, titleSize, L"en-us", &titleFmt)) ||
        FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, subSize, L"en-us", &subFmt)))
        return false;
    titleFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    subFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    const wchar_t* title = ReasonTitle(reason);
    const wchar_t* emoji = ReasonEmoji(reason);
    const wchar_t* sub = reason == PauseReason::Locked   ? L"Unlock to continue"
                         : reason == PauseReason::Asleep ? L"Wake display to continue"
                         : reason == PauseReason::Admin  ? L"Approve to continue"
                                                         : L"Resumes automatically";
    ComPtr<IDWriteTextLayout> titleLayout, subLayout;
    if (FAILED(dwrite->CreateTextLayout(title, static_cast<UINT32>(wcslen(title)), titleFmt.Get(), w, h, &titleLayout)) ||
        FAILED(dwrite->CreateTextLayout(sub, static_cast<UINT32>(wcslen(sub)), subFmt.Get(), w, h, &subLayout)))
        return false;
    DWRITE_TEXT_METRICS tm{}, sm{};
    titleLayout->GetMetrics(&tm);
    subLayout->GetMetrics(&sm);

    // Toast layout: emoji on the left, title over subtitle beside it, both
    // left-aligned. The emoji's ink is ~85% of the two-line block's height
    // and centered on it; the whole group is centered with equal padding.
    const float padX = titleSize * 1.5f;
    const float padY = titleSize * 0.95f;
    const float gap = titleSize * 0.35f;      // title -> subtitle
    const float emojiGap = titleSize * 0.6f;  // emoji -> text
    const float textW = (std::max)(tm.width, sm.width);
    const float blockH = tm.height + gap + sm.height;

    // Size the emoji font from its ink at a reference size.
    constexpr float kRefSize = 100.0f;
    ComPtr<IDWriteTextFormat> emojiRefFmt;
    ComPtr<IDWriteTextLayout> emojiLayout;
    if (FAILED(dwrite->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, kRefSize, L"en-us", &emojiRefFmt)) ||
        FAILED(dwrite->CreateTextLayout(emoji, static_cast<UINT32>(wcslen(emoji)), emojiRefFmt.Get(), w, h,
                                        &emojiLayout)))
        return false;
    const D2D1_RECT_F refInk = InkBox(emojiLayout.Get());
    const float refInkH = (std::max)(1.0f, refInk.bottom - refInk.top);
    const float emojiSize = kRefSize * (blockH * 0.85f) / refInkH;
    ComPtr<IDWriteTextFormat> emojiFmt;
    emojiLayout.Reset();
    if (FAILED(dwrite->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, emojiSize, L"en-us", &emojiFmt)) ||
        FAILED(dwrite->CreateTextLayout(emoji, static_cast<UINT32>(wcslen(emoji)), emojiFmt.Get(), w, h, &emojiLayout)))
        return false;
    const D2D1_RECT_F ink = InkBox(emojiLayout.Get());
    const float emojiW = ink.right - ink.left;
    const float emojiH = ink.bottom - ink.top;

    const float cardW = (std::min)(w * 0.9f, padX + emojiW + emojiGap + textW + padX);
    const float cardH = blockH + 2 * padY;
    const float left = std::round((w - cardW) / 2);
    const float top = std::round((h - cardH) / 2);
    const float radius = titleSize * 0.8f;
    const D2D1_ROUNDED_RECT card = D2D1::RoundedRect(D2D1::RectF(left, top, left + cardW, top + cardH), radius, radius);

    ComPtr<ID2D1SolidColorBrush> dim, shadow, fill, border, titleBrush, subBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.6f), &dim);         // picture at ~40%
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.28f), &shadow);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.09f, 0.09f, 0.11f, 0.84f), &fill);
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.12f), &border);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.97f, 0.97f, 0.98f, 1.0f), &titleBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.97f, 0.97f, 0.98f, 0.6f), &subBrush);
    if (!dim || !shadow || !fill || !border || !titleBrush || !subBrush)
        return false;

    rt->BeginDraw();
    rt->FillRectangle(D2D1::RectF(0, 0, w, h), dim.Get());
    // Soft shadow: a few widening, fading rounded rects under the card.
    for (int i = 3; i >= 1; --i) {
        const float grow = titleSize * 0.12f * i;
        D2D1_ROUNDED_RECT s = card;
        s.rect.left -= grow;
        s.rect.right += grow;
        s.rect.top -= grow * 0.5f;
        s.rect.bottom += grow * 1.5f;
        s.radiusX = s.radiusY = radius + grow;
        shadow->SetOpacity(0.10f * (4 - i));
        rt->FillRoundedRectangle(s, shadow.Get());
    }
    rt->FillRoundedRectangle(card, fill.Get());
    rt->DrawRoundedRectangle(card, border.Get(), (std::max)(1.0f, titleSize * 0.05f));

    const float blockTop = top + padY;
    const float emojiX = left + padX - ink.left;
    const float emojiY = blockTop + (blockH - emojiH) / 2 - ink.top;
    rt->DrawTextLayout(D2D1::Point2F(emojiX, emojiY), emojiLayout.Get(), titleBrush.Get(),
                       D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    const float textX = left + padX + emojiW + emojiGap;
    const float titleX = textX - tm.left;
    const float titleY = blockTop - tm.top;
    rt->DrawTextLayout(D2D1::Point2F(titleX, titleY), titleLayout.Get(), titleBrush.Get(),
                       D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    const float subX = textX - sm.left;
    const float subY = blockTop + tm.height + gap - sm.top;
    rt->DrawTextLayout(D2D1::Point2F(subX, subY), subLayout.Get(), subBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    return SUCCEEDED(rt->EndDraw());
}

bool CreateWicFactory(ComPtr<IWICImagingFactory>& wic)
{
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
}

bool SavePng(IWICImagingFactory* wic, IWICBitmap* bitmap, const std::wstring& path)
{
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)))
        return false;
    UINT w = 0, h = 0;
    bitmap->GetSize(&w, &h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&fmt)) ||
        FAILED(frame->WriteSource(bitmap, nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return false;
    return true;
}

} // namespace

const char* PauseReasonName(PauseReason reason)
{
    switch (reason) {
        case PauseReason::None:
            return "none";
        case PauseReason::Locked:
            return "locked";
        case PauseReason::Asleep:
            return "display asleep";
        case PauseReason::Admin:
            return "admin prompt";
        default:
            return "paused";
    }
}

bool RenderPauseOverlay(uint8_t* bgra, uint32_t width, uint32_t height, PauseReason reason)
{
    if (!bgra || width == 0 || height == 0)
        return false;
    ComScope com;
    ComPtr<IWICImagingFactory> wic;
    if (!CreateWicFactory(wic))
        return false;
    const size_t bytes = static_cast<size_t>(width) * height * 4;
    for (size_t i = 3; i < bytes; i += 4)
        bgra[i] = 0xFF; // desktop alpha is undefined; draw on an opaque picture
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA, width * 4,
                                           static_cast<UINT>(bytes), bgra, &bitmap)))
        return false;
    if (!DrawOverlay(wic.Get(), bitmap.Get(), width, height, reason))
        return false;
    return SUCCEEDED(bitmap->CopyPixels(nullptr, width * 4, static_cast<UINT>(bytes), bgra));
}

bool SavePauseOverlayPreviews(const std::wstring& dir, uint32_t width, uint32_t height)
{
    ComScope com;
    ComPtr<IWICImagingFactory> wic;
    if (!CreateWicFactory(wic))
        return false;
    CreateDirectoryW(dir.c_str(), nullptr);

    // Desktop-ish sample: blue-gray gradient, a light "window" and a taskbar.
    std::vector<uint8_t> sample(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* p = &sample[(static_cast<size_t>(y) * width + x) * 4];
            const float fx = static_cast<float>(x) / width, fy = static_cast<float>(y) / height;
            int r = static_cast<int>(40 + 60 * fx), g = static_cast<int>(70 + 50 * fy), b = static_cast<int>(120 + 80 * (1 - fy));
            const bool window = fx > 0.12f && fx < 0.62f && fy > 0.14f && fy < 0.72f;
            const bool titleBar = window && fy < 0.19f;
            const bool taskbar = fy > 0.95f;
            if (window) r = g = b = titleBar ? 225 : 245;
            if (window && !titleBar && (y / 24) % 3 == 0 && fx < 0.5f) r = g = b = 180;
            if (taskbar) r = g = b = 32;
            p[0] = static_cast<uint8_t>(b);
            p[1] = static_cast<uint8_t>(g);
            p[2] = static_cast<uint8_t>(r);
            p[3] = 0xFF;
        }
    }

    bool ok = true;
    const struct {
        PauseReason reason;
        const wchar_t* file;
    } items[] = {{PauseReason::Locked, L"overlay-locked.png"},
                 {PauseReason::Asleep, L"overlay-asleep.png"},
                 {PauseReason::Admin, L"overlay-admin.png"},
                 {PauseReason::Paused, L"overlay-paused.png"}};
    for (const auto& item : items) {
        std::vector<uint8_t> img = sample;
        if (!RenderPauseOverlay(img.data(), width, height, item.reason)) {
            ok = false;
            continue;
        }
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(wic->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, width * 4,
                                               static_cast<UINT>(img.size()), img.data(), &bitmap)) ||
            !SavePng(wic.Get(), bitmap.Get(), dir + L"\\" + item.file))
            ok = false;
    }
    return ok;
}

} // namespace od
