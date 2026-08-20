#include "encode/H264Encoder.h"
#include "encode/AnnexB.h"

#include <atomic>
#include <codecapi.h>
#include <icodecapi.h>
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
    ComPtr<IMFMediaEventGenerator> eventGen;
    ComPtr<ICodecAPI> codecApi;
    bool isAsync = false;
    bool pendingNeedInput = false;
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

    bool CreateTransform(bool hardware)
    {
        MFT_REGISTER_TYPE_INFO outputInfo = {MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** activates = nullptr;
        UINT32 count = 0;

        UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER;
        flags |= hardware ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT) : MFT_ENUM_FLAG_SYNCMFT;

        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outputInfo, &activates, &count);
        if (FAILED(hr) || count == 0) {
            if (activates) CoTaskMemFree(activates);
            return false;
        }

        mft.Reset();
        hr = activates[0]->ActivateObject(IID_PPV_ARGS(mft.ReleaseAndGetAddressOf()));
        for (UINT32 i = 0; i < count; ++i)
            activates[i]->Release();
        CoTaskMemFree(activates);

        return SUCCEEDED(hr) && mft;
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
            for (;;) {
                ComPtr<IMFMediaEvent> event;
                if (FAILED(eventGen->GetEvent(0, &event)))
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

        mft->ProcessInput(0, sample, 0);

        DrainAvailableAsync(out);
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

    if (!impl_->CreateTransform(/*hardware=*/true) && !impl_->CreateTransform(/*hardware=*/false))
        return false;

    ComPtr<IMFAttributes> attrs;
    impl_->isAsync = false;
    if (SUCCEEDED(impl_->mft->GetAttributes(&attrs))) {
        UINT32 async = 0;
        attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async);
        impl_->isAsync = (async != 0);
        if (impl_->isAsync) {
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            impl_->mft.As(&impl_->eventGen);
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

    if (FAILED(impl_->mft->SetOutputType(0, outType.Get(), 0)))
        return false;

    ComPtr<IMFMediaType> inType;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    if (FAILED(impl_->mft->SetInputType(0, inType.Get(), 0)))
        return false;

    impl_->RefreshOutputStreamInfo();

    impl_->codecApi.Reset();
    if (SUCCEEDED(impl_->mft.As(&impl_->codecApi))) {
        TrySetUInt32(impl_->codecApi.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        TrySetUInt32(impl_->codecApi.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrateBps);
        TrySetUInt32(impl_->codecApi.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        TrySetBool(impl_->codecApi.Get(), CODECAPI_AVLowLatencyMode, true);
        // Without an explicit GOP size some MFTs (observed with NVENC here)
        // default to all-intra, i.e. every frame an IDR — correct but wastes
        // a huge amount of bandwidth. 2 seconds between forced IDRs is a
        // normal streaming default; RequestKeyFrame() still forces one early
        // on demand (spec "kf").
        TrySetUInt32(impl_->codecApi.Get(), CODECAPI_AVEncMPVGOPSize, fps * 2);
    }

    impl_->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    impl_->configured = true;
    return true;
}

std::vector<EncodedFrame> H264Encoder::EncodeNv12(const uint8_t* nv12, size_t size)
{
    std::vector<EncodedFrame> outFrames;
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
        impl_->PumpAsync(sample.Get(), outFrames);
    } else {
        if (SUCCEEDED(impl_->mft->ProcessInput(0, sample.Get(), 0)))
            impl_->DrainSync(outFrames);
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

} // namespace od
