#include "app/SenderApp.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include <winsock2.h>

#include "display/DesktopDuplication.h"
#include "display/VirtualDisplay.h"
#include "encode/H264Encoder.h"
#include "input/InputInjector.h"
#include "net/Connection.h"
#include "net/Protocol.h"

namespace od {

namespace {

constexpr uint16_t kPort = 9000;
constexpr uint32_t kFps = 60;
constexpr uint32_t kBitrateBps = 30'000'000;
constexpr int kSendTimeoutMs = 500;      // backpressure: how long to wait for the socket before dropping a frame
constexpr int kReconnectDelayMs = 2000;
constexpr int kKeepaliveMs = 1000;       // max silence on a static screen; well under the iPad's ~5s watchdog
constexpr int kActiveTailMs = 300;       // keep feeding the encoder this long after the last change (drains its 1-frame hold)

uint32_t EvenClamp(int32_t v, uint32_t fallback)
{
    uint32_t u = v > 0 ? static_cast<uint32_t>(v) : fallback;
    return u & ~1u; // NV12 4:2:0 needs even dimensions
}

} // namespace

void SenderApp::Run()
{
    VirtualDisplay vdisp;
    DesktopDuplication dup;
    H264Encoder encoder;
    InputInjector input;
    std::mutex pipelineMutex;

    auto buildPipeline = [&](uint32_t width, uint32_t height) {
        // Caller holds pipelineMutex.
        if (!vdisp.IsOpen() && !vdisp.Open()) {
            fprintf(stderr, "VirtualDisplay::Open failed (parsec-vdd driver missing/inaccessible?)\n");
            return false;
        }
        if (!vdisp.EnsureResolution(width, height, kFps)) {
            fprintf(stderr, "VirtualDisplay::EnsureResolution failed (run as Administrator?)\n");
            return false;
        }
        dup.Close();
        if (!dup.Open(vdisp.DeviceName())) {
            fprintf(stderr, "DesktopDuplication::Open failed\n");
            return false;
        }
        if (!encoder.Configure(dup.Width(), dup.Height(), kFps, kBitrateBps)) {
            fprintf(stderr, "encoder configure failed\n");
            return false;
        }
        input.SetMonitorRect(vdisp.MonitorRect());
        printf("pipeline ready: %ux%u\n", dup.Width(), dup.Height());
        return true;
    };

    while (true) {
        printf("connecting to %s:%u ...\n", ip_.c_str(), kPort);
        auto conn = Connection::Connect(ip_, kPort);
        if (!conn) {
            fprintf(stderr, "connect failed, retrying in %dms\n", kReconnectDelayMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectDelayMs));
            continue;
        }
        printf("connected, waiting for hello...\n");

        HelloMsg hello;
        bool gotHello = false;
        while (!gotHello) {
            auto frame = conn->ReadFrame();
            if (!frame)
                break;
            if (!IsControlPayload(frame->data(), frame->size()))
                continue;
            auto msg = ParseControlMessage(frame->data(), frame->size());
            if (msg && msg->type == ControlType::Hello) {
                hello = msg->hello;
                gotHello = true;
            }
        }
        if (!gotHello) {
            fprintf(stderr, "disconnected before hello, reconnecting\n");
            continue;
        }

        uint32_t width = EvenClamp(hello.pixelsWide, 1920);
        uint32_t height = EvenClamp(hello.pixelsHigh, 1080);
        printf("hello: %dx%d -> %ux%u\n", hello.pixelsWide, hello.pixelsHigh, width, height);

        bool pipelineOk;
        {
            std::lock_guard<std::mutex> lock(pipelineMutex);
            pipelineOk = buildPipeline(width, height);
        }
        if (!pipelineOk) {
            conn->Close();
            std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectDelayMs));
            continue;
        }

        std::atomic<bool> running{true};

        std::thread reader([&] {
            while (running) {
                auto frame = conn->ReadFrame();
                if (!frame) {
                    running = false;
                    break;
                }
                if (!IsControlPayload(frame->data(), frame->size()))
                    continue;
                auto msg = ParseControlMessage(frame->data(), frame->size());
                if (!msg)
                    continue;

                switch (msg->type) {
                    case ControlType::Kf:
                        printf("kf requested by receiver\n");
                        encoder.RequestKeyFrame();
                        break;
                    case ControlType::Hello: {
                        // Rotation (or any panel-size change): rebuild the
                        // whole pipeline for the new dimensions.
                        uint32_t w = EvenClamp(msg->hello.pixelsWide, width);
                        uint32_t h = EvenClamp(msg->hello.pixelsHigh, height);
                        printf("hello again: %dx%d -> rebuilding pipeline at %ux%u\n", msg->hello.pixelsWide,
                               msg->hello.pixelsHigh, w, h);
                        std::lock_guard<std::mutex> lock(pipelineMutex);
                        if (!buildPipeline(w, h)) {
                            fprintf(stderr, "pipeline rebuild failed, disconnecting\n");
                            running = false;
                        }
                        break;
                    }
                    case ControlType::Touch:
                        // No pipelineMutex here on purpose: the capture loop
                        // holds it ~90% of the time (each capture blocks up to
                        // one frame interval), and taking it here starved
                        // input injection — the actual cause of the sluggish
                        // touch/drag. `input` is only ever touched by this
                        // reader thread (its initial SetMonitorRect happens on
                        // the main thread before this thread starts, and
                        // rotation rebuilds run on this same thread), so no
                        // lock is needed.
                        input.HandleTouch(msg->touch);
                        break;
                    case ControlType::Scroll:
                        input.HandleScroll(msg->scroll);
                        break;
                    default:
                        break;
                }
            }
        });

        std::vector<uint8_t> nv12;
        auto lastSend = std::chrono::steady_clock::now();
        auto lastChange = std::chrono::steady_clock::now() - std::chrono::milliseconds(kActiveTailMs);

        while (running) {
            std::vector<EncodedFrame> encoded;
            {
                // Only capture+encode need the pipeline lock (they touch dup
                // and encoder, which the reader thread may rebuild on
                // rotation). The network send is deliberately outside the
                // lock so a slow link never stalls a pending rebuild. Input
                // injection deliberately does NOT take this lock (see the
                // reader thread) — this loop holds it almost continuously.
                std::lock_guard<std::mutex> lock(pipelineMutex);

                nv12.resize(static_cast<size_t>(dup.Width()) * dup.Height() * 3 / 2);
                // CaptureFrameNv12 returns false on a pure timeout (nothing on
                // screen or cursor changed since last time).
                bool changed = dup.CaptureFrameNv12(nv12, 1000 / static_cast<int>(kFps));

                auto now = std::chrono::steady_clock::now();
                if (changed)
                    lastChange = now;

                // The async encoder holds one frame until the *next* frame is
                // fed, so if we stopped feeding the instant the screen went
                // idle, the last frame of an interaction (a tap, the end of a
                // scroll) would sit in the encoder until the next change or
                // keepalive — up to a second later, which feels sluggish. Keep
                // feeding at capture rate for a short tail after the last
                // change so the encoder stays drained and interaction stays
                // low-latency.
                bool active = now - lastChange < std::chrono::milliseconds(kActiveTailMs);

                // Keepalive re-encodes the last image (a tiny P-frame) purely
                // so the iPad's ~5s liveness watchdog (spec §5) never trips on
                // an otherwise idle desktop. Not gated on "captured a real
                // frame yet": if the first DXGI frame is delayed past the
                // watchdog, this still sends the (zero-filled) buffer so the
                // connection survives until real content arrives.
                bool keepaliveDue = now - lastSend >= std::chrono::milliseconds(kKeepaliveMs);

                if (active || keepaliveDue)
                    encoded = encoder.EncodeNv12(nv12.data(), nv12.size());
            }

            bool sentSomething = false;
            for (auto& f : encoded) {
                // Backpressure: if the socket can't take data within the
                // budget, drop the whole frame (never a partial write — that
                // would desync the receiver's framing) and force a keyframe
                // so the next frame we do send resyncs the decoder.
                if (!conn->WaitWritable(kSendTimeoutMs)) {
                    encoder.RequestKeyFrame();
                    break;
                }
                if (!conn->SendFrame(f.annexB.data(), static_cast<uint32_t>(f.annexB.size()))) {
                    // A real send error (blocking send, so not a timeout):
                    // the connection is gone. Drop it and let the outer loop
                    // reconnect + resync with a fresh hello and keyframe.
                    running = false;
                    break;
                }
                sentSomething = true;
                if (f.isKeyFrame)
                    printf("sent keyframe, %zu bytes\n", f.annexB.size());
            }
            if (sentSomething)
                lastSend = std::chrono::steady_clock::now();
        }

        conn->Close();
        reader.join();
        printf("disconnected, reconnecting\n");
    }
}

} // namespace od
