#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace od {

struct EncodedFrame {
    std::vector<uint8_t> annexB; // one full access unit, 4-byte start codes, SPS/PPS ensured on IDR
    bool isKeyFrame = false;
};

// Wraps a Media Foundation H.264 encoder MFT (hardware NVENC/QuickSync/AMF if
// available, software fallback otherwise - spec §7c). Handles both
// synchronous and asynchronous MFTs transparently (hardware encoders are
// almost always asynchronous).
class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // (Re)configures the encoder for the given frame size.
    // bitrateBps: peak for PeakConstrainedVBR; soft max under Quality RC.
    // qualityVsSpeed: MS CODECAPI 0=faster/lower quality, 100=slower/higher.
    // gopSeconds: IDR interval; 0 defaults to 2 seconds.
    // useQualityRc: MF Quality RC + rcQuality (CODECAPI_AVEncCommonQuality).
    bool Configure(uint32_t width, uint32_t height, uint32_t fps = 60, uint32_t bitrateBps = 30'000'000,
                   uint32_t qualityVsSpeed = 0, uint32_t gopSeconds = 0, bool useQualityRc = false,
                   uint32_t rcQuality = 90);

    // Mid-stream peak/mean via ICodecAPI (no rebind). No-op under Quality RC.
    // Caller should RequestKeyFrame after a successful change.
    bool UpdateBitrate(uint32_t peakBitrateBps, uint32_t meanBitrateBps);

    bool UsesQualityRc() const;

    // Encodes one NV12 frame (size must be width*height*3/2 bytes). May
    // return zero access units (encoder still warming up / buffering) or,
    // occasionally, more than one.
    std::vector<EncodedFrame> EncodeNv12(const uint8_t* nv12, size_t size);

    // Makes the next encoded access unit an IDR keyframe with SPS/PPS
    // prepended (spec §5 "kf" handling).
    void RequestKeyFrame();

    bool IsConfigured() const;

    // The geometry the encoder is currently configured for. The capture side
    // compares against these to notice a rotation/resolution change that came
    // from Windows instead of from the receiver's `hello`.
    uint32_t Width() const;
    uint32_t Height() const;

    // Friendly name of the activated H.264 MFT (empty if unknown).
    std::wstring MftName() const;

    // Async MFTs that stall on NeedInput: last EncodeNv12 timed out, and a
    // running count so the sender can fall back to software after N misses.
    bool TookAsyncTimeout() const;
    int ConsecutiveAsyncTimeouts() const;
    void SetAllowHardware(bool allow);

    // Create an Intel D3D11 device + DXGI manager for QSV (not the capture /
    // Parsec adapter). Call before Configure(). Returns false if no Intel GPU.
    bool EnsureIntelEncoderDevice();

    // Prefer the capture D3D device when it is Intel (MTT path): one device for
    // VideoProcessor convert + QSV DXGI input. Call before Configure().
    bool AdoptD3DDevice(ID3D11Device* device, ID3D11DeviceContext* context);

    bool UsesDxgiInput() const;
    ID3D11Device* D3DDevice() const;

    // Encode an NV12 texture already resident on the encoder D3D device
    // (must match Configure width/height, including 16-ceil pad).
    std::vector<EncodedFrame> EncodeDxgiNv12(ID3D11Texture2D* nv12);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace od
