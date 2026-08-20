#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace od {

struct EncodedFrame {
    std::vector<uint8_t> annexB; // one full access unit, 4-byte start codes, SPS/PPS ensured on IDR
    bool isKeyFrame = false;
};

// Wraps a Media Foundation H.264 encoder MFT (hardware NVENC/QuickSync/AMF if
// available, software fallback otherwise — spec §7c). Handles both
// synchronous and asynchronous MFTs transparently (hardware encoders are
// almost always asynchronous).
class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // (Re)configures the encoder for the given frame size. bitrateBps default
    // sits in the 20-40 Mbit/s range recommended for a local, low-latency link.
    bool Configure(uint32_t width, uint32_t height, uint32_t fps = 60, uint32_t bitrateBps = 30'000'000);

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace od
