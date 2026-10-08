#pragma once

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace od {

struct CaptureResult {
    bool acquired = false;
    bool desktopChanged = false;
    bool cursorChanged = false;
    bool pointerShapeChanged = false;
    // DXGI_ERROR_ACCESS_LOST (mode change, etc.): duplication was closed/reopened
    // onto a fresh D3D device. Caller must rebuild the encoder on the new device
    // instead of feeding the old MFT (ProcessInput E_FAIL / frozen picture).
    bool accessLost = false;
};

struct PointerShapeBgra {
    int width = 0;
    int height = 0;
    int hotX = 0;
    int hotY = 0;
    std::vector<uint8_t> bgra;
};

class DesktopDuplication {
public:
    DesktopDuplication() = default;
    ~DesktopDuplication() = default;

    DesktopDuplication(const DesktopDuplication&) = delete;
    DesktopDuplication& operator=(const DesktopDuplication&) = delete;

    bool Open(const std::wstring& deviceName);
    void Close();

    // CPU path: Map staging BGRA + ConvertBgraToNv12 into tightly-packed NV12.
    CaptureResult CaptureFrameNv12(std::vector<uint8_t>& nv12, int timeoutMs = 500);

    // GPU path: VideoProcessor BGRA->NV12 into a padded DXGI NV12 texture on
    // this device (encW/encH >= capture, typically 16-ceil). outNv12 is the
    // last good frame when desktop did not change (keepalive).
    CaptureResult CaptureFrameNv12Gpu(Microsoft::WRL::ComPtr<ID3D11Texture2D>& outNv12,
                                      uint32_t encW, uint32_t encH, int timeoutMs = 500);

    // Bind / rebuild the VideoProcessor + NV12 output for encoder geometry.
    bool EnsureGpuNv12Converter(uint32_t encW, uint32_t encH);
    bool GpuNv12Ready() const { return gpuNv12Ready_; }
    bool HaveDesktopFrame() const { return haveDesktopFrame_; }

    // Force a tiny dirty region on this output so DXGI duplication gets a
    // first present (MTT/VDD often sits idle after Open with no frames).
    void NudgePresent();

    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }

    // Same D3D11 device used for DXGI duplication - pass to the HW H.264 MFT
    // (Intel QSV wants one shared device + DXGI device manager).
    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }

    bool PointerVisible() const { return pointerVisible_; }
    int PointerX() const { return pointerPosition_.x; }
    int PointerY() const { return pointerPosition_.y; }
    int PointerHotX() const { return static_cast<int>(pointerShapeInfo_.HotSpot.x); }
    int PointerHotY() const { return static_cast<int>(pointerShapeInfo_.HotSpot.y); }

    bool GetPointerShapeBgra(PointerShapeBgra& out) const;

private:
    bool UpdatePointer(const DXGI_OUTDUPL_FRAME_INFO& info, bool* shapeChanged = nullptr);
    bool Reopen();
    void ResetGpuConverter();
    bool ConvertDesktopToGpuNv12(ID3D11Texture2D* desktopBgra, uint32_t frameW, uint32_t frameH,
                                 uint32_t encW, uint32_t encH);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;

    // GPU BGRA->NV12 (Video Processor)
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> videoProcessor_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> bgraGpu_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> gpuNv12_;
    uint32_t gpuEncW_ = 0;
    uint32_t gpuEncH_ = 0;
    uint32_t gpuSrcW_ = 0;
    uint32_t gpuSrcH_ = 0;
    bool gpuNv12Ready_ = false;

    std::wstring deviceName_;
    bool reportedLoss_ = false;
    bool haveDesktopFrame_ = false;
    bool reportedConvertFail_ = false;
    RECT outputRect_{};
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    std::vector<uint8_t> pointerShape_;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointerShapeInfo_{};
    POINT pointerPosition_{};
    bool pointerVisible_ = false;
    bool pointerShapeValid_ = false;
};

} // namespace od
