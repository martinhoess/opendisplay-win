#pragma once

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

    CaptureResult CaptureFrameNv12(std::vector<uint8_t>& nv12, int timeoutMs = 500);

    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }

    bool PointerVisible() const { return pointerVisible_; }
    int PointerX() const { return pointerPosition_.x; }
    int PointerY() const { return pointerPosition_.y; }
    int PointerHotX() const { return static_cast<int>(pointerShapeInfo_.HotSpot.x); }
    int PointerHotY() const { return static_cast<int>(pointerShapeInfo_.HotSpot.y); }

    bool GetPointerShapeBgra(PointerShapeBgra& out) const;

private:
    bool UpdatePointer(const DXGI_OUTDUPL_FRAME_INFO& info, bool* shapeChanged = nullptr);
    bool Reopen();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;

    std::wstring deviceName_;
    bool reportedLoss_ = false;
    bool haveDesktopFrame_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    std::vector<uint8_t> pointerShape_;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointerShapeInfo_{};
    POINT pointerPosition_{};
    bool pointerVisible_ = false;
    bool pointerShapeValid_ = false;
};

} // namespace od
