#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace od {

// Captures the virtual monitor's output via DXGI Desktop Duplication and
// converts BGRA -> NV12 (the encoder's required input format) on the CPU.
class DesktopDuplication {
public:
    DesktopDuplication() = default;
    ~DesktopDuplication() = default;

    DesktopDuplication(const DesktopDuplication&) = delete;
    DesktopDuplication& operator=(const DesktopDuplication&) = delete;

    // deviceName is the GDI device name (e.g. L"\\.\DISPLAY3") from
    // VirtualDisplay::DeviceName() — the D3D11 device must be created on the
    // same adapter as this output for DuplicateOutput() to work.
    bool Open(const std::wstring& deviceName);
    void Close();

    // Captures one frame and appends its NV12 conversion into `nv12`
    // (resized as needed). Returns false on timeout (no desktop update since
    // the last call — caller should just re-encode/resend the previous
    // frame) or on error.
    bool CaptureFrameNv12(std::vector<uint8_t>& nv12, int timeoutMs = 500);

    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }

private:
    // DXGI delivers the mouse cursor out-of-band (it is NOT baked into the
    // duplicated desktop image), so we cache its latest shape/position and
    // blend it into each captured frame ourselves before NV12 conversion.
    void UpdatePointer(const DXGI_OUTDUPL_FRAME_INFO& info);
    void CompositePointer(uint8_t* bgra, uint32_t stride) const;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;

    std::wstring deviceName_; // remembered for reacquiring after DXGI_ERROR_ACCESS_LOST
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    std::vector<uint8_t> pointerShape_;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointerShapeInfo_{};
    POINT pointerPosition_{};
    bool pointerVisible_ = false;
};

} // namespace od
