// Standalone H.264 MFT probe — no Parsec, network, or tray.
// Usage:
//   mft_encoder_probe.exe [width height fps] [--sysmem|--d3d] [--frames N] [--hw-only|--sw-only]
// Exit 0 iff at least one MFT produced >=1 access unit.

#include <windows.h>
#include <d3d11.h>
#include <d3d10_1.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <cwctype>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    UINT width = 1920;
    UINT height = 1088;
    UINT fps = 30;
    UINT frames = 30;
    bool useD3d = false;
    bool hw = true;
    bool sw = true;
};

void PrintUsage()
{
    std::printf(
        "mft_encoder_probe [width height fps] [--sysmem|--d3d] [--frames N] [--hw-only|--sw-only]\n"
        "  Default: 1920 1088 30 --sysmem --frames 30 (HW then software)\n");
}

std::string Narrow(const std::wstring& w)
{
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w)
        s.push_back(c >= 32 && c < 127 ? static_cast<char>(c) : '?');
    return s;
}

std::wstring FriendlyName(IMFActivate* act)
{
    WCHAR* friendly = nullptr;
    UINT32 len = 0;
    if (SUCCEEDED(act->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &len)) && friendly) {
        std::wstring name(friendly);
        CoTaskMemFree(friendly);
        return name;
    }
    return L"(unnamed)";
}

bool CreateIntelDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& ctx)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        std::wstring name(desc.Description);
        std::wstring lower = name;
        for (auto& c : lower)
            c = static_cast<wchar_t>(towlower(c));
        const bool intel = lower.find(L"intel") != std::wstring::npos;
        std::printf("  adapter[%u]: %s%s\n", i, Narrow(name).c_str(), intel ? "  <-- pick" : "");
        if (!intel)
            continue;
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                       D3D11_SDK_VERSION, &device, &level, &ctx);
        if (FAILED(hr)) {
            std::printf("  D3D11CreateDevice(Intel) failed hr=0x%08lX\n", static_cast<unsigned long>(hr));
            continue;
        }
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device.As(&mt)))
            mt->SetMultithreadProtected(TRUE);
        return true;
    }
    return false;
}

bool MakeSysmemNv12Sample(UINT width, UINT height, LONGLONG pts, LONGLONG dur, ComPtr<IMFSample>& sample)
{
    const DWORD expected = width * height * 3 / 2;
    std::vector<uint8_t> nv12(expected, 0);
    std::fill(nv12.begin(), nv12.begin() + static_cast<std::ptrdiff_t>(width * height), uint8_t{0x10});
    std::fill(nv12.begin() + static_cast<std::ptrdiff_t>(width * height), nv12.end(), uint8_t{0x80});
    // Changing luma pattern so the encoder sees motion.
    const UINT stripe = static_cast<UINT>((pts / (dur > 0 ? dur : 1)) % height);
    if (stripe < height)
        std::fill(nv12.begin() + static_cast<std::ptrdiff_t>(stripe) * width,
                  nv12.begin() + static_cast<std::ptrdiff_t>(stripe + 1) * width, uint8_t{0xEB});

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(expected, &buffer)))
        return false;
    BYTE* dst = nullptr;
    buffer->Lock(&dst, nullptr, nullptr);
    memcpy(dst, nv12.data(), expected);
    buffer->Unlock();
    buffer->SetCurrentLength(expected);

    if (FAILED(MFCreateSample(&sample)))
        return false;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(pts);
    sample->SetSampleDuration(dur);
    return true;
}

bool MakeDxgiNv12Sample(ID3D11Device* device, ID3D11DeviceContext* ctx, UINT width, UINT height,
                        LONGLONG pts, LONGLONG dur, ComPtr<IMFSample>& sample)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    // Staging upload with a simple Y/UV pattern.
    D3D11_TEXTURE2D_DESC stagingDesc = td;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)))
        return false;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &tex)))
        return false;

    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &map)))
        return false;
    auto* row = static_cast<uint8_t*>(map.pData);
    for (UINT y = 0; y < height; ++y) {
        memset(row + y * map.RowPitch, (y == (pts / (dur > 0 ? dur : 1)) % height) ? 0xEB : 0x10, width);
    }
    uint8_t* uv = row + height * map.RowPitch;
    for (UINT y = 0; y < height / 2; ++y)
        memset(uv + y * map.RowPitch, 0x80, width);
    ctx->Unmap(staging.Get(), 0);
    ctx->CopyResource(tex.Get(), staging.Get());

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex.Get(), 0, FALSE, &buffer)))
        return false;
    if (FAILED(MFCreateSample(&sample)))
        return false;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(pts);
    sample->SetSampleDuration(dur);
    return true;
}

struct ProbeResult {
    std::wstring name;
    bool hardware = false;
    HRESULT activateHr = E_FAIL;
    HRESULT d3dHr = S_OK;
    HRESULT setOutHr = S_OK;
    HRESULT setInHr = S_OK;
    bool isAsync = false;
    int needInput = 0;
    int haveOutput = 0;
    int auCount = 0;
    size_t auBytes = 0;
    int timeouts = 0;
    std::string note;
};

bool ProcessOutputOnce(IMFTransform* mft, bool providesSamples, DWORD outputBufferSize,
                       int& auCount, size_t& auBytes)
{
    MFT_OUTPUT_DATA_BUFFER outBuf{};
    ComPtr<IMFSample> sample;
    if (!providesSamples) {
        ComPtr<IMFMediaBuffer> buffer;
        MFCreateSample(&sample);
        MFCreateMemoryBuffer(outputBufferSize, &buffer);
        sample->AddBuffer(buffer.Get());
        outBuf.pSample = sample.Get();
    }
    DWORD status = 0;
    HRESULT hr = mft->ProcessOutput(0, 1, &outBuf, &status);
    if (outBuf.pEvents) {
        outBuf.pEvents->Release();
        outBuf.pEvents = nullptr;
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT || FAILED(hr))
        return false;
    if (providesSamples)
        sample.Attach(outBuf.pSample);
    if (!sample)
        return false;
    ComPtr<IMFMediaBuffer> contiguous;
    if (FAILED(sample->ConvertToContiguousBuffer(&contiguous)))
        return false;
    BYTE* data = nullptr;
    DWORD len = 0;
    if (FAILED(contiguous->Lock(&data, nullptr, &len)))
        return false;
    contiguous->Unlock();
    ++auCount;
    auBytes += len;
    return true;
}

ProbeResult ProbeOne(IMFActivate* act, bool hardware, const Options& opt,
                     ID3D11Device* device, ID3D11DeviceContext* ctx,
                     IMFDXGIDeviceManager* dxgiManager)
{
    ProbeResult r;
    r.name = FriendlyName(act);
    r.hardware = hardware;

    ComPtr<IMFTransform> mft;
    r.activateHr = act->ActivateObject(IID_PPV_ARGS(&mft));
    if (FAILED(r.activateHr) || !mft) {
        r.note = "activate failed";
        act->ShutdownObject();
        return r;
    }

    ComPtr<IMFAttributes> attrs;
    ComPtr<IMFMediaEventGenerator> eventGen;
    if (SUCCEEDED(mft->GetAttributes(&attrs)) && attrs) {
        UINT32 async = 0;
        attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async);
        r.isAsync = async != 0;
        if (r.isAsync) {
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            mft.As(&eventGen);
        }
    }

    if (opt.useD3d && dxgiManager) {
        UINT32 aware = 0;
        if (attrs)
            attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware);
        if (aware) {
            r.d3dHr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                          reinterpret_cast<ULONG_PTR>(dxgiManager));
            if (FAILED(r.d3dHr)) {
                r.note = "SET_D3D_MANAGER failed";
                mft.Reset();
                act->ShutdownObject();
                return r;
            }
        } else {
            r.note = "not D3D11_AWARE; feeding DXGI anyway skipped → sysmem";
        }
    }

    const UINT32 bitrate = 10'000'000;
    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, opt.width, opt.height);
    MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, opt.fps, 1);
    MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    r.setOutHr = mft->SetOutputType(0, outType.Get(), 0);
    if (FAILED(r.setOutHr)) {
        r.note = "SetOutputType failed";
        mft.Reset();
        act->ShutdownObject();
        return r;
    }

    ComPtr<IMFMediaType> inType;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, opt.width, opt.height);
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, opt.fps, 1);
    MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    r.setInHr = mft->SetInputType(0, inType.Get(), 0);
    if (FAILED(r.setInHr)) {
        r.note = "SetInputType failed";
        mft.Reset();
        act->ShutdownObject();
        return r;
    }

    bool providesSamples = false;
    DWORD outputBufferSize = opt.width * opt.height * 2;
    MFT_OUTPUT_STREAM_INFO info{};
    if (SUCCEEDED(mft->GetOutputStreamInfo(0, &info))) {
        providesSamples =
            (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        if (info.cbSize > 0)
            outputBufferSize = info.cbSize;
    }

    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(mft.As(&codecApi)) && codecApi) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = eAVEncCommonRateControlMode_PeakConstrainedVBR;
        codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        v.ulVal = bitrate * 6 / 10;
        codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
        v.ulVal = bitrate;
        codecApi->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &v);
        v.ulVal = 0;
        codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
        v.vt = VT_BOOL;
        v.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        v.vt = VT_UI4;
        v.ulVal = opt.fps * 2;
        codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
        v.ulVal = TRUE;
        codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
    }

    mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    const LONGLONG dur = 10'000'000LL / opt.fps;
    const bool wantDxgi = opt.useD3d && device && ctx && r.note.find("not D3D11_AWARE") == std::string::npos;

    auto feed_one = [&](UINT i) -> bool {
        ComPtr<IMFSample> sample;
        const LONGLONG pts = static_cast<LONGLONG>(i) * dur;
        if (wantDxgi) {
            if (!MakeDxgiNv12Sample(device, ctx, opt.width, opt.height, pts, dur, sample))
                return false;
        } else {
            if (!MakeSysmemNv12Sample(opt.width, opt.height, pts, dur, sample))
                return false;
        }

        if (!r.isAsync || !eventGen) {
            HRESULT phr = mft->ProcessInput(0, sample.Get(), 0);
            if (FAILED(phr))
                return false;
            while (ProcessOutputOnce(mft.Get(), providesSamples, outputBufferSize, r.auCount, r.auBytes)) {
            }
            return true;
        }

        // Async: wait NeedInput -> ProcessInput -> wait HaveOutput (NeedInput alone != success).
        bool pendingNeed = false;
        auto drain = [&](DWORD timeoutMs, bool requireOutput) {
            const DWORD start = GetTickCount();
            bool gotOut = false;
            while (GetTickCount() - start < timeoutMs) {
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
                if (met == METransformNeedInput) {
                    ++r.needInput;
                    pendingNeed = true;
                    if (!requireOutput)
                        return true;
                } else if (met == METransformHaveOutput) {
                    ++r.haveOutput;
                    if (ProcessOutputOnce(mft.Get(), providesSamples, outputBufferSize, r.auCount, r.auBytes))
                        gotOut = true;
                    if (requireOutput && gotOut)
                        return true;
                }
            }
            if (requireOutput && !gotOut) {
                ++r.timeouts;
                return false;
            }
            return pendingNeed || gotOut;
        };

        if (!pendingNeed) {
            if (!drain(500, false) || !pendingNeed) {
                ++r.timeouts;
                return false;
            }
        }
        pendingNeed = false;
        HRESULT phr = mft->ProcessInput(0, sample.Get(), 0);
        if (FAILED(phr))
            return false;
        drain(800, true);
        return true;
    };

    for (UINT i = 0; i < opt.frames; ++i)
        feed_one(i);

    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (r.isAsync && eventGen) {
        const DWORD start = GetTickCount();
        while (GetTickCount() - start < 500) {
            ComPtr<IMFMediaEvent> event;
            if (FAILED(eventGen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event)) || !event)
                break;
            MediaEventType met = MEUnknown;
            event->GetType(&met);
            if (met == METransformHaveOutput) {
                ++r.haveOutput;
                ProcessOutputOnce(mft.Get(), providesSamples, outputBufferSize, r.auCount, r.auBytes);
            }
        }
    } else {
        while (ProcessOutputOnce(mft.Get(), providesSamples, outputBufferSize, r.auCount, r.auBytes)) {
        }
    }

    mft.Reset();
    act->ShutdownObject();
    return r;
}

void EnumAndProbe(bool hardware, const Options& opt, ID3D11Device* device, ID3D11DeviceContext* ctx,
                  IMFDXGIDeviceManager* dxgiManager, bool& anyAu)
{
    MFT_REGISTER_TYPE_INFO outputInfo{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER;
    flags |= hardware ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT) : MFT_ENUM_FLAG_SYNCMFT;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outputInfo, &activates, &count);
    std::printf("\n=== MFTEnumEx(%s) hr=0x%08lX count=%u ===\n", hardware ? "hardware" : "software",
                static_cast<unsigned long>(hr), count);
    if (FAILED(hr) || !activates)
        return;
    for (UINT32 i = 0; i < count; ++i) {
        ProbeResult r = ProbeOne(activates[i], hardware, opt, device, ctx, dxgiManager);
        std::printf(
            "[%u] %s (%s)\n"
            "     activate=0x%08lX d3d=0x%08lX setOut=0x%08lX setIn=0x%08lX async=%d\n"
            "     NeedInput=%d HaveOutput=%d AU=%d bytes=%zu timeouts=%d%s%s\n",
            i, Narrow(r.name).c_str(), hardware ? "hw" : "sw",
            static_cast<unsigned long>(r.activateHr), static_cast<unsigned long>(r.d3dHr),
            static_cast<unsigned long>(r.setOutHr), static_cast<unsigned long>(r.setInHr),
            r.isAsync ? 1 : 0, r.needInput, r.haveOutput, r.auCount, r.auBytes, r.timeouts,
            r.note.empty() ? "" : "  note=", r.note.c_str());
        if (r.auCount > 0)
            anyAu = true;
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--sysmem")
            opt.useD3d = false;
        else if (a == "--d3d")
            opt.useD3d = true;
        else if (a == "--hw-only") {
            opt.hw = true;
            opt.sw = false;
        } else if (a == "--sw-only") {
            opt.hw = false;
            opt.sw = true;
        } else if (a == "--frames" && i + 1 < argc)
            opt.frames = static_cast<UINT>(std::atoi(argv[++i]));
        else if (a == "-h" || a == "--help") {
            PrintUsage();
            return 2;
        } else if (!a.empty() && a[0] == '-') {
            std::printf("unknown flag: %s\n", a.c_str());
            PrintUsage();
            return 2;
        } else {
            pos.push_back(a);
        }
    }
    if (pos.size() >= 1)
        opt.width = static_cast<UINT>(std::atoi(pos[0].c_str()));
    if (pos.size() >= 2)
        opt.height = static_cast<UINT>(std::atoi(pos[1].c_str()));
    if (pos.size() >= 3)
        opt.fps = static_cast<UINT>(std::atoi(pos[2].c_str()));
    if (opt.width == 0 || opt.height == 0 || opt.fps == 0 || opt.frames == 0) {
        PrintUsage();
        return 2;
    }

    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    std::printf("mft_encoder_probe %ux%u @ %u fps, frames=%u, mode=%s\n", opt.width, opt.height, opt.fps,
                opt.frames, opt.useD3d ? "d3d/DXGI NV12" : "sysmem NV12");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IMFDXGIDeviceManager> dxgiManager;
    UINT resetToken = 0;
    if (opt.useD3d) {
        std::printf("DXGI adapters:\n");
        if (!CreateIntelDevice(device, ctx)) {
            std::printf("ERROR: no Intel D3D11 device\n");
            MFShutdown();
            if (SUCCEEDED(com))
                CoUninitialize();
            return 1;
        }
        HRESULT hr = MFCreateDXGIDeviceManager(&resetToken, &dxgiManager);
        if (FAILED(hr) || FAILED(dxgiManager->ResetDevice(device.Get(), resetToken))) {
            std::printf("ERROR: DXGI device manager hr=0x%08lX\n", static_cast<unsigned long>(hr));
            MFShutdown();
            if (SUCCEEDED(com))
                CoUninitialize();
            return 1;
        }
        std::printf("DXGI device manager ready\n");
    }

    bool anyAu = false;
    if (opt.hw)
        EnumAndProbe(true, opt, device.Get(), ctx.Get(), dxgiManager.Get(), anyAu);
    if (opt.sw)
        EnumAndProbe(false, opt, device.Get(), ctx.Get(), dxgiManager.Get(), anyAu);

    std::printf("\nRESULT: %s\n", anyAu ? "PASS (>=1 AU)" : "FAIL (no AU)");
    MFShutdown();
    if (SUCCEEDED(com))
        CoUninitialize();
    return anyAu ? 0 : 1;
}
