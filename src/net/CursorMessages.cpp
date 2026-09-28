#include "net/CursorMessages.h"

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace od {

namespace {

constexpr char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

std::string Base64Encode(const uint8_t* data, size_t size)
{
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < size) {
        uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kB64[(n >> 18) & 63]);
        out.push_back(kB64[(n >> 12) & 63]);
        out.push_back(kB64[(n >> 6) & 63]);
        out.push_back(kB64[n & 63]);
        i += 3;
    }
    if (i < size) {
        uint32_t n = uint32_t(data[i]) << 16;
        out.push_back(kB64[(n >> 18) & 63]);
        if (i + 1 < size) {
            n |= uint32_t(data[i + 1]) << 8;
            out.push_back(kB64[(n >> 12) & 63]);
            out.push_back(kB64[(n >> 6) & 63]);
            out.push_back('=');
        } else {
            out.push_back(kB64[(n >> 12) & 63]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

std::vector<uint8_t> EncodeBgraPng(const uint8_t* bgra, int width, int height)
{
    std::vector<uint8_t> png;
    if (!bgra || width <= 0 || height <= 0)
        return png;

    // WIC needs COM; MF/tray may already have initialized it.
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (co == RPC_E_CHANGED_MODE)
        co = S_OK;
    if (FAILED(co))
        return png;

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        return png;

    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromMemory(
            width, height, GUID_WICPixelFormat32bppBGRA,
            width * 4, static_cast<UINT>(width) * height * 4,
            const_cast<BYTE*>(bgra), &bitmap)))
        return png;

    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
        return png;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)))
        return png;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return png;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props)))
        return png;
    if (FAILED(frame->Initialize(props.Get())))
        return png;
    if (FAILED(frame->SetSize(width, height)))
        return png;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format)))
        return png;
    if (FAILED(frame->WriteSource(bitmap.Get(), nullptr)))
        return png;
    if (FAILED(frame->Commit()))
        return png;
    if (FAILED(encoder->Commit()))
        return png;

    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)))
        return png;
    const ULONG size = static_cast<ULONG>(stat.cbSize.QuadPart);
    png.resize(size);
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    if (FAILED(stream->Read(png.data(), size, &read)) || read != size) {
        png.clear();
        return png;
    }

    // PROTOCOL.md: keep under 24000 bytes pre-base64 so cursorImg stays < 32 KiB.
    if (png.size() > 24000)
        png.clear();
    return png;
}

std::string MakeCursorMessage(bool visible, double x, double y, uint64_t seq)
{
    char buf[192];
    if (!visible) {
        if (seq > 0)
            std::snprintf(buf, sizeof(buf), "{\"type\":\"cursor\",\"v\":0,\"s\":%llu}",
                          static_cast<unsigned long long>(seq));
        else
            std::snprintf(buf, sizeof(buf), "{\"type\":\"cursor\",\"v\":0}");
    } else if (seq > 0) {
        std::snprintf(buf, sizeof(buf),
                      "{\"type\":\"cursor\",\"v\":1,\"x\":%.6f,\"y\":%.6f,\"s\":%llu}",
                      x, y, static_cast<unsigned long long>(seq));
    } else {
        std::snprintf(buf, sizeof(buf), "{\"type\":\"cursor\",\"v\":1,\"x\":%.6f,\"y\":%.6f}", x, y);
    }
    return std::string(buf);
}

std::string MakeCursorImgMessage(const PointerShapeBgra& shape, uint32_t displayW, uint32_t displayH)
{
    if (shape.width <= 0 || shape.height <= 0 || displayW == 0 || displayH == 0)
        return {};

    auto png = EncodeBgraPng(shape.bgra.data(), shape.width, shape.height);
    if (png.empty())
        return {};

    const double nw = static_cast<double>(shape.width) / displayW;
    const double nh = static_cast<double>(shape.height) / displayH;
    const double ax = shape.width > 0 ? static_cast<double>(shape.hotX) / shape.width : 0.0;
    const double ay = shape.height > 0 ? static_cast<double>(shape.hotY) / shape.height : 0.0;
    const std::string b64 = Base64Encode(png.data(), png.size());

    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(6);
    oss << "{\"type\":\"cursorImg\",\"nw\":" << nw << ",\"nh\":" << nh
        << ",\"ax\":" << ax << ",\"ay\":" << ay
        << ",\"png\":\"" << b64 << "\"}";
    const std::string msg = oss.str();
    if (msg.size() >= 32768 || msg.find('\0') != std::string::npos)
        return {};
    return msg;
}

} // namespace od
