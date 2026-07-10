#include "display/DesktopDuplication.h"

#include <algorithm>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace od {

namespace {

inline uint8_t Clamp8(int v)
{
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// BT.601 studio-range BGRA -> NV12. CPU-side; fine for a local-link sender,
// not optimized (a GPU color-convert MFT would be the follow-up if this
// turns out to be the bottleneck).
void ConvertBgraToNv12(const uint8_t* bgra, UINT rowPitch, uint32_t width, uint32_t height, std::vector<uint8_t>& nv12)
{
    nv12.resize(static_cast<size_t>(width) * height * 3 / 2);
    uint8_t* yPlane = nv12.data();
    uint8_t* uvPlane = nv12.data() + static_cast<size_t>(width) * height;

    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* srcRow = bgra + static_cast<size_t>(row) * rowPitch;
        uint8_t* yRow = yPlane + static_cast<size_t>(row) * width;
        for (uint32_t col = 0; col < width; ++col) {
            const uint8_t* px = srcRow + static_cast<size_t>(col) * 4; // B G R A
            int b = px[0], g = px[1], r = px[2];
            int y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yRow[col] = Clamp8(y);
        }
    }

    for (uint32_t row = 0; row < height; row += 2) {
        const uint8_t* srcRow0 = bgra + static_cast<size_t>(row) * rowPitch;
        const uint8_t* srcRow1 = bgra + static_cast<size_t>(std::min(row + 1, height - 1)) * rowPitch;
        uint8_t* uvRow = uvPlane + static_cast<size_t>(row / 2) * width;

        for (uint32_t col = 0; col < width; col += 2) {
            uint32_t col1 = std::min(col + 1, width - 1);

            auto sample = [](const uint8_t* rowPtr, uint32_t c, int& r, int& g, int& b) {
                const uint8_t* px = rowPtr + static_cast<size_t>(c) * 4;
                b = px[0];
                g = px[1];
                r = px[2];
            };

            int r, g, b, sr = 0, sg = 0, sb = 0;
            sample(srcRow0, col, r, g, b); sr += r; sg += g; sb += b;
            sample(srcRow0, col1, r, g, b); sr += r; sg += g; sb += b;
            sample(srcRow1, col, r, g, b); sr += r; sg += g; sb += b;
            sample(srcRow1, col1, r, g, b); sr += r; sg += g; sb += b;
            r = sr / 4; g = sg / 4; b = sb / 4;

            int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

            uvRow[col] = Clamp8(u);
            uvRow[col1] = Clamp8(v);
        }
    }
}

} // namespace

bool DesktopDuplication::Open(const std::wstring& deviceName)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;

    for (UINT ai = 0; !output && factory->EnumAdapters1(ai, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++ai) {
        ComPtr<IDXGIOutput> candidate;
        for (UINT oi = 0; adapter->EnumOutputs(oi, candidate.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(candidate->GetDesc(&desc)) && deviceName == desc.DeviceName) {
                output = candidate;
                break;
            }
        }
    }

    if (!output) {
        fprintf(stderr, "DesktopDuplication: output matching %ls not found\n", deviceName.c_str());
        return false;
    }

    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                    D3D11_SDK_VERSION, &device_, &level, &context_);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D11CreateDevice failed: 0x%08lx\n", hr);
        return false;
    }

    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1)))
        return false;

    hr = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
        fprintf(stderr, "DuplicateOutput failed: 0x%08lx\n", hr);
        return false;
    }

    DXGI_OUTDUPL_DESC dupDesc;
    duplication_->GetDesc(&dupDesc);
    width_ = dupDesc.ModeDesc.Width;
    height_ = dupDesc.ModeDesc.Height;

    deviceName_ = deviceName;
    return true;
}

void DesktopDuplication::Close()
{
    duplication_.Reset();
    staging_.Reset();
    context_.Reset();
    device_.Reset();
}

bool DesktopDuplication::CaptureFrameNv12(std::vector<uint8_t>& nv12, int timeoutMs)
{
    if (!duplication_)
        return false;

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(timeoutMs, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        return false;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        // Common after a lock-screen/power-state change; the duplication
        // interface is permanently dead once this happens and must be
        // recreated from scratch (staging texture too, in case format/size
        // changed while we were locked out).
        fprintf(stderr, "AcquireNextFrame: access lost, reacquiring duplication\n");
        std::wstring name = deviceName_;
        Close();
        Open(name);
        return false;
    }
    if (FAILED(hr)) {
        fprintf(stderr, "AcquireNextFrame failed: 0x%08lx\n", hr);
        return false;
    }

    // The cursor's shape and position arrive via the frame info, independent
    // of whether the desktop image itself changed (a mouse-only move still
    // produces a frame). Refresh our cache before compositing.
    UpdatePointer(info);

    ComPtr<ID3D11Texture2D> texture;
    hr = resource.As(&texture);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    if (!staging_) {
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        // READ to convert to NV12, WRITE so we can blend the cursor in place.
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
        stagingDesc.MiscFlags = 0;
        if (FAILED(device_->CreateTexture2D(&stagingDesc, nullptr, &staging_))) {
            duplication_->ReleaseFrame();
            return false;
        }
    }

    context_->CopyResource(staging_.Get(), texture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ_WRITE, 0, &mapped);
    if (SUCCEEDED(hr)) {
        if (pointerVisible_ && !pointerShape_.empty())
            CompositePointer(reinterpret_cast<uint8_t*>(mapped.pData), mapped.RowPitch);
        ConvertBgraToNv12(reinterpret_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, width_, height_, nv12);
        context_->Unmap(staging_.Get(), 0);
    }

    duplication_->ReleaseFrame();
    return SUCCEEDED(hr);
}

void DesktopDuplication::UpdatePointer(const DXGI_OUTDUPL_FRAME_INFO& info)
{
    // Position is only meaningful when the mouse actually updated this frame;
    // otherwise keep the last known position so a static cursor stays put.
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        pointerVisible_ = info.PointerPosition.Visible != 0;
        pointerPosition_.x = info.PointerPosition.Position.x;
        pointerPosition_.y = info.PointerPosition.Position.y;
    }

    // A non-zero shape buffer size means the cursor bitmap itself changed
    // (e.g. arrow -> I-beam) — re-fetch and cache it.
    if (info.PointerShapeBufferSize != 0) {
        pointerShape_.resize(info.PointerShapeBufferSize);
        UINT required = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo{};
        HRESULT hr = duplication_->GetFramePointerShape(
            info.PointerShapeBufferSize, pointerShape_.data(), &required, &shapeInfo);
        if (SUCCEEDED(hr))
            pointerShapeInfo_ = shapeInfo;
    }
}

void DesktopDuplication::CompositePointer(uint8_t* bgra, uint32_t stride) const
{
    const int posX = pointerPosition_.x;
    const int posY = pointerPosition_.y;
    const UINT pitch = pointerShapeInfo_.Pitch;
    const int shapeW = static_cast<int>(pointerShapeInfo_.Width);
    int shapeH = static_cast<int>(pointerShapeInfo_.Height);

    auto inFrame = [&](int fx, int fy) {
        return fx >= 0 && fy >= 0 && fx < static_cast<int>(width_) && fy < static_cast<int>(height_);
    };

    if (pointerShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        // 1bpp, AND mask on top of XOR mask (so reported height is doubled).
        shapeH /= 2;
        const uint8_t* andMask = pointerShape_.data();
        const uint8_t* xorMask = pointerShape_.data() + static_cast<size_t>(pitch) * shapeH;
        for (int y = 0; y < shapeH; ++y) {
            for (int x = 0; x < shapeW; ++x) {
                int fx = posX + x, fy = posY + y;
                if (!inFrame(fx, fy))
                    continue;
                size_t byteIdx = static_cast<size_t>(y) * pitch + (x / 8);
                int bit = 7 - (x % 8);
                int a = (andMask[byteIdx] >> bit) & 1;
                int xr = (xorMask[byteIdx] >> bit) & 1;
                uint8_t* dst = bgra + static_cast<size_t>(fy) * stride + static_cast<size_t>(fx) * 4;
                if (a == 0 && xr == 0) {
                    dst[0] = dst[1] = dst[2] = 0; // black
                } else if (a == 0 && xr == 1) {
                    dst[0] = dst[1] = dst[2] = 255; // white
                } else if (a == 1 && xr == 1) {
                    dst[0] = 255 - dst[0]; dst[1] = 255 - dst[1]; dst[2] = 255 - dst[2]; // invert screen
                }
                // a==1, xr==0 -> transparent (leave the screen pixel)
            }
        }
        return;
    }

    // COLOR and MASKED_COLOR are both 32bpp BGRA.
    const bool masked = pointerShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
    for (int y = 0; y < shapeH; ++y) {
        for (int x = 0; x < shapeW; ++x) {
            int fx = posX + x, fy = posY + y;
            if (!inFrame(fx, fy))
                continue;
            const uint8_t* src = pointerShape_.data() + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4;
            uint8_t* dst = bgra + static_cast<size_t>(fy) * stride + static_cast<size_t>(fx) * 4;
            uint8_t alpha = src[3];
            if (masked) {
                // MASKED_COLOR: alpha 0 => opaque copy, 0xFF => XOR with screen.
                if (alpha == 0) {
                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
                } else {
                    dst[0] ^= src[0]; dst[1] ^= src[1]; dst[2] ^= src[2];
                }
            } else {
                // COLOR: straight per-pixel alpha blend.
                dst[0] = static_cast<uint8_t>((src[0] * alpha + dst[0] * (255 - alpha)) / 255);
                dst[1] = static_cast<uint8_t>((src[1] * alpha + dst[1] * (255 - alpha)) / 255);
                dst[2] = static_cast<uint8_t>((src[2] * alpha + dst[2] * (255 - alpha)) / 255);
            }
        }
    }
}

} // namespace od
