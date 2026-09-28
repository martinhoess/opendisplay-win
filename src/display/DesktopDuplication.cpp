#include "display/DesktopDuplication.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace od {

namespace {

constexpr int kRecoveryBackoffMs = 100;

inline uint8_t Clamp8(int v)
{
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

void ConvertBgraToNv12(const uint8_t* bgra, UINT rowPitch, uint32_t width, uint32_t height, std::vector<uint8_t>& nv12)
{
    nv12.resize(static_cast<size_t>(width) * height * 3 / 2);
    uint8_t* yPlane = nv12.data();
    uint8_t* uvPlane = nv12.data() + static_cast<size_t>(width) * height;

    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* srcRow = bgra + static_cast<size_t>(row) * rowPitch;
        uint8_t* yRow = yPlane + static_cast<size_t>(row) * width;
        for (uint32_t col = 0; col < width; ++col) {
            const uint8_t* px = srcRow + static_cast<size_t>(col) * 4;
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
    haveDesktopFrame_ = false;
    return true;
}

void DesktopDuplication::Close()
{
    duplication_.Reset();
    staging_.Reset();
    context_.Reset();
    device_.Reset();
    haveDesktopFrame_ = false;
}

bool DesktopDuplication::Reopen()
{
    std::wstring name = deviceName_;
    Close();
    return Open(name);
}

CaptureResult DesktopDuplication::CaptureFrameNv12(std::vector<uint8_t>& nv12, int timeoutMs)
{
    CaptureResult result{};

    if (!duplication_) {
        if (!Reopen()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kRecoveryBackoffMs));
            return result;
        }
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(timeoutMs, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        return result;
    if (FAILED(hr)) {
        if (!reportedLoss_) {
            fprintf(stderr, "AcquireNextFrame lost (0x%08lx), rebuilding duplication\n", hr);
            reportedLoss_ = true;
        }
        Reopen();
        std::this_thread::sleep_for(std::chrono::milliseconds(kRecoveryBackoffMs));
        return result;
    }
    reportedLoss_ = false;
    result.acquired = true;

    bool shapeChanged = false;
    result.cursorChanged = UpdatePointer(info, &shapeChanged);
    result.pointerShapeChanged = shapeChanged;

    const bool desktopChanged = info.LastPresentTime.QuadPart != 0 || !haveDesktopFrame_;
    result.desktopChanged = desktopChanged;

    if (!desktopChanged) {
        duplication_->ReleaseFrame();
        return result;
    }

    ComPtr<ID3D11Texture2D> texture;
    hr = resource.As(&texture);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        result.acquired = false;
        result.desktopChanged = false;
        return result;
    }

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    const uint32_t frameW = desc.Width;
    const uint32_t frameH = desc.Height;

    if (frameW != width_ || frameH != height_) {
        width_ = frameW;
        height_ = frameH;
        staging_.Reset();
    }

    if (!staging_) {
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        if (FAILED(device_->CreateTexture2D(&stagingDesc, nullptr, &staging_))) {
            duplication_->ReleaseFrame();
            result.acquired = false;
            result.desktopChanged = false;
            return result;
        }
    }

    context_->CopyResource(staging_.Get(), texture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(hr)) {
        ConvertBgraToNv12(reinterpret_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, frameW, frameH, nv12);
        context_->Unmap(staging_.Get(), 0);
        haveDesktopFrame_ = true;
    } else {
        result.desktopChanged = false;
    }

    duplication_->ReleaseFrame();
    if (FAILED(hr))
        result.acquired = false;
    return result;
}

bool DesktopDuplication::UpdatePointer(const DXGI_OUTDUPL_FRAME_INFO& info, bool* shapeChanged)
{
    bool changed = false;
    if (shapeChanged)
        *shapeChanged = false;

    if (info.LastMouseUpdateTime.QuadPart != 0) {
        const bool vis = info.PointerPosition.Visible != 0;
        const int x = info.PointerPosition.Position.x;
        const int y = info.PointerPosition.Position.y;
        if (vis != pointerVisible_ || x != pointerPosition_.x || y != pointerPosition_.y)
            changed = true;
        pointerVisible_ = vis;
        pointerPosition_.x = x;
        pointerPosition_.y = y;
    }

    if (info.PointerShapeBufferSize != 0) {
        pointerShape_.resize(info.PointerShapeBufferSize);
        UINT required = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo{};
        HRESULT hr = duplication_->GetFramePointerShape(
            info.PointerShapeBufferSize, pointerShape_.data(), &required, &shapeInfo);
        if (SUCCEEDED(hr)) {
            pointerShapeInfo_ = shapeInfo;
            pointerShapeValid_ = true;
            changed = true;
            if (shapeChanged)
                *shapeChanged = true;
        }
    }

    return changed;
}

bool DesktopDuplication::GetPointerShapeBgra(PointerShapeBgra& out) const
{
    if (!pointerShapeValid_ || pointerShape_.empty())
        return false;

    const UINT pitch = pointerShapeInfo_.Pitch;
    const int shapeW = static_cast<int>(pointerShapeInfo_.Width);
    int shapeH = static_cast<int>(pointerShapeInfo_.Height);
    out.hotX = static_cast<int>(pointerShapeInfo_.HotSpot.x);
    out.hotY = static_cast<int>(pointerShapeInfo_.HotSpot.y);

    if (pointerShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        shapeH /= 2;
        out.width = shapeW;
        out.height = shapeH;
        out.bgra.assign(static_cast<size_t>(shapeW) * shapeH * 4, 0);
        const uint8_t* andMask = pointerShape_.data();
        const uint8_t* xorMask = pointerShape_.data() + static_cast<size_t>(pitch) * shapeH;
        for (int y = 0; y < shapeH; ++y) {
            for (int x = 0; x < shapeW; ++x) {
                size_t byteIdx = static_cast<size_t>(y) * pitch + (x / 8);
                int bit = 7 - (x % 8);
                int a = (andMask[byteIdx] >> bit) & 1;
                int xr = (xorMask[byteIdx] >> bit) & 1;
                uint8_t* dst = out.bgra.data() + (static_cast<size_t>(y) * shapeW + x) * 4;
                if (a == 0 && xr == 0) {
                    dst[0] = dst[1] = dst[2] = 0;
                    dst[3] = 255;
                } else if (a == 0 && xr == 1) {
                    dst[0] = dst[1] = dst[2] = 255;
                    dst[3] = 255;
                } else if (a == 1 && xr == 1) {
                    dst[0] = dst[1] = dst[2] = 255;
                    dst[3] = 255;
                } else {
                    dst[3] = 0;
                }
            }
        }
        return true;
    }

    out.width = shapeW;
    out.height = shapeH;
    out.bgra.resize(static_cast<size_t>(shapeW) * shapeH * 4);
    const bool masked = pointerShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
    for (int y = 0; y < shapeH; ++y) {
        for (int x = 0; x < shapeW; ++x) {
            const uint8_t* src = pointerShape_.data() + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4;
            uint8_t* dst = out.bgra.data() + (static_cast<size_t>(y) * shapeW + x) * 4;
            if (masked) {
                if (src[3] == 0) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = 255;
                } else {
                    dst[0] = dst[1] = dst[2] = 0;
                    dst[3] = 0;
                }
            } else {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
            }
        }
    }
    return true;
}

} // namespace od
