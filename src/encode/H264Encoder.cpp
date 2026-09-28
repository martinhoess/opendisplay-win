#include "encode/H264Encoder.h"
#include "encode/AnnexB.h"
#include "app/Log.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <atomic>
#include <string>
#include <vector>
#include <codecapi.h>
#include <icodecapi.h>
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <oleauto.h>
#include <wrl/client.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;

namespace od {

namespace {

// Best-effort ICodecAPI setter — hardware MFTs don't all support every knob,
// and that's fine (we only require the resulting *behavior*, not every switch).
// Failures are expected and intentionally silent: e.g. NVENC rejects setting
// the B-picture count to 0 (E_INVALIDARG) yet emits no B-frames anyway.
void TrySetUInt32(ICodecAPI* api, const GUID& key, ULONG value)
{
    if (!api) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    api->SetValue(&key, &v);
}

void TrySetBool(ICodecAPI* api, const GUID& key, bool value)
{
    if (!api) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    api->SetValue(&key, &v);
}

} // namespace

struct H264Encoder::Impl {
    ComPtr<IMFTransform> mft;
    std::wstring mftName;
    ComPtr<IMFMediaEventGenerator> eventGen;
    ComPtr<ICodecAPI> codecApi;
    bool isAsync = false;
    bool pendingNeedInput = false;
    bool asyncTimedOut = false;
    bool lastAsyncTimedOut = false;
    int consecutiveAsyncTimeouts = 0;
    bool allowHardware = true;
    bool providesSamples = false;
    DWORD outputBufferSize = 0;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 60;
    LONGLONG frameDuration100ns = 0;
    LONGLONG timestamp = 0;

    bool configured = false;
    std::atomic<bool> forceKeyFrame{false};
    SpsPpsCache spsPpsCache;

    HRESULT comInitResult = S_FALSE;

    static std::string NarrowAscii(const std::wstring& w)
    {
        std::string s;
        s.reserve(w.size());
        for (wchar_t c : w)
            s.push_back(c >= 32 && c < 127 ? static_cast<char>(c) : '?');
        return s;
    }

    static std::wstring FriendlyName(IMFActivate* act)
    {
        WCHAR* friendly = nullptr;
        UINT32 friendlyLen = 0;
        if (SUCCEEDED(act->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &friendlyLen)) && friendly) {
            std::wstring name(friendly);
            CoTaskMemFree(friendly);
            return name;
        }
        return L"(unnamed)";
    }

    // Lower preference number = try first. Prefer discrete GPU encoders so we
    // can use NVENC while DXGI capture stays on the iGPU that owns Parsec's VDD.
    static int HwPreference(const std::wstring& name)
    {
        std::wstring lower = name;
        for (auto& c : lower)
            c = static_cast<wchar_t>(towlower(c));
        auto has = [&](const wchar_t* sub) { return lower.find(sub) != std::wstring::npos; };
        if (has(L"nvidia") || has(L"nvenc"))
            return 0;
        // Prefer Intel QSV after NVIDIA: same-side as typical Parsec/iGPU
        // capture on Optimus, unlike NVENC which often ActivateObject-fails.
        if (has(L"intel") || has(L"quick sync") || has(L"qsv"))
            return 1;
        if (has(L"amd") || has(L"vce") || has(L"amf") || has(L"radeon"))
            return 2;
        return 3;
    }


    void ResetMft()
    {
        if (mft) {
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        }
        mft.Reset();
        eventGen.Reset();
        codecApi.Reset();
        isAsync = false;
        pendingNeedInput = false;
        providesSamples = false;
        outputBufferSize = 0;
        mftName.clear();
    }

    // Activate + media types + codec knobs for one MFT. On failure logs and
    // leaves the encoder unbound (caller may try the next activate).
    bool TryBindActivate(IMFActivate* act,
                         const std::wstring& name,
                         uint32_t width,
                         uint32_t height,
                         uint32_t fps,
                         uint32_t bitrateBps)
    {
        ResetMft();

        HRESULT hr = act->ActivateObject(IID_PPV_ARGS(mft.ReleaseAndGetAddressOf()));
        if (FAILED(hr) || !mft) {
            Logf("encoder", "MFT activate failed: %s hr=0x%08lX\n", NarrowAscii(name).c_str(),
                 static_cast<unsigned long>(hr));
            act->ShutdownObject();
            mft.Reset();
            return false;
        }
        mftName = name;

        ComPtr<IMFAttributes> attrs;
        isAsync = false;
        if (SUCCEEDED(mft->GetAttributes(&attrs))) {
            UINT32 async = 0;
            attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async);
            isAsync = (async != 0);
            if (isAsync) {
                attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                mft.As(&eventGen);
            }
        }

        ComPtr<IMFMediaType> outType;
        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outType->SetUINT32(MF_MT_AVG_BITRATE, bitrateBps);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        hr = mft->SetOutputType(0, outType.Get(), 0);
        if (FAILED(hr)) {
            Logf("encoder", "MFT SetOutputType failed: %s hr=0x%08lX (%ux%u @ %u)\n",
                 NarrowAscii(name).c_str(), static_cast<unsigned long>(hr), width, height, fps);
            ResetMft();
            act->ShutdownObject();
            return false;
        }

        ComPtr<IMFMediaType> inType;
        MFCreateMediaType(&inType);
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        hr = mft->SetInputType(0, inType.Get(), 0);
        if (FAILED(hr)) {
            Logf("encoder", "MFT SetInputType failed: %s hr=0x%08lX (%ux%u NV12)\n",
                 NarrowAscii(name).c_str(), static_cast<unsigned long>(hr), width, height);
            ResetMft();
            act->ShutdownObject();
            return false;
        }

        RefreshOutputStreamInfo();

        codecApi.Reset();
        if (SUCCEEDED(mft.As(&codecApi))) {
            // Peak-constrained VBR: idle desktop stays cheap; motion can rise
            // up to bitrateBps. Unconstrained VBR can spike and stall Wi-Fi.
            TrySetUInt32(codecApi.Get(), CODECAPI_AVEncCommonRateControlMode,
                         eAVEncCommonRateControlMode_PeakConstrainedVBR);
            TrySetUInt32(codecApi.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrateBps * 6 / 10);
            TrySetUInt32(codecApi.Get(), CODECAPI_AVEncCommonMaxBitRate, bitrateBps);
            TrySetUInt32(codecApi.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
            TrySetBool(codecApi.Get(), CODECAPI_AVLowLatencyMode, true);
            // Without an explicit GOP size some MFTs (observed with NVENC here)
            // default to all-intra. 2 seconds between forced IDRs is a normal
            // streaming default; RequestKeyFrame() still forces one early on
            // demand (spec "kf").
            TrySetUInt32(codecApi.Get(), CODECAPI_AVEncMPVGOPSize, fps * 2);
        }

        mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        if (!ProbeFirstFrame()) {
            ResetMft();
            act->ShutdownObject();
            return false;
        }

        Logf("encoder", "MFT bound: %s (%s)\n", NarrowAscii(name).c_str(), isAsync ? "async" : "sync");
        return true;
    }


    // Feed one black NV12 frame. Async MFTs that never signal NeedInput fail
    // the probe so Configure can try the next activate instead of hanging the UI.
    bool ProbeFirstFrame()
    {
        asyncTimedOut = false;
        pendingNeedInput = false;

        const DWORD expected = width * height * 3 / 2;
        auto make_sample = [&](LONGLONG pts) -> ComPtr<IMFSample> {
            std::vector<uint8_t> nv12(expected, 0);
            std::fill(nv12.begin(), nv12.begin() + static_cast<std::ptrdiff_t>(width * height),
                      static_cast<uint8_t>(0x10));
            std::fill(nv12.begin() + static_cast<std::ptrdiff_t>(width * height), nv12.end(),
                      static_cast<uint8_t>(0x80));

            ComPtr<IMFMediaBuffer> buffer;
            MFCreateMemoryBuffer(expected, &buffer);
            BYTE* dst = nullptr;
            buffer->Lock(&dst, nullptr, nullptr);
            memcpy(dst, nv12.data(), expected);
            buffer->Unlock();
            buffer->SetCurrentLength(expected);

            ComPtr<IMFSample> sample;
            MFCreateSample(&sample);
            sample->AddBuffer(buffer.Get());
            sample->SetSampleTime(pts);
            sample->SetSampleDuration(frameDuration100ns > 0 ? frameDuration100ns : 333333);
            return sample;
        };

        std::vector<EncodedFrame> out;
        const LONGLONG dur = frameDuration100ns > 0 ? frameDuration100ns : 333333;

        // Sync software MFTs often buffer the first frame and return 0 AU from
        // a single ProcessInput/Drain. Requiring an AU there rejected Microsoft
        // software and caused a connect/configure/disconnect flash loop.
        // Async HW (QSV) must produce HaveOutput / an AU or we fail the bind.
        if (!isAsync) {
            ComPtr<IMFSample> sample = make_sample(0);
            HRESULT hr = mft->ProcessInput(0, sample.Get(), 0);
            if (FAILED(hr)) {
                Logf("encoder", "MFT probe ProcessInput failed: %s hr=0x%08lX\n",
                     NarrowAscii(mftName).c_str(), static_cast<unsigned long>(hr));
                return false;
            }
            DrainSync(out);
            // Optional second IDR feed — still OK if 0 AU.
            if (out.empty()) {
                TrySetUInt32(codecApi.Get(), CODECAPI_AVEncVideoForceKeyFrame, TRUE);
                sample = make_sample(dur);
                if (SUCCEEDED(mft->ProcessInput(0, sample.Get(), 0)))
                    DrainSync(out);
            }
            Logf("encoder", "MFT probe ok (sync): %s (%zu au)\n", NarrowAscii(mftName).c_str(),
                 out.size());
            return true;
        }

        auto feed = [&](LONGLONG pts) {
            ComPtr<IMFSample> sample = make_sample(pts);
            PumpAsync(sample.Get(), out);
        };

        feed(0);
        if (asyncTimedOut) {
            Logf("encoder", "MFT probe timed out (NeedInput): %s\n", NarrowAscii(mftName).c_str());
            return false;
        }
        if (out.empty()) {
            const DWORD start = GetTickCount();
            while (out.empty() && GetTickCount() - start < 500) {
                DrainAvailableAsync(out);
                if (out.empty())
                    Sleep(1);
            }
        }
        if (out.empty()) {
            TrySetUInt32(codecApi.Get(), CODECAPI_AVEncVideoForceKeyFrame, TRUE);
            feed(dur);
            if (asyncTimedOut) {
                Logf("encoder", "MFT probe timed out on 2nd frame: %s\n", NarrowAscii(mftName).c_str());
                return false;
            }
            if (out.empty()) {
                const DWORD start = GetTickCount();
                while (out.empty() && GetTickCount() - start < 500) {
                    DrainAvailableAsync(out);
                    if (out.empty())
                        Sleep(1);
                }
            }
        }
        if (out.empty()) {
            Logf("encoder", "MFT probe produced no AU: %s\n", NarrowAscii(mftName).c_str());
            return false;
        }

        Logf("encoder", "MFT probe ok: %s (%zu au)\n", NarrowAscii(mftName).c_str(), out.size());
        return true;
    }

    bool EnumAndBind(bool hardware, uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrateBps)
    {
        MFT_REGISTER_TYPE_INFO outputInfo = {MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** activates = nullptr;
        UINT32 count = 0;

        UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER;
        flags |= hardware ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT) : MFT_ENUM_FLAG_SYNCMFT;

        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outputInfo, &activates, &count);
        if (FAILED(hr) || count == 0) {
            Logf("encoder", "MFTEnumEx(%s) failed: hr=0x%08lX count=%u\n", hardware ? "hardware" : "software",
                 static_cast<unsigned long>(hr), count);
            if (activates)
                CoTaskMemFree(activates);
            return false;
        }

        struct Candidate {
            IMFActivate* act = nullptr;
            std::wstring name;
            int preference = 99;
            UINT32 index = 0;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(count);
        for (UINT32 i = 0; i < count; ++i) {
            Candidate c;
            c.act = activates[i];
            c.name = FriendlyName(activates[i]);
            c.preference = hardware ? HwPreference(c.name) : 0;
            c.index = i;
            candidates.push_back(c);
        }
        if (hardware) {
            std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
                if (a.preference != b.preference)
                    return a.preference < b.preference;
                return a.index < b.index;
            });
        }

        Logf("encoder", "MFTEnumEx(%s): %u candidate(s)\n", hardware ? "hardware" : "software", count);
        for (const auto& c : candidates)
            Logf("encoder", "  [%u] pref=%d %s\n", c.index, c.preference, NarrowAscii(c.name).c_str());

        bool ok = false;
        for (const auto& c : candidates) {
            if (TryBindActivate(c.act, c.name, width, height, fps, bitrateBps)) {
                ok = true;
                break;
            }
        }

        for (UINT32 i = 0; i < count; ++i)
            activates[i]->Release();
        CoTaskMemFree(activates);
        return ok;
    }

    void RefreshOutputStreamInfo()
    {
        MFT_OUTPUT_STREAM_INFO info = {};
        if (SUCCEEDED(mft->GetOutputStreamInfo(0, &info))) {
            providesSamples =
                (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            outputBufferSize = info.cbSize > 0 ? info.cbSize : (width * height * 2);
        }
    }

    void ExtractFrame(IMFSample* sample, std::vector<EncodedFrame>& out)
    {
        if (!sample) return;

        ComPtr<IMFMediaBuffer> contiguous;
        if (FAILED(sample->ConvertToContiguousBuffer(&contiguous)))
            return;

        BYTE* data = nullptr;
        DWORD len = 0;
        if (FAILED(contiguous->Lock(&data, nullptr, &len)))
            return;

        auto nals = ScanStartCodes(data, len);
        contiguous->Unlock();

        if (nals.empty())
            return;

        auto fixedNals = spsPpsCache.EnsureParameterSets(nals);

        bool isKeyFrame = false;
        for (const auto& n : fixedNals) {
            if (n.type == kNalTypeIdrSlice) {
                isKeyFrame = true;
                break;
            }
        }

        EncodedFrame frame;
        frame.annexB = BuildAccessUnit(fixedNals);
        frame.isKeyFrame = isKeyFrame;
        out.push_back(std::move(frame));
    }

    // Returns true if an access unit was produced.
    bool ProcessOutputOnce(std::vector<EncodedFrame>& out)
    {
        MFT_OUTPUT_DATA_BUFFER outputBuf = {};
        ComPtr<IMFSample> sample;

        if (!providesSamples) {
            ComPtr<IMFMediaBuffer> buffer;
            MFCreateSample(&sample);
            MFCreateMemoryBuffer(outputBufferSize, &buffer);
            sample->AddBuffer(buffer.Get());
            outputBuf.pSample = sample.Get();
        }

        DWORD status = 0;
        HRESULT hr = mft->ProcessOutput(0, 1, &outputBuf, &status);

        if (outputBuf.pEvents) {
            outputBuf.pEvents->Release();
            outputBuf.pEvents = nullptr;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
            return false;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            RefreshOutputStreamInfo();
            return false;
        }
        if (FAILED(hr))
            return false;

        if (providesSamples)
            sample.Attach(outputBuf.pSample); // ProcessOutput transferred us this reference

        ExtractFrame(sample.Get(), out);
        return true;
    }

    void DrainAvailableAsync(std::vector<EncodedFrame>& out)
    {
        for (;;) {
            ComPtr<IMFMediaEvent> event;
            HRESULT hr = eventGen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE || FAILED(hr))
                return;

            MediaEventType met = MEUnknown;
            event->GetType(&met);

            if (met == METransformHaveOutput) {
                ProcessOutputOnce(out);
            } else if (met == METransformNeedInput) {
                pendingNeedInput = true;
                return; // consumed lazily at the top of the next PumpAsync()
            }
        }
    }

    void PumpAsync(IMFSample* sample, std::vector<EncodedFrame>& out)
    {
        DrainAvailableAsync(out);

        if (!pendingNeedInput) {
            // Never block forever on GetEvent(0): Intel QSV has been observed to
            // never signal NeedInput when capture is on another adapter, which
            // freezes the whole sender (tray UI included).
            const DWORD kNeedInputTimeoutMs = 500;
            const DWORD start = GetTickCount();
            for (;;) {
                ComPtr<IMFMediaEvent> event;
                HRESULT hr = eventGen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
                if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                    if (GetTickCount() - start >= kNeedInputTimeoutMs) {
                        asyncTimedOut = true;
                        Logf("encoder", "async MFT timed out waiting for NeedInput (%lu ms)\n",
                             static_cast<unsigned long>(kNeedInputTimeoutMs));
                        return;
                    }
                    Sleep(1);
                    continue;
                }
                if (FAILED(hr))
                    return;

                MediaEventType met = MEUnknown;
                event->GetType(&met);

                if (met == METransformHaveOutput)
                    ProcessOutputOnce(out);
                else if (met == METransformNeedInput)
                    break;
            }
        }
        pendingNeedInput = false;

        HRESULT phr = mft->ProcessInput(0, sample, 0);
        if (FAILED(phr)) {
            Logf("encoder", "async ProcessInput failed hr=0x%08lX\n", static_cast<unsigned long>(phr));
            return;
        }

        // QSV often posts HaveOutput asynchronously. A single NO_WAIT drain can
        // leave the AU queued and the next frame never sees NeedInput (black
        // video). Poll briefly for output / the next NeedInput.
        {
            const DWORD kOutputTimeoutMs = 500;
            const DWORD start = GetTickCount();
            bool sawOutputOrNeed = false;
            while (GetTickCount() - start < kOutputTimeoutMs) {
                DrainAvailableAsync(out);
                if (pendingNeedInput || !out.empty()) {
                    sawOutputOrNeed = true;
                    break;
                }
                ComPtr<IMFMediaEvent> event;
                HRESULT hr = eventGen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
                if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                    Sleep(1);
                    continue;
                }
                if (FAILED(hr))
                    break;
                MediaEventType met = MEUnknown;
                event->GetType(&met);
                if (met == METransformHaveOutput) {
                    ProcessOutputOnce(out);
                    sawOutputOrNeed = true;
                } else if (met == METransformNeedInput) {
                    pendingNeedInput = true;
                    sawOutputOrNeed = true;
                    break;
                }
            }
            if (!sawOutputOrNeed && out.empty()) {
                asyncTimedOut = true;
                Logf("encoder", "async MFT timed out waiting for HaveOutput (%lu ms)\n",
                     static_cast<unsigned long>(kOutputTimeoutMs));
            }
        }
    }


    void DrainSync(std::vector<EncodedFrame>& out)
    {
        while (ProcessOutputOnce(out)) {
        }
    }
};

H264Encoder::H264Encoder() : impl_(std::make_unique<Impl>())
{
    impl_->comInitResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
}

H264Encoder::~H264Encoder()
{
    if (impl_->mft) {
        impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        impl_->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
    impl_->mft.Reset();
    impl_->eventGen.Reset();

    MFShutdown();
    if (impl_->comInitResult == S_OK || impl_->comInitResult == S_FALSE)
        CoUninitialize();
}

bool H264Encoder::Configure(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrateBps)
{
    impl_->configured = false;
    impl_->width = width;
    impl_->height = height;
    impl_->fps = fps;
    impl_->frameDuration100ns = 10'000'000LL / fps;
    impl_->timestamp = 0;
    impl_->pendingNeedInput = false;
    impl_->spsPpsCache = SpsPpsCache{};
    impl_->ResetMft();

    // Walk every hardware activate (NVIDIA first), then software. Previously we
    // only ActivateObject'd activates[0]; one bad first entry dropped us on the
    // Microsoft software MFT with no HRESULT in the log.
    impl_->consecutiveAsyncTimeouts = 0;
    impl_->lastAsyncTimedOut = false;

    bool ok = false;
    if (impl_->allowHardware)
        ok = impl_->EnumAndBind(/*hardware=*/true, width, height, fps, bitrateBps);
    if (!ok)
        ok = impl_->EnumAndBind(/*hardware=*/false, width, height, fps, bitrateBps);
    if (!ok) {
        Logf("encoder", "no H.264 MFT could be configured for %ux%u @ %u\n", width, height, fps);
        return false;
    }

    impl_->configured = true;
    return true;
}

std::vector<EncodedFrame> H264Encoder::EncodeNv12(const uint8_t* nv12, size_t size)
{
    std::vector<EncodedFrame> outFrames;
    impl_->lastAsyncTimedOut = false;
    if (!impl_->configured)
        return outFrames;

    DWORD expected = impl_->width * impl_->height * 3 / 2;
    if (size < expected)
        return outFrames;

    ComPtr<IMFMediaBuffer> buffer;
    MFCreateMemoryBuffer(expected, &buffer);
    BYTE* dst = nullptr;
    buffer->Lock(&dst, nullptr, nullptr);
    memcpy(dst, nv12, expected);
    buffer->Unlock();
    buffer->SetCurrentLength(expected);

    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(impl_->timestamp);
    sample->SetSampleDuration(impl_->frameDuration100ns);
    impl_->timestamp += impl_->frameDuration100ns;

    if (impl_->forceKeyFrame.exchange(false)) {
        TrySetUInt32(impl_->codecApi.Get(), CODECAPI_AVEncVideoForceKeyFrame, TRUE);
    }

    if (impl_->isAsync) {
        impl_->asyncTimedOut = false;
        impl_->PumpAsync(sample.Get(), outFrames);
        if (impl_->asyncTimedOut) {
            impl_->lastAsyncTimedOut = true;
            ++impl_->consecutiveAsyncTimeouts;
        } else {
            impl_->consecutiveAsyncTimeouts = 0;
        }
    } else {
        if (SUCCEEDED(impl_->mft->ProcessInput(0, sample.Get(), 0)))
            impl_->DrainSync(outFrames);
        impl_->consecutiveAsyncTimeouts = 0;
    }

    return outFrames;
}

void H264Encoder::RequestKeyFrame()
{
    impl_->forceKeyFrame = true;
}

bool H264Encoder::IsConfigured() const
{
    return impl_->configured;
}

uint32_t H264Encoder::Width() const
{
    return impl_->width;
}

uint32_t H264Encoder::Height() const
{
    return impl_->height;
}


std::wstring H264Encoder::MftName() const
{
    return impl_ ? impl_->mftName : std::wstring{};
}

bool H264Encoder::TookAsyncTimeout() const
{
    return impl_ && impl_->lastAsyncTimedOut;
}

int H264Encoder::ConsecutiveAsyncTimeouts() const
{
    return impl_ ? impl_->consecutiveAsyncTimeouts : 0;
}

void H264Encoder::SetAllowHardware(bool allow)
{
    if (impl_)
        impl_->allowHardware = allow;
}

} // namespace od
