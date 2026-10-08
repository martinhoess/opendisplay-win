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
#include <d3d11.h>
#include <dxgi.h>
#include <d3d10_1.h>
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
    uint32_t qualityVsSpeed = 0; // MS: 0=faster, 100=slower
    uint32_t gopSeconds = 2;
    bool useQualityRc = false;
    uint32_t rcQuality = 90;
    int consecutiveAsyncTimeouts = 0;
    bool allowHardware = true;
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ComPtr<IMFDXGIDeviceManager> dxgiManager;
    UINT dxgiResetToken = 0;
    ComPtr<ID3D11Texture2D> nv12Tex;
    ComPtr<ID3D11Texture2D> nv12Staging;
    bool useDxgiInput = false; // true after SET_D3D_MANAGER on a D3D11-aware MFT
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

    // Lower preference number = try first. Prefer Intel QSV on the DXGI capture
    // device (MTT / Optimus), then other hardware MFTs, then software.
    static int HwPreference(const std::wstring& name)
    {
        std::wstring lower = name;
        for (auto& c : lower)
            c = static_cast<wchar_t>(towlower(c));
        auto has = [&](const wchar_t* sub) { return lower.find(sub) != std::wstring::npos; };
        if (has(L"intel") || has(L"quick sync") || has(L"qsv"))
            return 0;
        if (has(L"nvidia") || has(L"nvenc"))
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
        useDxgiInput = false;
        mftName.clear();
    }


    static bool IsIntelDevice(ID3D11Device* device)
    {
        if (!device)
            return false;
        ComPtr<IDXGIDevice> dxgiDev;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDev))) || !dxgiDev)
            return false;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDev->GetAdapter(&adapter)) || !adapter)
            return false;
        DXGI_ADAPTER_DESC desc{};
        if (FAILED(adapter->GetDesc(&desc)))
            return false;
        // 0x8086 = Intel. Also match description as a belt-and-suspenders check.
        if (desc.VendorId == 0x8086)
            return true;
        std::wstring name(desc.Description);
        for (auto& ch : name)
            ch = static_cast<wchar_t>(towlower(ch));
        return name.find(L"intel") != std::wstring::npos;
    }

    bool AdoptD3DDevice(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        if (!device || !context || !IsIntelDevice(device))
            return false;

        // Already adopted this exact device+context.
        if (d3dDevice.Get() == device && d3dContext.Get() == context && dxgiManager)
            return true;

        d3dDevice.Reset();
        d3dContext.Reset();
        nv12Tex.Reset();
        nv12Staging.Reset();
        dxgiManager.Reset();
        dxgiResetToken = 0;
        useDxgiInput = false;

        d3dDevice = device;
        d3dContext = context;

        HRESULT hr = MFCreateDXGIDeviceManager(&dxgiResetToken, &dxgiManager);
        if (FAILED(hr) || !dxgiManager) {
            Logf("encoder", "AdoptD3DDevice: MFCreateDXGIDeviceManager failed hr=0x%08lX\n",
                 static_cast<unsigned long>(hr));
            d3dDevice.Reset();
            d3dContext.Reset();
            dxgiManager.Reset();
            return false;
        }
        hr = dxgiManager->ResetDevice(d3dDevice.Get(), dxgiResetToken);
        if (FAILED(hr)) {
            Logf("encoder", "AdoptD3DDevice: ResetDevice failed hr=0x%08lX\n",
                 static_cast<unsigned long>(hr));
            d3dDevice.Reset();
            d3dContext.Reset();
            dxgiManager.Reset();
            return false;
        }

        ComPtr<IDXGIDevice> dxgiDev;
        std::string adapterName = "Intel";
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDev))) && dxgiDev) {
            ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter) {
                DXGI_ADAPTER_DESC desc{};
                if (SUCCEEDED(adapter->GetDesc(&desc)))
                    adapterName = NarrowAscii(desc.Description);
            }
        }
        Logf("encoder", "adopted capture D3D device for QSV: %s\n", adapterName.c_str());
        return true;
    }

    bool MakeDxgiSampleFromTexture(ID3D11Texture2D* srcNv12, LONGLONG pts, LONGLONG dur,
                                   ComPtr<IMFSample>& sample)
    {
        if (!srcNv12 || !d3dDevice || !d3dContext || !useDxgiInput || width == 0 || height == 0)
            return false;

        D3D11_TEXTURE2D_DESC srcDesc{};
        srcNv12->GetDesc(&srcDesc);
        if (srcDesc.Format != DXGI_FORMAT_NV12 || srcDesc.Width != width || srcDesc.Height != height)
            return false;

        // Copy into our stable encoder input texture so the VP output can be
        // rewritten next frame while QSV still holds the sample.
        if (!EnsureNv12Textures())
            return false;
        d3dContext->CopyResource(nv12Tex.Get(), srcNv12);

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12Tex.Get(), 0, FALSE, &buffer)))
            return false;
        if (FAILED(MFCreateSample(&sample)))
            return false;
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(pts);
        sample->SetSampleDuration(dur);
        return true;
    }

    bool EnsureIntelEncoderDevice()
    {
        if (d3dDevice && dxgiManager && d3dContext && IsIntelDevice(d3dDevice.Get()))
            return true;

        d3dDevice.Reset();
        d3dContext.Reset();
        nv12Tex.Reset();
        nv12Staging.Reset();
        dxgiManager.Reset();
        dxgiResetToken = 0;
        useDxgiInput = false;

        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            Logf("encoder", "CreateDXGIFactory1 failed\n");
            return false;
        }

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
            if (lower.find(L"intel") == std::wstring::npos)
                continue;

            D3D_FEATURE_LEVEL level{};
            HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                           D3D11_SDK_VERSION, &d3dDevice, &level, &d3dContext);
            if (FAILED(hr) || !d3dDevice) {
                Logf("encoder", "Intel D3D11CreateDevice failed: %s hr=0x%08lX\n",
                     NarrowAscii(name).c_str(), static_cast<unsigned long>(hr));
                d3dDevice.Reset();
                d3dContext.Reset();
                continue;
            }
            {
                ComPtr<ID3D10Multithread> mt;
                if (SUCCEEDED(d3dDevice.As(&mt)))
                    mt->SetMultithreadProtected(TRUE);
            }

            hr = MFCreateDXGIDeviceManager(&dxgiResetToken, &dxgiManager);
            if (FAILED(hr) || !dxgiManager) {
                Logf("encoder", "MFCreateDXGIDeviceManager failed hr=0x%08lX\n",
                     static_cast<unsigned long>(hr));
                d3dDevice.Reset();
                d3dContext.Reset();
                dxgiManager.Reset();
                continue;
            }
            hr = dxgiManager->ResetDevice(d3dDevice.Get(), dxgiResetToken);
            if (FAILED(hr)) {
                Logf("encoder", "DXGIDeviceManager::ResetDevice failed hr=0x%08lX\n",
                     static_cast<unsigned long>(hr));
                d3dDevice.Reset();
                d3dContext.Reset();
                dxgiManager.Reset();
                continue;
            }
            Logf("encoder", "Intel encoder D3D device ready: %s\n", NarrowAscii(name).c_str());
            return true;
        }

        Logf("encoder", "no Intel adapter for QSV DXGI path\n");
        return false;
    }

    bool ApplyD3DManager()
    {
        useDxgiInput = false;
        if (!mft)
            return true;
        ComPtr<IMFAttributes> attrs;
        UINT32 aware = 0;
        if (SUCCEEDED(mft->GetAttributes(&attrs)) && attrs)
            attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware);
        if (!aware || !dxgiManager) {
            Logf("encoder", "sysmem input for %s (D3D11_AWARE=%u manager=%d)\n",
                 NarrowAscii(mftName).c_str(), aware, dxgiManager ? 1 : 0);
            return true;
        }
        HRESULT hr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                         reinterpret_cast<ULONG_PTR>(dxgiManager.Get()));
        if (FAILED(hr)) {
            Logf("encoder", "MFT_MESSAGE_SET_D3D_MANAGER failed: %s hr=0x%08lX - using sysmem input\n",
                 NarrowAscii(mftName).c_str(), static_cast<unsigned long>(hr));
            useDxgiInput = false;
            return true;
        }
        useDxgiInput = true;
        Logf("encoder", "MFT D3D manager set (DXGI NV12 input): %s\n", NarrowAscii(mftName).c_str());
        return true;
    }

    // Activate + media types + codec knobs for one MFT. On failure logs and
    // leaves the encoder unbound (caller may try the next activate).
    bool TryBindActivate(IMFActivate* act,
                         const std::wstring& name,
                         uint32_t width,
                         uint32_t height,
                         uint32_t fps,
                         uint32_t bitrateBps, uint32_t qualityVsSpeed, uint32_t gopSeconds,
                         bool useQualityRc, uint32_t rcQuality)
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
            // Belt-and-suspenders with CODECAPI_AVLowLatencyMode (same intent).
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }

        if (!ApplyD3DManager()) {
            ResetMft();
            act->ShutdownObject();
            return false;
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
            // Quality preset: MF Quality RC (QSV CQP-ish). Else PeakConstrainedVBR.
            if (useQualityRc) {
                TrySetUInt32(codecApi.Get(), CODECAPI_AVEncCommonRateControlMode,
                             eAVEncCommonRateControlMode_Quality);
                const ULONG q = rcQuality > 100 ? 100 : rcQuality;
                TrySetUInt32(codecApi.Get(), CODECAPI_AVEncCommonQuality, q);
                TrySetUInt32(codecApi.Get(), CODECAPI_AVEncCommonMaxBitRate, bitrateBps);
            } else {
                TrySetUInt32(codecApi.Get(), CODECAPI_AVEncCommonRateControlMode,
                             eAVEncCommonRateControlMode_PeakConstrainedVBR);
                TrySetUInt32(codecApi.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrateBps * 6 / 10);
                TrySetUInt32(codecApi.Get(), CODECAPI_AVEncCommonMaxBitRate, bitrateBps);
            }
            TrySetUInt32(codecApi.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
            TrySetBool(codecApi.Get(), CODECAPI_AVLowLatencyMode, true);
            // Without an explicit GOP size some MFTs (observed with NVENC here)
            // default to all-intra. RequestKeyFrame() still forces one early on
            // demand (spec "kf").
            // MS CODECAPI: 0 = faster/lower quality, 100 = slower/higher quality.
            const ULONG gopSec = gopSeconds ? gopSeconds : 2;
            const ULONG gop = fps * gopSec;
            const ULONG qvs = qualityVsSpeed > 100 ? 100 : qualityVsSpeed;
            TrySetUInt32(codecApi.Get(), CODECAPI_AVEncMPVGOPSize, gop);
            TrySetUInt32(codecApi.Get(), CODECAPI_AVEncCommonQualityVsSpeed, qvs);
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



    bool EnsureNv12Textures()
    {
        if (!d3dDevice || !d3dContext || width == 0 || height == 0)
            return false;
        if (nv12Tex && nv12Staging) {
            D3D11_TEXTURE2D_DESC desc{};
            nv12Tex->GetDesc(&desc);
            if (desc.Width == width && desc.Height == height)
                return true;
        }
        nv12Tex.Reset();
        nv12Staging.Reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

        D3D11_TEXTURE2D_DESC stagingDesc = td;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(d3dDevice->CreateTexture2D(&td, nullptr, &nv12Tex)))
            return false;
        if (FAILED(d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &nv12Staging))) {
            nv12Tex.Reset();
            return false;
        }
        return true;
    }

    // Copy tightly-packed NV12 (width stride) into a DXGI surface sample.
    bool MakeDxgiSampleFromNv12(const uint8_t* nv12, size_t size, LONGLONG pts, LONGLONG dur,
                                ComPtr<IMFSample>& sample)
    {
        const DWORD expected = width * height * 3 / 2;
        if (!nv12 || size < expected || !EnsureNv12Textures())
            return false;

        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(d3dContext->Map(nv12Staging.Get(), 0, D3D11_MAP_WRITE, 0, &map)))
            return false;
        auto* dst = static_cast<uint8_t*>(map.pData);
        for (uint32_t y = 0; y < height; ++y)
            memcpy(dst + static_cast<size_t>(y) * map.RowPitch,
                   nv12 + static_cast<size_t>(y) * width, width);
        const uint8_t* uvSrc = nv12 + static_cast<size_t>(width) * height;
        uint8_t* uvDst = dst + static_cast<size_t>(height) * map.RowPitch;
        for (uint32_t y = 0; y < height / 2; ++y)
            memcpy(uvDst + static_cast<size_t>(y) * map.RowPitch,
                   uvSrc + static_cast<size_t>(y) * width, width);
        d3dContext->Unmap(nv12Staging.Get(), 0);
        d3dContext->CopyResource(nv12Tex.Get(), nv12Staging.Get());

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12Tex.Get(), 0, FALSE, &buffer)))
            return false;
        if (FAILED(MFCreateSample(&sample)))
            return false;
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(pts);
        sample->SetSampleDuration(dur);
        return true;
    }

    bool MakeSysmemSampleFromNv12(const uint8_t* nv12, size_t size, LONGLONG pts, LONGLONG dur,
                                  ComPtr<IMFSample>& sample)
    {
        const DWORD expected = width * height * 3 / 2;
        if (!nv12 || size < expected)
            return false;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateMemoryBuffer(expected, &buffer)))
            return false;
        BYTE* dst = nullptr;
        buffer->Lock(&dst, nullptr, nullptr);
        memcpy(dst, nv12, expected);
        buffer->Unlock();
        buffer->SetCurrentLength(expected);
        if (FAILED(MFCreateSample(&sample)))
            return false;
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(pts);
        sample->SetSampleDuration(dur);
        return true;
    }

    bool MakeNv12Sample(const uint8_t* nv12, size_t size, LONGLONG pts, LONGLONG dur,
                        ComPtr<IMFSample>& sample)
    {
        if (useDxgiInput)
            return MakeDxgiSampleFromNv12(nv12, size, pts, dur, sample);
        return MakeSysmemSampleFromNv12(nv12, size, pts, dur, sample);
    }


    // Feed one black NV12 frame. Async MFTs that never signal NeedInput fail
    // the probe so Configure can try the next activate instead of hanging the UI.
    bool ProbeFirstFrame()
    {
        asyncTimedOut = false;
        pendingNeedInput = false;

        const DWORD expected = width * height * 3 / 2;
        auto make_sample = [&](LONGLONG pts) -> ComPtr<IMFSample> {
            const DWORD expected = width * height * 3 / 2;
            std::vector<uint8_t> nv12(expected, 0);
            std::fill(nv12.begin(), nv12.begin() + static_cast<std::ptrdiff_t>(width * height),
                      static_cast<uint8_t>(0x10));
            std::fill(nv12.begin() + static_cast<std::ptrdiff_t>(width * height), nv12.end(),
                      static_cast<uint8_t>(0x80));
            ComPtr<IMFSample> sample;
            const LONGLONG dur = frameDuration100ns > 0 ? frameDuration100ns : 333333;
            if (!MakeNv12Sample(nv12.data(), nv12.size(), pts, dur, sample)) {
                Logf("encoder", "MFT probe sample create failed (dxgi=%d)\n", useDxgiInput ? 1 : 0);
                return nullptr;
            }
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
            if (!sample)
                return false;
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
            if (!sample) {
                asyncTimedOut = true;
                return;
            }
            PumpAsync(sample.Get(), out);
        };

        // QSV may NeedInput several times before the first HaveOutput.
        TrySetUInt32(codecApi.Get(), CODECAPI_AVEncVideoForceKeyFrame, TRUE);
        for (int i = 0; i < 8 && out.empty(); ++i) {
            asyncTimedOut = false;
            feed(static_cast<LONGLONG>(i) * dur);
            if (!out.empty())
                break;
            const DWORD start = GetTickCount();
            while (out.empty() && GetTickCount() - start < 300) {
                DrainAvailableAsync(out);
                if (out.empty())
                    Sleep(1);
            }
            if (asyncTimedOut && out.empty() && i >= 2)
                break;
        }
        if (out.empty()) {
            Logf("encoder", "MFT probe produced no AU: %s (asyncTimedOut=%d)\n",
                 NarrowAscii(mftName).c_str(), asyncTimedOut ? 1 : 0);
            return false;
        }

        Logf("encoder", "MFT probe ok: %s (%zu au)\n", NarrowAscii(mftName).c_str(), out.size());
        return true;
    }

    bool EnumAndBind(bool hardware, uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrateBps, uint32_t qualityVsSpeed, uint32_t gopSeconds,
                     bool useQualityRc, uint32_t rcQuality)
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

        Logf("encoder", "MFTEnumEx(%s): %u candidate(s)\n", hardware ? "hardware" : "software",
             static_cast<unsigned>(candidates.size()));
        for (const auto& c : candidates)
            Logf("encoder", "  [%u] pref=%d %s\n", c.index, c.preference, NarrowAscii(c.name).c_str());

        bool ok = false;
        for (const auto& c : candidates) {
            if (TryBindActivate(c.act, c.name, width, height, fps, bitrateBps, qualityVsSpeed, gopSeconds,
                                useQualityRc, rcQuality)) {
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
                // Keep draining: HaveOutput may already be queued behind NeedInput.
                pendingNeedInput = true;
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

                // After ProcessInput, Require HaveOutput. Another NeedInput alone is
        // not success — QSV was observed to re-ask for input forever with no AU.
        {
            const DWORD kOutputTimeoutMs = 800;
            const DWORD start = GetTickCount();
            bool gotOutput = false;
            while (GetTickCount() - start < kOutputTimeoutMs) {
                DrainAvailableAsync(out);
                if (!out.empty()) {
                    gotOutput = true;
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
                    if (ProcessOutputOnce(out))
                        gotOutput = true;
                } else if (met == METransformNeedInput) {
                    pendingNeedInput = true;
                }
            }
            if (!gotOutput) {
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

bool H264Encoder::Configure(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrateBps,
                            uint32_t qualityVsSpeed, uint32_t gopSeconds, bool useQualityRc,
                            uint32_t rcQuality)
{
    impl_->configured = false;
    impl_->width = width;
    impl_->height = height;
    impl_->fps = fps;
    impl_->qualityVsSpeed = qualityVsSpeed > 100 ? 100 : qualityVsSpeed;
    impl_->gopSeconds = gopSeconds ? gopSeconds : 2;
    impl_->useQualityRc = useQualityRc;
    impl_->rcQuality = rcQuality > 100 ? 100 : rcQuality;
    impl_->frameDuration100ns = 10'000'000LL / fps;
    impl_->timestamp = 0;
    impl_->pendingNeedInput = false;
    impl_->spsPpsCache = SpsPpsCache{};
    impl_->nv12Tex.Reset();
    impl_->nv12Staging.Reset();
    impl_->ResetMft();

    // Walk every hardware activate (Intel QSV preferred), then software.
    // Previously we only ActivateObject'd activates[0]; one bad first entry
    // dropped us on the Microsoft software MFT with no HRESULT in the log.
    impl_->consecutiveAsyncTimeouts = 0;
    impl_->lastAsyncTimedOut = false;

    if (!impl_->d3dDevice) {
        if (!impl_->EnsureIntelEncoderDevice())
            Logf("encoder", "Configure: Intel D3D device unavailable - HW may fall back\n");
    }

    bool hwOk = false;
    if (impl_->allowHardware)
        hwOk = impl_->EnumAndBind(/*hardware=*/true, width, height, fps, bitrateBps,
                                  impl_->qualityVsSpeed, impl_->gopSeconds,
                                  impl_->useQualityRc, impl_->rcQuality);
    bool ok = hwOk;
    if (!ok)
        ok = impl_->EnumAndBind(/*hardware=*/false, width, height, fps, bitrateBps,
                                impl_->qualityVsSpeed, impl_->gopSeconds,
                                impl_->useQualityRc, impl_->rcQuality);
    if (!ok) {
        Logf("encoder", "no H.264 MFT could be configured for %ux%u @ %u\n", width, height, fps);
        return false;
    }
    // Reconnect flash: a multi-second HW probe walk with no frames makes the
    // receiver drop us right as software finally binds. Stay on software for
    // the rest of this process once HW has already missed.
    if (impl_->allowHardware && !hwOk) {
        impl_->allowHardware = false;
        Logf("encoder", "sticking to software after HW bind miss\n");
    }

    impl_->configured = true;
    return true;
}

bool H264Encoder::UpdateBitrate(uint32_t peakBitrateBps, uint32_t meanBitrateBps)
{
    if (!impl_->configured || !impl_->codecApi)
        return false;
    if (impl_->useQualityRc)
        return false;
    // Best-effort mid-stream bitrate change without full MFT rebind. Some MFTs
    // accept Mean/Max at runtime; others ignore silently (TrySetUInt32).
    TrySetUInt32(impl_->codecApi.Get(), CODECAPI_AVEncCommonMeanBitRate, meanBitrateBps);
    TrySetUInt32(impl_->codecApi.Get(), CODECAPI_AVEncCommonMaxBitRate, peakBitrateBps);
    // Also refresh MF_MT_AVG_BITRATE on the output type when possible — ignored
    // failures are fine; peak/mean CodecAPI is what Dynamic adaptation needs.
    return true;
}

bool H264Encoder::UsesQualityRc() const
{
    return impl_->useQualityRc;
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

    ComPtr<IMFSample> sample;
    if (!impl_->MakeNv12Sample(nv12, size, impl_->timestamp, impl_->frameDuration100ns, sample)) {
        Logf("encoder", "EncodeNv12 sample create failed (dxgi=%d)\n", impl_->useDxgiInput ? 1 : 0);
        return outFrames;
    }
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

bool H264Encoder::EnsureIntelEncoderDevice()
{
    return impl_ && impl_->EnsureIntelEncoderDevice();
}

bool H264Encoder::AdoptD3DDevice(ID3D11Device* device, ID3D11DeviceContext* context)
{
    return impl_ && impl_->AdoptD3DDevice(device, context);
}

bool H264Encoder::UsesDxgiInput() const
{
    return impl_ && impl_->useDxgiInput;
}

ID3D11Device* H264Encoder::D3DDevice() const
{
    return impl_ ? impl_->d3dDevice.Get() : nullptr;
}

std::vector<EncodedFrame> H264Encoder::EncodeDxgiNv12(ID3D11Texture2D* nv12)
{
    std::vector<EncodedFrame> outFrames;
    impl_->lastAsyncTimedOut = false;
    if (!impl_->configured || !nv12)
        return outFrames;

    ComPtr<IMFSample> sample;
    if (!impl_->MakeDxgiSampleFromTexture(nv12, impl_->timestamp, impl_->frameDuration100ns, sample)) {
        Logf("encoder", "EncodeDxgiNv12 sample create failed (dxgi=%d)\n", impl_->useDxgiInput ? 1 : 0);
        return outFrames;
    }
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

} // namespace od

