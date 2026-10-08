#include "display/DesktopDuplication.h"

#include <d3d10_1.h>
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
                outputRect_ = desc.DesktopCoordinates;
                break;
            }
        }
    }

    if (!output) {
        fprintf(stderr, "DesktopDuplication: output matching %ls not found\n", deviceName.c_str());
        return false;
    }

    D3D_FEATURE_LEVEL level;
    // VIDEO_SUPPORT required when sharing this device with a HW H.264 MFT.
    const UINT createFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, createFlags, nullptr, 0,
                                    D3D11_SDK_VERSION, &device_, &level, &context_);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D11CreateDevice failed: 0x%08lx\n", hr);
        return false;
    }
    {
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device_.As(&mt)))
            mt->SetMultithreadProtected(TRUE);
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
    ResetGpuConverter();
    return true;
}

void DesktopDuplication::ResetGpuConverter()
{
    videoProcessor_.Reset();
    vpEnum_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    bgraGpu_.Reset();
    gpuNv12_.Reset();
    gpuEncW_ = gpuEncH_ = gpuSrcW_ = gpuSrcH_ = 0;
    gpuNv12Ready_ = false;
}

void DesktopDuplication::Close()
{
    duplication_.Reset();
    staging_.Reset();
    ResetGpuConverter();
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

void DesktopDuplication::NudgePresent()
{
    // 1x1 SRCCOPY on the output's desktop origin dirties DWM enough for
    // DuplicateOutput to deliver a frame (otherwise AcquireNextFrame timeouts).
    if (outputRect_.right <= outputRect_.left || outputRect_.bottom <= outputRect_.top)
        return;
    HDC hdc = GetDC(nullptr);
    if (!hdc)
        return;
    const int x = outputRect_.left;
    const int y = outputRect_.top;
    BitBlt(hdc, x, y, 1, 1, hdc, x, y, SRCCOPY);
    ReleaseDC(nullptr, hdc);
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
        result.accessLost = true;
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
        ResetGpuConverter();
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


bool DesktopDuplication::EnsureGpuNv12Converter(uint32_t encW, uint32_t encH)
{
    if (!device_ || !context_ || encW == 0 || encH == 0 || width_ == 0 || height_ == 0)
        return false;
    if (encW < width_ || encH < height_)
        return false;

    if (gpuNv12Ready_ && gpuEncW_ == encW && gpuEncH_ == encH && gpuSrcW_ == width_ && gpuSrcH_ == height_ &&
        videoProcessor_ && gpuNv12_ && bgraGpu_)
        return true;

    ResetGpuConverter();

    if (FAILED(device_.As(&videoDevice_)) || !videoDevice_)
        return false;
    if (FAILED(context_.As(&videoContext_)) || !videoContext_)
        return false;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {60, 1};
    cd.InputWidth = width_;
    cd.InputHeight = height_;
    cd.OutputFrameRate = {60, 1};
    cd.OutputWidth = encW;
    cd.OutputHeight = encH;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (FAILED(videoDevice_->CreateVideoProcessorEnumerator(&cd, &vpEnum_)) || !vpEnum_)
        return false;

    UINT caps = 0;
    if (FAILED(vpEnum_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &caps)) ||
        (caps & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0)
        return false;
    caps = 0;
    if (FAILED(vpEnum_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &caps)) ||
        (caps & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0)
        return false;

    if (FAILED(videoDevice_->CreateVideoProcessor(vpEnum_.Get(), 0, &videoProcessor_)) || !videoProcessor_)
        return false;

    // Desktop capture: AutoProcessing (denoise/sharpen) muddies chroma.
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(videoProcessor_.Get(), 0, FALSE);

    D3D11_TEXTURE2D_DESC bgraDesc{};
    bgraDesc.Width = width_;
    bgraDesc.Height = height_;
    bgraDesc.MipLevels = 1;
    bgraDesc.ArraySize = 1;
    bgraDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bgraDesc.SampleDesc.Count = 1;
    bgraDesc.Usage = D3D11_USAGE_DEFAULT;
    bgraDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&bgraDesc, nullptr, &bgraGpu_)))
        return false;

    D3D11_TEXTURE2D_DESC nv12Desc{};
    nv12Desc.Width = encW;
    nv12Desc.Height = encH;
    nv12Desc.MipLevels = 1;
    nv12Desc.ArraySize = 1;
    nv12Desc.Format = DXGI_FORMAT_NV12;
    nv12Desc.SampleDesc.Count = 1;
    nv12Desc.Usage = D3D11_USAGE_DEFAULT;
    nv12Desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&nv12Desc, nullptr, &gpuNv12_)))
        return false;

    // Full-range RGB (desktop) -> BT.709 limited NV12 (typical for H.264).
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
    inCs.RGB_Range = 0; // full (0-255)
    inCs.YCbCr_Matrix = 1;
    inCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    videoContext_->VideoProcessorSetStreamColorSpace(videoProcessor_.Get(), 0, &inCs);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
    outCs.YCbCr_Matrix = 1; // BT.709
    outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetOutputColorSpace(videoProcessor_.Get(), &outCs);

    RECT srcRect{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    RECT dstRect{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)}; // top-left pad
    RECT target{0, 0, static_cast<LONG>(encW), static_cast<LONG>(encH)};
    videoContext_->VideoProcessorSetStreamSourceRect(videoProcessor_.Get(), 0, TRUE, &srcRect);
    videoContext_->VideoProcessorSetStreamDestRect(videoProcessor_.Get(), 0, TRUE, &dstRect);
    videoContext_->VideoProcessorSetOutputTargetRect(videoProcessor_.Get(), TRUE, &target);

    D3D11_VIDEO_COLOR bg{};
    bg.YCbCr = {0.0625f, 0.5f, 0.5f, 1.0f}; // limited black
    videoContext_->VideoProcessorSetOutputBackgroundColor(videoProcessor_.Get(), TRUE, &bg);

    gpuEncW_ = encW;
    gpuEncH_ = encH;
    gpuSrcW_ = width_;
    gpuSrcH_ = height_;
    gpuNv12Ready_ = true;
    return true;
}

bool DesktopDuplication::ConvertDesktopToGpuNv12(ID3D11Texture2D* desktopBgra, uint32_t frameW, uint32_t frameH,
                                                 uint32_t encW, uint32_t encH)
{
    if (!desktopBgra || !EnsureGpuNv12Converter(encW, encH))
        return false;
    if (frameW != gpuSrcW_ || frameH != gpuSrcH_) {
        width_ = frameW;
        height_ = frameH;
        if (!EnsureGpuNv12Converter(encW, encH))
            return false;
    }

    context_->CopyResource(bgraGpu_.Get(), desktopBgra);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice = 0;
    ComPtr<ID3D11VideoProcessorInputView> inputView;
    if (FAILED(videoDevice_->CreateVideoProcessorInputView(bgraGpu_.Get(), vpEnum_.Get(), &ivd, &inputView)))
        return false;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    if (FAILED(videoDevice_->CreateVideoProcessorOutputView(gpuNv12_.Get(), vpEnum_.Get(), &ovd, &outputView)))
        return false;

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();

    HRESULT hr = videoContext_->VideoProcessorBlt(videoProcessor_.Get(), outputView.Get(), 0, 1, &stream);
    return SUCCEEDED(hr);
}

CaptureResult DesktopDuplication::CaptureFrameNv12Gpu(ComPtr<ID3D11Texture2D>& outNv12,
                                                      uint32_t encW, uint32_t encH, int timeoutMs)
{
    CaptureResult result{};
    // Only hand out the NV12 texture after a real Blt. EnsureGpuNv12Converter
    // allocates a blank texture at pipeline build; returning that before the
    // first present made keepalive encode solid black (long black screen).
    outNv12 = haveDesktopFrame_ ? gpuNv12_ : nullptr;

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
        result.accessLost = true;
        outNv12 = nullptr; // drop stale GPU frame tied to the old device
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
        ResetGpuConverter();
    }

    // Encoder still on the other orientation (common after Display Settings
    // rotate): do not Blt into the wrong NV12 - keep desktopChanged so the
    // capture loop rebuilds encode to exact capture WxH.
    const uint32_t frameEncW = (frameW + 15u) & ~15u;
    const uint32_t frameEncH = (frameH + 15u) & ~15u;
    if (frameEncW != encW || frameEncH != encH) {
        duplication_->ReleaseFrame();
        result.desktopChanged = true;
        result.acquired = true;
        outNv12 = nullptr;
        return result;
    }

    if (!ConvertDesktopToGpuNv12(texture.Get(), frameW, frameH, encW, encH)) {
        if (!reportedConvertFail_) {
            fprintf(stderr, "ConvertDesktopToGpuNv12 failed (%ux%u -> enc %ux%u)\n",
                    frameW, frameH, encW, encH);
            reportedConvertFail_ = true;
        }
        duplication_->ReleaseFrame();
        result.desktopChanged = false;
        result.acquired = false;
        outNv12 = haveDesktopFrame_ ? gpuNv12_ : nullptr;
        return result;
    }

    reportedConvertFail_ = false;
    haveDesktopFrame_ = true;
    outNv12 = gpuNv12_;
    duplication_->ReleaseFrame();
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

