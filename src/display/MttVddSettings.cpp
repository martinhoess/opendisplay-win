#include "display/MttVddSettings.h"

#include "display/ExistingMonitor.h"

#include "app/Log.h"

#include <windows.h>

#include <cfgmgr32.h>
#include <setupapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace od {

namespace {

constexpr wchar_t kLiveSettingsPath[] = L"C:\\VirtualDisplayDriver\\vdd_settings.xml";
constexpr wchar_t kLiveEdidPath[] = L"C:\\VirtualDisplayDriver\\user_edid.bin";

// Display adapter class.
const GUID kDisplayClassGuid = {
    0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

struct ModeKey {
    uint32_t w = 0, h = 0, hz = 0;
    bool operator<(const ModeKey& o) const
    {
        if (w != o.w)
            return w < o.w;
        if (h != o.h)
            return h < o.h;
        return hz < o.hz;
    }
    bool operator==(const ModeKey& o) const { return w == o.w && h == o.h && hz == o.hz; }
};

bool FileExists(const wchar_t* path)
{
    const DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string NarrowPath(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

bool ReadFileUtf8(const std::wstring& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool WriteFileUtf8Atomic(const std::wstring& path, const std::string& body)
{
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!out)
            return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        // Fall back to overwrite-in-place if replace fails (e.g. cross-volume).
        if (!CopyFileW(tmp.c_str(), path.c_str(), FALSE)) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        DeleteFileW(tmp.c_str());
    }
    return true;
}

// Scan existing <resolution> blocks for width/height/refresh_rate triples.
std::vector<ModeKey> ParseResolutions(const std::string& xml)
{
    std::vector<ModeKey> out;
    size_t pos = 0;
    while (true) {
        const size_t resOpen = xml.find("<resolution>", pos);
        if (resOpen == std::string::npos)
            break;
        const size_t resClose = xml.find("</resolution>", resOpen);
        if (resClose == std::string::npos)
            break;
        const std::string block = xml.substr(resOpen, resClose - resOpen);

        auto tagU32 = [&](const char* openTag, const char* closeTag, uint32_t& v) -> bool {
            const size_t a = block.find(openTag);
            if (a == std::string::npos)
                return false;
            const size_t start = a + strlen(openTag);
            const size_t b = block.find(closeTag, start);
            if (b == std::string::npos)
                return false;
            v = static_cast<uint32_t>(strtoul(block.c_str() + start, nullptr, 10));
            return v > 0;
        };

        ModeKey m;
        if (tagU32("<width>", "</width>", m.w) && tagU32("<height>", "</height>", m.h)) {
            if (!tagU32("<refresh_rate>", "</refresh_rate>", m.hz))
                m.hz = 60;
            out.push_back(m);
        }
        pos = resClose + 13;
    }
    return out;
}

std::string FormatResolutionBlock(uint32_t w, uint32_t h, uint32_t hz)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
             "        <resolution>\n"
             "            <width>%u</width>\n"
             "            <height>%u</height>\n"
             "            <refresh_rate>%u</refresh_rate>\n"
             "        </resolution>\n",
             w, h, hz);
    return buf;
}

// Landscape-only modes @ 60 Hz (and any explicit m.hz); de-dupe.
// Portrait is Windows Display orientation — listing both WxH and HxW fought
// hello vs DXGI and made "Keep" resolution useless under mode forcing.
std::vector<ModeKey> ExpandWanted(const std::vector<MttMode>& wanted)
{
    std::vector<ModeKey> keys;
    auto add = [&](uint32_t w, uint32_t h, uint32_t hz) {
        if (w == 0 || h == 0 || hz == 0)
            return;
        if (h > w)
            std::swap(w, h); // store landscape only
        ModeKey k{w, h, hz};
        if (std::find(keys.begin(), keys.end(), k) == keys.end())
            keys.push_back(k);
    };
    for (const MttMode& m : wanted) {
        const uint32_t hz = m.hz ? m.hz : 60u;
        add(m.width, m.height, hz);
        if (hz != 60u)
            add(m.width, m.height, 60u);
    }
    return keys;
}

// Replace the entire <resolutions>...</resolutions> body with `modes` (prune leftovers).
std::string ReplaceResolutionsXml(const std::string& xml, const std::vector<ModeKey>& modes)
{
    const size_t open = xml.find("<resolutions>");
    const size_t close = xml.rfind("</resolutions>");
    if (open == std::string::npos || close == std::string::npos || close < open)
        return {}; // malformed

    const size_t bodyStart = open + strlen("<resolutions>");
    std::string body = "\n";
    for (const ModeKey& m : modes)
        body += FormatResolutionBlock(m.w, m.h, m.hz);
    body += "    ";

    std::string out = xml;
    out.replace(bodyStart, close - bodyStart, body);
    return out;
}

bool SameModeSet(std::vector<ModeKey> a, std::vector<ModeKey> b)
{
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

// Ask the MttVDD driver to re-read vdd_settings.xml without ChangeDisplaySettings.
// Prefer pnputil /restart-device on Root\MttVDD. Fall back to disable/enable of the
// MTT1337 *monitor* only - never DICS_DISABLE the display adapter (that can leave
// CM_PROB_DISABLED / pending reboot on this class of device).
bool ReloadMttVddDevice(std::string& detail)
{
    auto findMttAdapterInstanceId = [](std::wstring& outId) -> bool {
        HDEVINFO devInfo = SetupDiGetClassDevsW(&kDisplayClassGuid, nullptr, nullptr, DIGCF_PRESENT);
        if (devInfo == INVALID_HANDLE_VALUE)
            return false;
        SP_DEVINFO_DATA did{};
        did.cbSize = sizeof(did);
        bool found = false;
        for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &did); ++i) {
            wchar_t hwIds[1024] = {};
            if (!SetupDiGetDeviceRegistryPropertyW(devInfo, &did, SPDRP_HARDWAREID, nullptr,
                                                   reinterpret_cast<PBYTE>(hwIds), sizeof(hwIds), nullptr))
                continue;
            bool match = false;
            for (const wchar_t* p = hwIds; *p; p += wcslen(p) + 1) {
                if (_wcsicmp(p, L"Root\\MttVDD") == 0) {
                    match = true;
                    break;
                }
            }
            if (!match)
                continue;
            wchar_t instanceId[256];
            if (!SetupDiGetDeviceInstanceIdW(devInfo, &did, instanceId, 256, nullptr))
                continue;
            outId = instanceId;
            found = true;
            break;
        }
        SetupDiDestroyDeviceInfoList(devInfo);
        return found;
    };

    auto runPnputilRestart = [](const std::wstring& instanceId, std::string& detailOut) -> bool {
        std::wstring args = L"/restart-device "" + instanceId + L""";
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"open";
        sei.lpFile = L"pnputil.exe";
        sei.lpParameters = args.c_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr) {
            detailOut = "pnputil launch failed";
            return false;
        }
        WaitForSingleObject(sei.hProcess, 30000);
        DWORD code = 1;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        if (code == 0) {
            detailOut = "reloaded MttVDD via pnputil /restart-device";
            Sleep(1000);
            return true;
        }
        char buf[96];
        snprintf(buf, sizeof(buf), "pnputil /restart-device exit=%lu", static_cast<unsigned long>(code));
        detailOut = buf;
        return false;
    };

    // Monitor-class bounce: DISPLAY\MTT1337\... - softer than killing the adapter.
    auto bounceMttMonitor = [](std::string& detailOut) -> bool {
        static const GUID kMonitorClass = {
            0x4d36e96e, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
        HDEVINFO devInfo = SetupDiGetClassDevsW(&kMonitorClass, nullptr, nullptr, DIGCF_PRESENT);
        if (devInfo == INVALID_HANDLE_VALUE) {
            detailOut = "monitor SetupDiGetClassDevs failed";
            return false;
        }
        SP_DEVINFO_DATA did{};
        did.cbSize = sizeof(did);
        bool ok = false;
        for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &did); ++i) {
            wchar_t instanceId[256];
            if (!SetupDiGetDeviceInstanceIdW(devInfo, &did, instanceId, 256, nullptr))
                continue;
            if (wcsstr(instanceId, L"MTT1337") == nullptr)
                continue;

            auto propChange = [&](DWORD state) -> bool {
                SP_PROPCHANGE_PARAMS pcp{};
                pcp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
                pcp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
                pcp.StateChange = state;
                pcp.Scope = DICS_FLAG_GLOBAL;
                pcp.HwProfile = 0;
                if (!SetupDiSetClassInstallParamsW(devInfo, &did, &pcp.ClassInstallHeader, sizeof(pcp)))
                    return false;
                return SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devInfo, &did) != FALSE;
            };

            const bool disabled = propChange(DICS_DISABLE);
            Sleep(800);
            const bool enabled = propChange(DICS_ENABLE);
            ok = disabled && enabled;
            detailOut = ok ? "reloaded MTT monitor via SetupAPI disable/enable (MTT1337)"
                           : "MTT1337 monitor disable/enable failed";
            break;
        }
        SetupDiDestroyDeviceInfoList(devInfo);
        if (!ok && detailOut.empty())
            detailOut = "MTT1337 monitor not found";
        return ok;
    };

    std::wstring instanceId;
    if (!findMttAdapterInstanceId(instanceId)) {
        detail = "MttVDD root device (Root\\MttVDD) not found";
        return false;
    }

    std::string step;
    if (runPnputilRestart(instanceId, step)) {
        detail = step;
        return true;
    }
    detail = step + "; ";
    if (bounceMttMonitor(step)) {
        detail += step;
        return true;
    }
    detail += step + " (modes written; Windows may need a display re-plug)";
    return false;
}



int ParseMonitorCount(const std::string& xml)
{
    const size_t monOpen = xml.find("<monitors>");
    const size_t monClose = xml.find("</monitors>");
    if (monOpen == std::string::npos || monClose == std::string::npos || monClose < monOpen)
        return -1;
    const std::string block = xml.substr(monOpen, monClose - monOpen);
    const size_t a = block.find("<count>");
    const size_t b = block.find("</count>");
    if (a == std::string::npos || b == std::string::npos || b <= a)
        return -1;
    return static_cast<int>(strtol(block.c_str() + a + strlen("<count>"), nullptr, 10));
}

std::string ReplaceMonitorCountXml(const std::string& xml, uint32_t count)
{
    const size_t monOpen = xml.find("<monitors>");
    const size_t monClose = xml.find("</monitors>");
    if (monOpen == std::string::npos || monClose == std::string::npos || monClose < monOpen)
        return {};
    const size_t blockEnd = monClose + strlen("</monitors>");
    char body[128];
    snprintf(body, sizeof(body),
             "<monitors>\n"
             "        <count>%u</count>\n"
             "    </monitors>",
             count);
    std::string out = xml;
    out.replace(monOpen, blockEnd - monOpen, body);
    return out;
}

bool WaitForMttMonitor(bool wantPresent, int timeoutMs)
{
    ExistingMonitor mon{};
    int waited = 0;
    while (waited < timeoutMs) {
        const bool present = FindMttVirtualMonitor(mon);
        if (present == wantPresent)
            return true;
        Sleep(200);
        waited += 200;
    }
    ExistingMonitor again{};
    return FindMttVirtualMonitor(again) == wantPresent;
}

std::wstring AppDataDirW()
{
    wchar_t* appData = nullptr;
    size_t len = 0;
    std::wstring base;
    if (_wdupenv_s(&appData, &len, L"APPDATA") == 0 && appData) {
        base = appData;
        free(appData);
    }
    if (base.empty())
        base = L".";
    return base + L"\\opendisplay-win";
}

std::wstring MttTopologyPathW()
{
    return AppDataDirW() + L"\\mtt_display.json";
}

bool JsonReadIntField(const std::string& json, const char* key, int& out)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos)
        return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return false;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        ++pos;
    char* endp = nullptr;
    long v = std::strtol(json.c_str() + pos, &endp, 10);
    if (endp == json.c_str() + pos)
        return false;
    out = static_cast<int>(v);
    return true;
}

bool JsonReadU32Field(const std::string& json, const char* key, uint32_t& out)
{
    int v = 0;
    if (!JsonReadIntField(json, key, v) || v < 0)
        return false;
    out = static_cast<uint32_t>(v);
    return true;
}

bool JsonReadStringField(const std::string& json, const char* key, std::string& out)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos)
        return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return false;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    if (pos >= json.size() || json[pos] != '"')
        return false;
    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos)
        return false;
    out.assign(json, pos + 1, end - (pos + 1));
    return true;
}

struct MttTopology {
    int x = 0;
    int y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t hz = 0;
    std::string deviceId;
    bool valid = false;
};

bool LoadTopologyFile(MttTopology& out)
{
    std::string body;
    if (!ReadFileUtf8(MttTopologyPathW(), body) || body.empty())
        return false;
    int x = 0, y = 0;
    if (!JsonReadIntField(body, "x", x) || !JsonReadIntField(body, "y", y))
        return false;
    uint32_t w = 0, h = 0, hz = 0;
    (void)JsonReadU32Field(body, "width", w);
    (void)JsonReadU32Field(body, "height", h);
    (void)JsonReadU32Field(body, "hz", hz);
    std::string id;
    (void)JsonReadStringField(body, "deviceId", id);
    out = {};
    out.x = x;
    out.y = y;
    out.width = w;
    out.height = h;
    out.hz = hz;
    out.deviceId = std::move(id);
    out.valid = true;
    return true;
}

bool WriteTopologyFile(const MttTopology& t)
{
    const std::wstring dir = AppDataDirW();
    CreateDirectoryW(dir.c_str(), nullptr);
    std::ostringstream ss;
    ss << "{\n"
       << "  \"x\": " << t.x << ",\n"
       << "  \"y\": " << t.y << ",\n"
       << "  \"width\": " << t.width << ",\n"
       << "  \"height\": " << t.height << ",\n"
       << "  \"hz\": " << t.hz << ",\n"
       << "  \"deviceId\": \"";
    for (char c : t.deviceId) {
        if (c == '\\')
            ss << "\\\\";
        else if (c != '"')
            ss << c;
    }
    ss << "\"\n}\n";
    return WriteFileUtf8Atomic(MttTopologyPathW(), ss.str());
}

struct MttGraceState {
    std::mutex mu;
    std::condition_variable cv;
    bool pending = false;
    uint64_t epoch = 0;
    std::string logTag;
    std::thread worker;
};

MttGraceState& MttGrace()
{
    static MttGraceState g;
    return g;
}


bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    if (n <= 0)
        return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(n));
    in.read(reinterpret_cast<char*>(out.data()), n);
    return static_cast<bool>(in) || in.eof();
}

bool WriteFileBytesAtomic(const std::wstring& path, const std::vector<uint8_t>& body)
{
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
        if (!out)
            return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (!CopyFileW(tmp.c_str(), path.c_str(), FALSE)) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        DeleteFileW(tmp.c_str());
    }
    return true;
}

bool FilesEqual(const std::wstring& a, const std::wstring& b)
{
    std::vector<uint8_t> ba, bb;
    if (!ReadFileBytes(a, ba) || !ReadFileBytes(b, bb))
        return false;
    return ba == bb;
}

std::wstring ExeDirW()
{
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
        return {};
    std::wstring p(exe);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    return p.substr(0, slash);
}

std::wstring FindShippedUserEdid()
{
    const std::wstring exeDir = ExeDirW();
    const std::wstring candidates[] = {
        exeDir + L"\\assets\\mtt\\user_edid.bin",
        exeDir + L"\\user_edid.bin",
        kLiveEdidPath,
    };
    for (const std::wstring& c : candidates) {
        if (!c.empty() && FileExists(c.c_str()))
            return c;
    }
    return {};
}


bool ParseOptionBool(const std::string& xml, const char* tag, bool& out)
{
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const size_t a = xml.find(open);
    if (a == std::string::npos)
        return false;
    const size_t start = a + open.size();
    const size_t b = xml.find(close, start);
    if (b == std::string::npos)
        return false;
    std::string v = xml.substr(start, b - start);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r' || v.front() == '\n'))
        v.erase(v.begin());
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n'))
        v.pop_back();
    if (v == "true" || v == "1") {
        out = true;
        return true;
    }
    if (v == "false" || v == "0") {
        out = false;
        return true;
    }
    return false;
}

std::string SetOptionBoolXml(const std::string& xml, const char* tag, bool value)
{
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const size_t a = xml.find(open);
    if (a == std::string::npos)
        return {};
    const size_t start = a + open.size();
    const size_t b = xml.find(close, start);
    if (b == std::string::npos)
        return {};
    std::string out = xml;
    out.replace(start, b - start, value ? "true" : "false");
    return out;
}



// ---- MTT topology policy state ------------------------------------------
// Serializes attach / restore / save / teardown across sender threads, the
// grace worker and the tray thread. Recursive: Ensure -> Restore, user
// teardown -> Save -> TearDown. Never hold it while joining the grace worker
// (the worker takes it inside TearDown).
std::recursive_mutex& MttTopoMutex()
{
    static std::recursive_mutex mu;
    return mu;
}

// True once the saved position has been applied to the current MTT head
// attachment (real CDS attach by us, or one-time adoption of a head that was
// already on the desktop, e.g. left over from a previous run / reboot). Reset
// when we tear the head down. While true, nothing re-applies x/y: ACCESS_LOST,
// rotate/resize rebuilds and hello-again all leave the user's layout alone.
std::atomic<bool> g_mttPositionedThisAttach{false};

// Set on Quit / session end: EnsureMttVddAttached must not re-attach the head
// from a racing pipeline rebuild while we are tearing it down for exit.
std::atomic<bool> g_mttShuttingDown{false};

// Our own topology applies (attach, restore) stamp NoteSelfDisplayChange();
// observers ignore display changes within this window.
constexpr uint32_t kSelfApplySettleMs = 2000;

struct ActiveDisplayInfo {
    std::wstring gdiName;
    RECT rect{};
    uint32_t sourceId = 0;
    LUID adapter{};
    uint32_t hzNum = 0, hzDen = 0;
};

// Active CCD paths with a valid source mode (desktop rects in physical px).
bool QueryActiveDisplays(std::vector<ActiveDisplayInfo>& out)
{
    out.clear();
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG rc = ERROR_INSUFFICIENT_BUFFER;
    for (int tries = 0; tries < 4 && rc == ERROR_INSUFFICIENT_BUFFER; ++tries) {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS)
            return false;
        paths.resize(np);
        modes.resize(nm);
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr);
        if (rc == ERROR_SUCCESS) {
            paths.resize(np);
            modes.resize(nm);
        }
    }
    if (rc != ERROR_SUCCESS)
        return false;
    for (const auto& p : paths) {
        if (!(p.flags & DISPLAYCONFIG_PATH_ACTIVE))
            continue;
        const UINT32 idx = p.sourceInfo.modeInfoIdx;
        if (idx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || idx >= modes.size() ||
            modes[idx].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
            continue;
        const DISPLAYCONFIG_SOURCE_MODE& sm = modes[idx].sourceMode;
        if (sm.width == 0 || sm.height == 0)
            continue;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = p.sourceInfo.adapterId;
        sn.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS)
            continue;
        ActiveDisplayInfo d;
        d.gdiName = sn.viewGdiDeviceName;
        d.rect.left = sm.position.x;
        d.rect.top = sm.position.y;
        d.rect.right = sm.position.x + static_cast<LONG>(sm.width);
        d.rect.bottom = sm.position.y + static_cast<LONG>(sm.height);
        d.sourceId = p.sourceInfo.id;
        d.adapter = p.sourceInfo.adapterId;
        d.hzNum = p.targetInfo.refreshRate.Numerator;
        d.hzDen = p.targetInfo.refreshRate.Denominator;
        out.push_back(std::move(d));
    }
    return true;
}

bool RectsOverlap(const RECT& a, const RECT& b)
{
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

// MTT head attached AND active in the CCD topology with a valid, non-cloned
// source mode. Fills the live rect. This is the gate for every save: when the
// user picked "Show only on 1" (MTT inactive) or the head is absent / mid
// reconfigure, we must never overwrite the good saved position.
bool QueryMttActiveRect(const std::wstring& gdiName, RECT& rect, std::string* why)
{
    std::vector<ActiveDisplayInfo> act;
    if (!QueryActiveDisplays(act)) {
        if (why)
            *why = "QueryDisplayConfig failed";
        return false;
    }
    const ActiveDisplayInfo* mtt = nullptr;
    for (const auto& d : act) {
        if (_wcsicmp(d.gdiName.c_str(), gdiName.c_str()) == 0) {
            mtt = &d;
            break;
        }
    }
    if (!mtt) {
        if (why)
            *why = "MTT not active in topology";
        return false;
    }
    for (const auto& d : act) {
        if (&d == mtt)
            continue;
        const bool sameSource = d.sourceId == mtt->sourceId && d.adapter.LowPart == mtt->adapter.LowPart &&
                                d.adapter.HighPart == mtt->adapter.HighPart;
        if (sameSource || RectsOverlap(d.rect, mtt->rect)) {
            if (why)
                *why = "MTT cloned/overlapping another display";
            return false;
        }
    }
    rect = mtt->rect;
    return true;
}

// Would placing the MTT head at (x,y) with w x h overlap another active
// display? Then Windows would reshuffle the layout ("position reset").
bool SavedRectOverlapsOthers(const std::wstring& mttGdiName, int x, int y, uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0)
        return false;
    std::vector<ActiveDisplayInfo> act;
    if (!QueryActiveDisplays(act))
        return false;
    RECT want{x, y, x + static_cast<LONG>(w), y + static_cast<LONG>(h)};
    for (const auto& d : act) {
        if (!mttGdiName.empty() && _wcsicmp(d.gdiName.c_str(), mttGdiName.c_str()) == 0)
            continue;
        if (RectsOverlap(d.rect, want))
            return true;
    }
    return false;
}

// Re-commit the current active topology (MTT already detached) to the CCD
// persistence database so Windows does not re-extend the MTT head on the next
// display re-enumeration / reboot. CDS_UPDATEREGISTRY alone only updates the
// legacy registry view.
bool PersistActiveTopologyToDatabase(std::string& detail)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG rc = ERROR_INSUFFICIENT_BUFFER;
    for (int tries = 0; tries < 4 && rc == ERROR_INSUFFICIENT_BUFFER; ++tries) {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) {
            detail = "GetDisplayConfigBufferSizes failed";
            return false;
        }
        paths.resize(np);
        modes.resize(nm);
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr);
        if (rc == ERROR_SUCCESS) {
            paths.resize(np);
            modes.resize(nm);
        }
    }
    if (rc != ERROR_SUCCESS || paths.empty()) {
        detail = "QueryDisplayConfig failed rc=" + std::to_string(rc);
        return false;
    }
    NoteSelfDisplayChange();
    const LONG sr = SetDisplayConfig(static_cast<UINT32>(paths.size()), paths.data(),
                                     static_cast<UINT32>(modes.size()), modes.data(),
                                     SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE |
                                         SDC_ALLOW_CHANGES);
    NoteSelfDisplayChange();
    if (sr != ERROR_SUCCESS) {
        detail = "SetDisplayConfig(SAVE_TO_DATABASE) rc=" + std::to_string(sr);
        return false;
    }
    detail = "topology saved to CCD database";
    return true;
}

} // namespace

// ---- Runtime EDID: single source of MTT modes ----------------------------
// MttModesForHello is the ONE place the MTT mode list comes from: EDID DTDs,
// XML <resolutions>, attach defaults and BakedMttModes (shipped fallback) all
// derive from it. tools/gen_user_edid.py (modes_for_size / build_edid) is the
// byte-identical reference implementation - keep them in lockstep.
namespace {

constexpr uint32_t kDefaultHelloW = 2360; // shipped fallback (11" class)
constexpr uint32_t kDefaultHelloH = 1640;
constexpr uint32_t kMttRefresh = 60;
constexpr uint32_t kEdidMaxDim = 4095; // DTD 12-bit active fields

struct IpadPanel {
    uint32_t w, h;
};
// Native iPad panels (landscape); sibling candidates after 16-floor.
constexpr IpadPanel kIpadPanels[] = {
    {2360, 1640}, // 10.9" / 11" Air, iPad 10th
    {2388, 1668}, // 11" Pro (pre-M4), 11" Air M2
    {2420, 1668}, // 11" Pro M4
    {2732, 2048}, // 12.9" Pro, 13" Air
    {2752, 2064}, // 13" Pro M4
    {2266, 1488}, // mini 6/7
    {2160, 1620}, // 10.2"
    {2224, 1668}, // 10.5" Pro
    {2048, 1536}, // 9.7" / older
};

// Physical size: 2352 px -> 225 mm (~265 ppi). Keeps the shipped 225x156 mm.
constexpr uint32_t kMmNum = 225;
constexpr uint32_t kMmDen = 2352;

constexpr uint8_t kEdidManufacturer[2] = {0x36, 0x94}; // "MTT"
constexpr uint16_t kEdidProduct = 0x1337;
constexpr uint32_t kEdidSerial = 0x4F445731; // "ODW1"
constexpr uint8_t kEdidWeek = 1;
constexpr uint32_t kEdidYear = 2026;

uint32_t Align16Floor(uint32_t v)
{
    return v & ~15u;
}

uint32_t PhysicalMm(uint32_t px)
{
    return (px * kMmNum + kMmDen / 2) / kMmDen;
}

struct CvtTiming {
    uint32_t clock10khz = 0;
    uint32_t hActive = 0, hBlank = 0, hFront = 0, hSync = 0;
    uint32_t vActive = 0, vBlank = 0, vFront = 0, vSync = 0;
};

// Port of gen_user_edid.py cvt_rb_params: fixed CVT-RB-ish horizontal
// blanking, search v_back so the 10 kHz pixel clock lands closest to refresh.
// Python round() is half-to-even; reproduced exactly with integer math.
bool CvtRbTiming(uint32_t hActive, uint32_t vActive, uint32_t refresh, CvtTiming& out)
{
    const uint32_t hFront = 48, hSync = 32, hBlank = 160;
    const uint32_t vFront = 3, vSync = 10;
    const uint64_t hTotal = hActive + hBlank;
    bool have = false;
    double bestErr = 0.0;
    for (uint32_t vBack = 6; vBack < 120; ++vBack) {
        const uint32_t vBlank = vFront + vSync + vBack;
        const uint64_t vTotal = static_cast<uint64_t>(vActive) + vBlank;
        const uint64_t n = hTotal * vTotal * refresh;
        uint64_t q = n / 10000;
        const uint64_t r = n % 10000;
        if (r > 5000 || (r == 5000 && (q & 1)))
            ++q;
        if (q < 1 || q > 65535)
            continue;
        const double actual = (static_cast<double>(q) * 10000.0) / static_cast<double>(hTotal * vTotal);
        const double err = std::fabs(actual - static_cast<double>(refresh));
        if (!have || err < bestErr) {
            have = true;
            bestErr = err;
            out.clock10khz = static_cast<uint32_t>(q);
            out.hActive = hActive;
            out.hBlank = hBlank;
            out.hFront = hFront;
            out.hSync = hSync;
            out.vActive = vActive;
            out.vBlank = vBlank;
            out.vFront = vFront;
            out.vSync = vSync;
            if (err < 0.01)
                break;
        }
    }
    return have;
}

void PackDetailedTiming(const CvtTiming& p, uint32_t hMm, uint32_t vMm, uint8_t* b)
{
    b[0] = static_cast<uint8_t>(p.clock10khz & 0xFF);
    b[1] = static_cast<uint8_t>((p.clock10khz >> 8) & 0xFF);
    b[2] = static_cast<uint8_t>(p.hActive & 0xFF);
    b[3] = static_cast<uint8_t>(p.hBlank & 0xFF);
    b[4] = static_cast<uint8_t>(((p.hActive >> 8) << 4) | ((p.hBlank >> 8) & 0x0F));
    b[5] = static_cast<uint8_t>(p.vActive & 0xFF);
    b[6] = static_cast<uint8_t>(p.vBlank & 0xFF);
    b[7] = static_cast<uint8_t>(((p.vActive >> 8) << 4) | ((p.vBlank >> 8) & 0x0F));
    b[8] = static_cast<uint8_t>(p.hFront & 0xFF);
    b[9] = static_cast<uint8_t>(p.hSync & 0xFF);
    b[10] = static_cast<uint8_t>(((p.vFront & 0x0F) << 4) | (p.vSync & 0x0F));
    b[11] = static_cast<uint8_t>(((p.hFront >> 8) << 6) | ((p.hSync >> 8) << 4) | ((p.vFront >> 4) << 2) |
                                 ((p.vSync >> 4) & 0x03));
    b[12] = static_cast<uint8_t>(hMm & 0xFF);
    b[13] = static_cast<uint8_t>(vMm & 0xFF);
    b[14] = static_cast<uint8_t>(((hMm >> 8) << 4) | ((vMm >> 8) & 0x0F));
    b[15] = 0;
    b[16] = 0;
    b[17] = 0x1E; // digital separate sync, +h +v
}

std::mutex& NotifierMutex()
{
    static std::mutex mu;
    return mu;
}

std::function<void(const std::wstring&)>& NotifierFn()
{
    static std::function<void(const std::wstring&)> fn;
    return fn;
}

void NotifyUser(const std::wstring& text)
{
    std::function<void(const std::wstring&)> fn;
    {
        std::lock_guard<std::mutex> lock(NotifierMutex());
        fn = NotifierFn();
    }
    if (fn)
        fn(text);
}

bool ProcessIsElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elev{};
    DWORD len = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &len) != FALSE;
    CloseHandle(token);
    return ok && elev.TokenIsElevated != 0;
}

std::string FormatModes(const std::vector<MttMode>& modes)
{
    std::string s;
    for (const MttMode& m : modes) {
        if (!s.empty())
            s += ", ";
        s += std::to_string(m.width) + "x" + std::to_string(m.height) + "@" + std::to_string(m.hz);
    }
    return s;
}

bool ModeOffered(const std::vector<MttMode>& offered, uint32_t w, uint32_t h)
{
    if (h > w)
        std::swap(w, h);
    for (const MttMode& m : offered)
        if (m.width == w && m.height == h)
            return true;
    return false;
}

} // namespace

std::vector<MttMode> MttModesForHello(uint32_t helloW, uint32_t helloH)
{
    uint32_t w = helloW, h = helloH;
    if (h > w)
        std::swap(w, h); // always landscape: long side first
    w = Align16Floor(w);
    h = Align16Floor(h);
    if (w < 16 || h < 16 || w > kEdidMaxDim || h > kEdidMaxDim) {
        if (helloW == kDefaultHelloW && helloH == kDefaultHelloH)
            return {};
        return MttModesForHello(kDefaultHelloW, kDefaultHelloH);
    }

    std::vector<MttMode> out;
    auto push = [&](uint32_t mw, uint32_t mh, bool native) {
        if (mh > mw)
            std::swap(mw, mh);
        if (!native && (mw < 640 || mh < 480))
            return;
        for (const MttMode& m : out)
            if (m.width == mw && m.height == mh)
                return;
        out.push_back({mw, mh, kMttRefresh});
    };

    push(w, h, true);

    // Closest-aspect sibling panel: aspect within 1%, width within 8%.
    bool haveBest = false;
    uint64_t bestDiff = 0, bestPh = 0;
    uint32_t bestPw = 0;
    for (const IpadPanel& panel : kIpadPanels) {
        const uint32_t pw = Align16Floor(panel.w), ph = Align16Floor(panel.h);
        if (pw == w && ph == h)
            continue;
        const int64_t cross = static_cast<int64_t>(pw) * h - static_cast<int64_t>(w) * ph;
        const uint64_t diff = static_cast<uint64_t>(cross < 0 ? -cross : cross);
        if (100 * diff > static_cast<uint64_t>(w) * ph)
            continue;
        const uint32_t dw = pw > w ? pw - w : w - pw;
        if (100ull * dw > 8ull * w)
            continue;
        if (!haveBest || diff * bestPh < bestDiff * ph) {
            haveBest = true;
            bestDiff = diff;
            bestPh = ph;
            bestPw = pw;
        }
    }
    if (haveBest)
        push(bestPw, static_cast<uint32_t>(bestPh), false);

    push(Align16Floor(w * 3 / 4), Align16Floor(h * 3 / 4), false);
    push(Align16Floor(w / 2), Align16Floor(h / 2), false);
    if (out.size() > 4)
        out.resize(4);
    return out;
}

std::vector<uint8_t> BuildMttEdid(const std::vector<MttMode>& modes)
{
    if (modes.empty() || modes.size() > 4)
        return {};
    const uint32_t hMm = PhysicalMm(modes[0].width);
    const uint32_t vMm = PhysicalMm(modes[0].height);

    std::vector<uint8_t> e(128, 0);
    static const uint8_t kHeader[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    memcpy(e.data(), kHeader, 8);
    e[8] = kEdidManufacturer[0];
    e[9] = kEdidManufacturer[1];
    e[10] = static_cast<uint8_t>(kEdidProduct & 0xFF);
    e[11] = static_cast<uint8_t>(kEdidProduct >> 8);
    e[12] = static_cast<uint8_t>(kEdidSerial & 0xFF);
    e[13] = static_cast<uint8_t>((kEdidSerial >> 8) & 0xFF);
    e[14] = static_cast<uint8_t>((kEdidSerial >> 16) & 0xFF);
    e[15] = static_cast<uint8_t>((kEdidSerial >> 24) & 0xFF);
    e[16] = kEdidWeek;
    e[17] = static_cast<uint8_t>(kEdidYear - 1990);
    e[18] = 0x01; // EDID 1.3
    e[19] = 0x03;
    e[20] = 0x80; // digital input
    e[21] = static_cast<uint8_t>(std::max<uint32_t>(1, hMm / 10)); // cm
    e[22] = static_cast<uint8_t>(std::max<uint32_t>(1, vMm / 10));
    e[23] = 0x78; // gamma 2.2
    e[24] = 0x0A; // preferred timing mode, RGB
    static const uint8_t kChroma[10] = {0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54};
    memcpy(e.data() + 25, kChroma, 10);
    // 35..37: no established timings (zero). 38..53: standard timings unused.
    for (size_t i = 38; i < 54; ++i)
        e[i] = 0x01;

    for (size_t slot = 0; slot < 4; ++slot) {
        uint8_t* d = e.data() + 54 + slot * 18;
        if (slot < modes.size()) {
            CvtTiming t;
            if (!CvtRbTiming(modes[slot].width, modes[slot].height, modes[slot].hz ? modes[slot].hz : kMttRefresh,
                             t))
                return {};
            PackDetailedTiming(t, hMm, vMm, d);
        } else if (slot == modes.size()) {
            static const char kName[] = "OpenDisplay\n";
            d[3] = 0xFC; // monitor name
            memset(d + 5, ' ', 13);
            memcpy(d + 5, kName, sizeof(kName) - 1);
        } else {
            d[3] = 0x10; // dummy descriptor
        }
    }

    e[126] = 0; // no extensions
    uint32_t sum = 0;
    for (size_t i = 0; i < 127; ++i)
        sum += e[i];
    e[127] = static_cast<uint8_t>((256 - (sum % 256)) % 256);
    return e;
}

std::vector<MttMode> BakedMttModes()
{
    // Shipped fallback (assets/mtt/user_edid.bin) = the 11" default hello size.
    return MttModesForHello(kDefaultHelloW, kDefaultHelloH);
}

std::vector<MttMode> CurrentMttModes()
{
    std::string xml;
    const std::wstring path = FindLiveSettingsPath();
    std::vector<MttMode> out;
    if (!path.empty() && ReadFileUtf8(path, xml)) {
        for (const ModeKey& k : ParseResolutions(xml)) {
            uint32_t w = k.w, h = k.h;
            if (h > w)
                std::swap(w, h);
            if (!ModeOffered(out, w, h))
                out.push_back({w, h, k.hz ? k.hz : kMttRefresh});
        }
    }
    if (out.empty())
        out = BakedMttModes();
    return out;
}

void SetMttUserNotifier(std::function<void(const std::wstring&)> fn)
{
    std::lock_guard<std::mutex> lock(NotifierMutex());
    NotifierFn() = std::move(fn);
}


std::wstring FindLiveSettingsPath()
{
    if (FileExists(kLiveSettingsPath))
        return kLiveSettingsPath;
    return {};
}

namespace {

// Install `edid` as C:\VirtualDisplayDriver\user_edid.bin, set CustomEdid /
// PreventSpoof, mirror `modes` (in order, preferred first) into XML
// <resolutions>, reload MttVDD if anything changed. dryRun: only report
// whether something would change (r.changed) - never writes, never reloads.
MttEnsureResult EnsureCustomEdidBytes(const std::vector<uint8_t>& edid, const std::vector<MttMode>& modes,
                                      bool dryRun)
{
    MttEnsureResult r;
    r.path = FindLiveSettingsPath();
    if (r.path.empty()) {
        r.detail = "live settings missing (expected C:\\VirtualDisplayDriver\\vdd_settings.xml)";
        return r;
    }
    if (edid.size() < 128 || (edid.size() % 128) != 0 || modes.empty()) {
        r.detail = "invalid EDID / mode list";
        return r;
    }

    std::string xml;
    if (!ReadFileUtf8(r.path, xml)) {
        r.detail = "failed to read live settings";
        return r;
    }

    std::vector<std::string> notes;
    auto writeFail = [&](const char* what) {
        const DWORD err = GetLastError();
        char buf[112];
        snprintf(buf, sizeof(buf), "%s write failed (err=%lu)%s", what, static_cast<unsigned long>(err),
                 err == ERROR_ACCESS_DENIED ? " access denied" : "");
        r.detail = buf;
        return r;
    };

    // 1) user_edid.bin bytes.
    std::vector<uint8_t> live;
    const bool edidPresent = ReadFileBytes(kLiveEdidPath, live);
    const bool edidSame = edidPresent && live == edid;
    if (!edidSame) {
        notes.push_back(edidPresent ? "user_edid.bin differs" : "user_edid.bin missing");
        if (!dryRun && !WriteFileBytesAtomic(kLiveEdidPath, edid))
            return writeFail("edid");
    }

    // 2) CustomEdid=true, PreventSpoof=true (stable manufacturer+serial identity).
    bool custom = false, prevent = false;
    const bool haveCustom = ParseOptionBool(xml, "CustomEdid", custom);
    const bool havePrevent = ParseOptionBool(xml, "PreventSpoof", prevent);
    if (!haveCustom || !custom || !havePrevent || !prevent) {
        notes.push_back("CustomEdid/PreventSpoof flags");
        std::string next = xml;
        if (haveCustom)
            next = SetOptionBoolXml(next, "CustomEdid", true);
        if (next.empty()) {
            r.detail = "malformed XML (no CustomEdid)";
            return r;
        }
        if (havePrevent) {
            const std::string n2 = SetOptionBoolXml(next, "PreventSpoof", true);
            if (n2.empty()) {
                r.detail = "malformed XML (no PreventSpoof)";
                return r;
            }
            next = n2;
        }
        if (!dryRun && !WriteFileUtf8Atomic(r.path, next))
            return writeFail("options");
        xml = next;
    }

    // 3) XML <resolutions> = exactly `modes`, same order (IddCx mode list source).
    const std::vector<ModeKey> need = ExpandWanted(modes);
    const std::vector<ModeKey> present = ParseResolutions(xml);
    if (present != need) {
        notes.push_back("XML resolutions differ");
        const std::string rewritten = ReplaceResolutionsXml(xml, need);
        if (rewritten.empty()) {
            r.detail = "malformed XML (no <resolutions>)";
            return r;
        }
        if (!dryRun && !WriteFileUtf8Atomic(r.path, rewritten))
            return writeFail("resolutions");
        xml = rewritten;
        r.added = static_cast<int>(need.size());
    }

    r.ok = true;
    r.changed = !notes.empty();
    for (size_t i = 0; i < notes.size(); ++i) {
        if (i)
            r.detail += "; ";
        r.detail += notes[i];
    }
    if (!r.changed) {
        r.detail = "EDID + flags + XML modes already current";
        return r;
    }
    if (dryRun)
        return r;

    std::string reloadDetail;
    const bool reloaded = ReloadMttVddDevice(reloadDetail);
    r.detail += "; " + reloadDetail;
    if (!reloaded)
        r.detail += " (reload soft-failed; files written)";
    return r;
}

} // namespace

MttEnsureResult EnsureCustomEdid()
{
    // Shipped fallback: assets/mtt/user_edid.bin next to the exe (11" default).
    const std::wstring shipped = FindShippedUserEdid();
    std::vector<uint8_t> bytes;
    if (shipped.empty() || !ReadFileBytes(shipped, bytes)) {
        MttEnsureResult r;
        r.path = FindLiveSettingsPath();
        r.detail = "shipped user_edid.bin not found (assets/mtt/user_edid.bin)";
        return r;
    }
    return EnsureCustomEdidBytes(bytes, BakedMttModes(), /*dryRun=*/false);
}

MttEnsureResult EnsureCustomEdidForHello(uint32_t helloW, uint32_t helloH, bool dryRun)
{
    const std::vector<MttMode> modes = MttModesForHello(helloW, helloH);
    const std::vector<uint8_t> edid = BuildMttEdid(modes);
    if (edid.empty()) {
        MttEnsureResult r;
        r.path = FindLiveSettingsPath();
        r.detail = "EDID generation failed";
        return r;
    }
    return EnsureCustomEdidBytes(edid, modes, dryRun);
}

MttEnsureResult EnsureResolutions(const std::vector<MttMode>& wanted)
{
    MttEnsureResult r;
    r.path = FindLiveSettingsPath();
    if (r.path.empty()) {
        r.detail = "live settings missing (expected C:\\VirtualDisplayDriver\\vdd_settings.xml)";
        return r;
    }

    std::string xml;
    if (!ReadFileUtf8(r.path, xml)) {
        r.detail = "failed to read live settings";
        return r;
    }

    const std::vector<ModeKey> present = ParseResolutions(xml);
    const std::vector<ModeKey> need = ExpandWanted(wanted);

    // Exclusive OpenDisplay head: rewrite <resolutions> to the wanted set only
    // (prune 16:9 leftovers / other aspects), not append-only.
    if (SameModeSet(present, need)) {
        r.ok = true;
        r.changed = false;
        r.added = 0;
        r.detail = "already pruned to wanted set";
        return r;
    }

    const std::string rewritten = ReplaceResolutionsXml(xml, need);
    if (rewritten.empty()) {
        r.detail = "malformed XML (no <resolutions>)";
        return r;
    }

    const int removed = static_cast<int>(present.size()) - static_cast<int>(need.size());
    r.added = static_cast<int>(need.size());
    if (!WriteFileUtf8Atomic(r.path, rewritten)) {
        const DWORD err = GetLastError();
        char buf[96];
        snprintf(buf, sizeof(buf), "write failed (err=%lu)%s",
                 static_cast<unsigned long>(err),
                 err == ERROR_ACCESS_DENIED ? " access denied" : "");
        r.detail = buf;
        return r;
    }

    std::string reloadDetail;
    const bool reloaded = ReloadMttVddDevice(reloadDetail);

    r.ok = true;
    r.changed = true;
    r.detail = "rewrote " + std::to_string(need.size()) + " modes";
    if (removed > 0)
        r.detail += " (pruned ~" + std::to_string(removed) + " extras)";
    r.detail += "; " + reloadDetail;
    if (!reloaded)
        r.detail += " (reload soft-failed; modes written)";
    return r;

}

bool SelfElevateEnsure(uint32_t helloW, uint32_t helloH)
{
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
        return false;
    // The elevated child regenerates the same bytes from the hello size
    // (deterministic), writes them and reloads MttVDD. No size -> shipped file.
    std::wstring args = L"--ensure-mtt-edid";
    if (helloW && helloH)
        args += L" " + std::to_wstring(helloW) + L"x" + std::to_wstring(helloH);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr)
        return false; // UAC declined or launch failed

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}

namespace {
// Hello size whose EDID-change UAC prompt was declined/failed this run
// (w << 32 | h). Reconnects / link flaps of that iPad don't prompt again until
// the app restarts.
std::atomic<uint64_t> g_mttEdidDeclinedKey{0};
} // namespace

bool EnsureMttResolutionsForHello(uint32_t helloW, uint32_t helloH, const std::string& logTag)
{
    // Runs BEFORE the MTT head is attached (SenderApp::buildPipeline). Same
    // iPad again = bytes/XML already match = no UAC, no reload.
    const std::vector<MttMode> modes = MttModesForHello(helloW, helloH);
    Logf(logTag, "mtt: EDID modes for hello %ux%u: %s\n", helloW, helloH, FormatModes(modes).c_str());

    const MttEnsureResult probe = EnsureCustomEdidForHello(helloW, helloH, /*dryRun=*/true);
    if (!probe.ok) {
        Logf(logTag, "mtt: EDID check failed (%s) - keeping installed EDID\n", probe.detail.c_str());
        return false;
    }
    if (!probe.changed) {
        Logf(logTag, "mtt: EDID current for this iPad (no write, no reload) path=%s\n", NarrowPath(probe.path).c_str());
        return true;
    }
    Logf(logTag, "mtt: EDID update needed for hello %ux%u (%s)\n", helloW, helloH, probe.detail.c_str());

    if (ProcessIsElevated()) {
        const MttEnsureResult r = EnsureCustomEdidForHello(helloW, helloH, /*dryRun=*/false);
        Logf(logTag, "mtt: EDID install %s (%s)\n", r.ok ? "done" : "failed", r.detail.c_str());
        if (r.ok)
            g_mttPositionedThisAttach = false; // head re-enumerated: apply saved layout once
        return r.ok;
    }

    const uint64_t key = (static_cast<uint64_t>(helloW) << 32) | helloH;
    if (g_mttEdidDeclinedKey.load() == key) {
        Logf(logTag, "mtt: admin prompt for this size was declined earlier this run - connecting with the "
                     "installed EDID\n");
        return false;
    }

    NotifyUser(L"Setting up this iPad's screen size (one time only).");
    Logf(logTag, "mtt: EDID install needs admin, elevating...\n");
    if (!SelfElevateEnsure(helloW, helloH)) {
        Logf(logTag, "mtt: UAC declined or elevated EDID install failed - connecting with the installed EDID\n");
        g_mttEdidDeclinedKey.store(key);
        if (!FileExists(kLiveEdidPath)) {
            // Nothing installed at all: best effort with the shipped 11" file
            // (may still need admin to reload; never blocks the connection).
            const MttEnsureResult fb = EnsureCustomEdid();
            Logf(logTag, "mtt: shipped fallback EDID %s (%s)\n", fb.ok ? "installed" : "not installed",
                 fb.detail.c_str());
        }
        return false;
    }
    const MttEnsureResult after = EnsureCustomEdidForHello(helloW, helloH, /*dryRun=*/true);
    if (after.ok && !after.changed)
        Logf(logTag, "mtt: EDID installed for hello %ux%u (driver reloaded)\n", helloW, helloH);
    else
        Logf(logTag, "mtt: EDID still differs after elevated install (%s)\n", after.detail.c_str());
    g_mttPositionedThisAttach = false; // head re-enumerated: apply saved layout once
    return after.ok && !after.changed;
}



int ReadMonitorCount()
{
    const std::wstring path = FindLiveSettingsPath();
    if (path.empty())
        return -1;
    std::string xml;
    if (!ReadFileUtf8(path, xml))
        return -1;
    return ParseMonitorCount(xml);
}

MttEnsureResult SetMonitorCount(uint32_t count)
{
    MttEnsureResult r;
    r.path = FindLiveSettingsPath();
    if (r.path.empty()) {
        r.detail = "live settings missing (expected C:\\VirtualDisplayDriver\\vdd_settings.xml)";
        return r;
    }

    std::string xml;
    if (!ReadFileUtf8(r.path, xml)) {
        r.detail = "failed to read live settings";
        return r;
    }

    const int cur = ParseMonitorCount(xml);
    ExistingMonitor attached{};
    const bool headPresent = FindMttVirtualMonitor(attached);

    if (cur == static_cast<int>(count)) {
        if (count == 0 && !headPresent) {
            r.ok = true;
            r.changed = false;
            r.detail = "already count=0 (no MTT head)";
            return r;
        }
        if (count >= 1 && headPresent) {
            r.ok = true;
            r.changed = false;
            r.detail = "already count=" + std::to_string(count) + " (MTT head present)";
            return r;
        }
    }

    if (cur != static_cast<int>(count)) {
        const std::string rewritten = ReplaceMonitorCountXml(xml, count);
        if (rewritten.empty()) {
            r.detail = "malformed XML (no <monitors>)";
            return r;
        }
        if (!WriteFileUtf8Atomic(r.path, rewritten)) {
            const DWORD err = GetLastError();
            char buf[96];
            snprintf(buf, sizeof(buf), "write failed (err=%lu)%s",
                     static_cast<unsigned long>(err),
                     err == ERROR_ACCESS_DENIED ? " access denied" : "");
            r.detail = buf;
            return r;
        }
    }

    std::string reloadDetail;
    const bool reloaded = ReloadMttVddDevice(reloadDetail);
    bool settled = false;
    if (count == 0) {
        settled = WaitForMttMonitor(/*wantPresent=*/false, 4000);
    } else {
        // Attached OR merely present (CDS-detachable) is enough; Ensure attaches.
        settled = WaitForMttMonitor(/*wantPresent=*/true, 3000);
        if (!settled) {
            ExistingMonitor any{};
            settled = FindMttVirtualMonitorDevice(any, /*requireAttached=*/false);
        }
    }

    r.changed = true;
    r.added = 0;
    char buf[192];
    snprintf(buf, sizeof(buf), "set monitors count=%u (was %d); %s; settle=%s",
             count, cur, reloadDetail.c_str(), settled ? "ok" : "timeout");
    r.detail = buf;
    if (!reloaded)
        r.detail += " (reload soft-failed)";
    r.ok = settled;
    return r;
}

bool SelfElevateSetMonitorCount(uint32_t count)
{
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
        return false;
    std::wstring args = L"--set-mtt-monitor-count " + std::to_wstring(count);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr)
        return false;

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}

bool EnsureMttVddAttached(const std::string& logTag)
{
    if (g_mttShuttingDown.load()) {
        Logf(logTag, "mtt: exiting - not attaching VDD\n");
        return false;
    }
    // Join any grace worker BEFORE taking the topology lock (it locks in TearDown).
    CancelPendingMttVddTeardown();
    std::lock_guard<std::recursive_mutex> topoLock(MttTopoMutex());
    if (g_mttShuttingDown.load())
        return false;

    ExistingMonitor mon{};
    if (FindMttVirtualMonitor(mon)) {
        // Already on the desktop. Position is applied at most once per
        // attachment: if we have not positioned this head yet (left over from a
        // previous run / reboot, or re-extended outside the app) adopt it and
        // apply the saved layout once. Otherwise (ACCESS_LOST / rotate / resize
        // rebuild, hello-again, link-flap reconnect) leave the user's layout
        // alone - re-applying here is what snapped Display Settings moves back.
        if (!g_mttPositionedThisAttach.exchange(true)) {
            Logf(logTag, "mtt: adopting MTT head already on the desktop (%ls) - applying saved layout once\n",
                 mon.deviceName.c_str());
            (void)RestoreMttDisplayTopology(logTag);
        } else {
            Logf(logTag, "mtt: VDD already attached (%ls) - keeping current layout\n", mon.deviceName.c_str());
        }
        return true;
    }

    // Make sure the driver is willing to expose a head.
    MttEnsureResult r = SetMonitorCount(1);
    if (!r.ok && r.detail.find("access denied") != std::string::npos) {
        Logf(logTag, "mtt: set count=1 needs admin, elevating...\n");
        if (!SelfElevateSetMonitorCount(1)) {
            Logf(logTag, "mtt: elevate failed or UAC declined - VDD may be missing\n");
            return false;
        }
        r = SetMonitorCount(1);
    }

    if (FindMttVirtualMonitor(mon)) {
        Logf(logTag, "mtt: VDD attached (%ls) [%s]\n", mon.deviceName.c_str(), r.detail.c_str());
        g_mttPositionedThisAttach = true;
        (void)RestoreMttDisplayTopology(logTag);
        return true;
    }

    // CDS-detached (or not yet on desktop): find the MTT device and attach it.
    if (FindMttVirtualMonitorDevice(mon, /*requireAttached=*/false)) {
        // Saved mode if this iPad's EDID offers it, else the preferred
        // (hello-native) mode. Restore below reasserts position from
        // mtt_display.json.
        const std::vector<MttMode> offered = CurrentMttModes();
        uint32_t attachW = offered[0].width, attachH = offered[0].height, attachHz = offered[0].hz;
        int attachX = 0, attachY = 0;
        bool havePos = false;
        {
            MttTopology saved{};
            if (LoadTopologyFile(saved) && saved.valid && saved.width > 0 && saved.height > 0) {
                if (ModeOffered(offered, saved.width, saved.height)) {
                    attachW = saved.width;
                    attachH = saved.height;
                    if (saved.hz != 0)
                        attachHz = saved.hz;
                } else {
                    Logf(logTag, "mtt: saved mode %ux%u not offered for this iPad - using preferred %ux%u\n",
                         saved.width, saved.height, attachW, attachH);
                }
                attachX = saved.x;
                attachY = saved.y;
                havePos = true;
            }
        }
        if (havePos && SavedRectOverlapsOthers(mon.deviceName, attachX, attachY, attachW, attachH)) {
            Logf(logTag, "mtt: saved position (%d,%d) overlaps another display - letting Windows place the head\n",
                 attachX, attachY);
            havePos = false;
        }
        Logf(logTag, "mtt: attaching detached head %ls at %ux%u@%u%s\n", mon.deviceName.c_str(),
             attachW, attachH, attachHz, havePos ? " (saved position)" : "");
        if (AttachMonitorToDesktop(mon.deviceName, attachW, attachH, attachHz, havePos, attachX,
                                   attachY) &&
            WaitForMttMonitor(/*wantPresent=*/true, 5000)) {
            if (FindMttVirtualMonitor(mon)) {
                Logf(logTag, "mtt: VDD attached via CDS (%ls)\n", mon.deviceName.c_str());
                g_mttPositionedThisAttach = true;
                (void)RestoreMttDisplayTopology(logTag);
                return true;
            }
        }
    }

    // If a prior teardown DICS_DISABLE'd the MTT1337 monitor, re-enable it.
    {
        static const GUID kMonitorClass = {
            0x4d36e96e, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
        HDEVINFO devInfo = SetupDiGetClassDevsW(&kMonitorClass, nullptr, nullptr, 0);
        bool enabled = false;
        if (devInfo != INVALID_HANDLE_VALUE) {
            SP_DEVINFO_DATA did{};
            did.cbSize = sizeof(did);
            for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &did); ++i) {
                wchar_t instanceId[256];
                if (!SetupDiGetDeviceInstanceIdW(devInfo, &did, instanceId, 256, nullptr))
                    continue;
                if (wcsstr(instanceId, L"MTT1337") == nullptr)
                    continue;
                SP_PROPCHANGE_PARAMS pcp{};
                pcp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
                pcp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
                pcp.StateChange = DICS_ENABLE;
                pcp.Scope = DICS_FLAG_GLOBAL;
                pcp.HwProfile = 0;
                if (SetupDiSetClassInstallParamsW(devInfo, &did, &pcp.ClassInstallHeader, sizeof(pcp)) &&
                    SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devInfo, &did))
                    enabled = true;
            }
            SetupDiDestroyDeviceInfoList(devInfo);
        }
        if (enabled) {
            Logf(logTag, "mtt: re-enabled MTT1337 monitor node\n");
            (void)WaitForMttMonitor(/*wantPresent=*/true, 8000);
            if (FindMttVirtualMonitorDevice(mon, /*requireAttached=*/false)) {
                const std::vector<MttMode> offered = CurrentMttModes();
                uint32_t aw = offered[0].width, ah = offered[0].height, ahz = offered[0].hz;
                int ax = 0, ay = 0;
                bool hp = false;
                MttTopology saved{};
                if (LoadTopologyFile(saved) && saved.valid && saved.width > 0 && saved.height > 0) {
                    if (ModeOffered(offered, saved.width, saved.height)) {
                        aw = saved.width;
                        ah = saved.height;
                        if (saved.hz != 0)
                            ahz = saved.hz;
                    }
                    ax = saved.x;
                    ay = saved.y;
                    hp = !SavedRectOverlapsOthers(mon.deviceName, ax, ay, aw, ah);
                }
                (void)AttachMonitorToDesktop(mon.deviceName, aw, ah, ahz, hp, ax, ay);
                (void)WaitForMttMonitor(/*wantPresent=*/true, 5000);
            }
        }
    }

    if (FindMttVirtualMonitor(mon)) {
        Logf(logTag, "mtt: VDD attached (%ls) [%s]\n", mon.deviceName.c_str(), r.detail.c_str());
        g_mttPositionedThisAttach = true;
        (void)RestoreMttDisplayTopology(logTag);
        return true;
    }
    Logf(logTag, "mtt: ensure VDD failed (%s)\n", r.detail.c_str());
    return false;
}
bool TearDownMttVdd(const std::string& logTag)
{
    std::lock_guard<std::recursive_mutex> topoLock(MttTopoMutex());
    ExistingMonitor mon{};
    if (!FindMttVirtualMonitor(mon) && ReadMonitorCount() <= 0) {
        Logf(logTag, "mtt: VDD already down\n");
        return true;
    }

    // Prefer CDS detach: reliably removes the head from Display Settings.
    // Leave monitors/<count> alone so the next hello can CDS-attach without a
    // driver reload (count=0 + pnputil was slow and flaky to reverse).
    if (FindMttVirtualMonitor(mon)) {
        Logf(logTag, "mtt: detaching head %ls from desktop\n", mon.deviceName.c_str());
        if (!DetachMonitorFromDesktop(mon.deviceName))
            Logf(logTag, "mtt: CDS detach request failed on %ls\n", mon.deviceName.c_str());
        (void)WaitForMttMonitor(/*wantPresent=*/false, 4000);
    }

    MttEnsureResult r;
    r.ok = !FindMttVirtualMonitor(mon);
    r.changed = true;
    r.detail = r.ok ? "cds detach" : "cds detach incomplete";

    if (!FindMttVirtualMonitor(mon)) {
        Logf(logTag, "mtt: VDD torn down [%s]\n", r.detail.c_str());
        g_mttPositionedThisAttach = false;
        // Make the detach stick: commit the MTT-less topology to the CCD
        // database, otherwise Windows may re-extend the head on the next
        // display re-enumeration or reboot ("still connected after exit").
        std::string persist;
        if (PersistActiveTopologyToDatabase(persist))
            Logf(logTag, "mtt: %s\n", persist.c_str());
        else
            Logf(logTag, "mtt: persist detached topology failed (%s)\n", persist.c_str());
        return true;
    }

    // Last resort: SetupAPI disable of the MTT1337 monitor node.
    {
        static const GUID kMonitorClass = {
            0x4d36e96e, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
        HDEVINFO devInfo = SetupDiGetClassDevsW(&kMonitorClass, nullptr, nullptr, DIGCF_PRESENT);
        bool disabled = false;
        if (devInfo != INVALID_HANDLE_VALUE) {
            SP_DEVINFO_DATA did{};
            did.cbSize = sizeof(did);
            for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &did); ++i) {
                wchar_t instanceId[256];
                if (!SetupDiGetDeviceInstanceIdW(devInfo, &did, instanceId, 256, nullptr))
                    continue;
                if (wcsstr(instanceId, L"MTT1337") == nullptr)
                    continue;
                SP_PROPCHANGE_PARAMS pcp{};
                pcp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
                pcp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
                pcp.StateChange = DICS_DISABLE;
                pcp.Scope = DICS_FLAG_GLOBAL;
                pcp.HwProfile = 0;
                if (SetupDiSetClassInstallParamsW(devInfo, &did, &pcp.ClassInstallHeader, sizeof(pcp)) &&
                    SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devInfo, &did))
                    disabled = true;
            }
            SetupDiDestroyDeviceInfoList(devInfo);
        }
        if (disabled && WaitForMttMonitor(/*wantPresent=*/false, 4000)) {
            Logf(logTag, "mtt: VDD torn down via SetupAPI disable [%s]\n", r.detail.c_str());
            g_mttPositionedThisAttach = false;
            return true;
        }
    }

    Logf(logTag, "mtt: teardown failed (still attached) [%s]\n", r.detail.c_str());
    return false;
}

void CancelPendingMttVddTeardown()
{
    MttGraceState& g = MttGrace();
    std::thread toJoin;
    {
        std::lock_guard<std::mutex> lock(g.mu);
        ++g.epoch;
        g.pending = false;
        g.cv.notify_all();
        if (g.worker.joinable())
            toJoin = std::move(g.worker);
    }
    if (toJoin.joinable())
        toJoin.join();
}

void RequestMttVddTeardown(const std::string& logTag, MttTeardownReason reason, int graceMs)
{
    if (graceMs <= 0)
        graceMs = kMttLinkLossGraceMs;
    if (reason == MttTeardownReason::UserInitiated) {
        CancelPendingMttVddTeardown();
        Logf(logTag, "mtt: user-initiated teardown\n");
        (void)SaveMttDisplayTopology(logTag);
        (void)TearDownMttVdd(logTag);
        return;
    }

    MttGraceState& g = MttGrace();
    uint64_t myEpoch = 0;
    {
        std::lock_guard<std::mutex> lock(g.mu);
        ++g.epoch;
        myEpoch = g.epoch;
        g.pending = true;
        g.logTag = logTag;
        g.cv.notify_all();
    }

    std::thread prev;
    {
        std::lock_guard<std::mutex> lock(g.mu);
        if (g.worker.joinable())
            prev = std::move(g.worker);
    }
    if (prev.joinable())
        prev.join();

    {
        std::lock_guard<std::mutex> lock(g.mu);
        if (!g.pending || g.epoch != myEpoch)
            return;
        Logf(logTag, "mtt: link-loss grace %d ms (sticky VDD)\n", graceMs);
        g.worker = std::thread([myEpoch, graceMs]() {
            MttGraceState& gs = MttGrace();
            std::string tag;
            {
                std::unique_lock<std::mutex> lock(gs.mu);
                tag = gs.logTag;
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(graceMs);
                while (gs.pending && gs.epoch == myEpoch) {
                    if (gs.cv.wait_until(lock, deadline) == std::cv_status::timeout)
                        break;
                }
                if (!gs.pending || gs.epoch != myEpoch) {
                    Logf(tag.empty() ? "mtt" : tag, "mtt: link-loss grace cancelled\n");
                    return;
                }
                gs.pending = false;
            }
            Logf(tag.empty() ? "mtt" : tag, "mtt: link-loss grace expired - tearing down VDD\n");
            (void)TearDownMttVdd(tag.empty() ? "mtt" : tag);
        });
    }
}

bool SaveMttDisplayTopology(const std::string& logTag)
{
    std::lock_guard<std::recursive_mutex> topoLock(MttTopoMutex());
    static std::string s_lastSkip; // log each distinct skip reason once (poll runs every 2s)

    ExistingMonitor mon{};
    if (!FindMttVirtualMonitor(mon))
        return false; // absent / detached ("Show only on 1"): keep the good saved layout

    // Only save when the head is attached AND active in the CCD topology with a
    // valid, non-cloned source mode. Anything else (inactive, mid-reconfigure,
    // duplicated) would overwrite a good saved position with junk.
    RECT live{};
    std::string why;
    if (!QueryMttActiveRect(mon.deviceName, live, &why)) {
        if (why != s_lastSkip) {
            Logf(logTag, "mtt: not saving layout (%s)\n", why.c_str());
            s_lastSkip = why;
        }
        return false;
    }
    s_lastSkip.clear();

    const int x = live.left;
    const int y = live.top;
    uint32_t w = static_cast<uint32_t>(live.right - live.left);
    uint32_t h = static_cast<uint32_t>(live.bottom - live.top);
    uint32_t hz = 0;

    MttTopology prev{};
    const bool havePrev = LoadTopologyFile(prev) && prev.valid;

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mon.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
        hz = dm.dmDisplayFrequency;
        if (dm.dmDisplayOrientation != DMDO_DEFAULT && havePrev && prev.width && prev.height) {
            // Restore always applies landscape; keep the last landscape mode and
            // only take the new position from a rotated head.
            w = prev.width;
            h = prev.height;
            hz = prev.hz;
        }
    }

    if (havePrev && prev.x == x && prev.y == y && prev.width == w && prev.height == h && prev.hz == hz)
        return true;

    MttTopology t;
    t.x = x;
    t.y = y;
    t.width = w;
    t.height = h;
    t.hz = hz;
    t.deviceId = NarrowPath(mon.deviceId);
    t.valid = true;
    if (!WriteTopologyFile(t)) {
        Logf(logTag, "mtt: failed to save position (%d,%d) %ux%u@%u\n", x, y, w, h, hz);
        return false;
    }
    Logf(logTag, "mtt: saved position (%d,%d) %ux%u@%u\n", x, y, w, h, hz);
    return true;
}

bool SaveMttDisplayTopologyIfUserChange(const std::string& logTag)
{
    // A change that lands right after one of our own applies (attach / restore
    // / mode fix) is ours, not the user's - the streaming poll picks up any
    // genuine user change once the settle window has passed.
    if (RecentSelfDisplayChange(kSelfApplySettleMs))
        return false;
    return SaveMttDisplayTopology(logTag);
}

bool QueryMttSavedMode(uint32_t& width, uint32_t& height, uint32_t& hz)
{
    MttTopology t{};
    if (!LoadTopologyFile(t) || !t.valid || t.width == 0 || t.height == 0)
        return false;
    if (!ModeOffered(CurrentMttModes(), t.width, t.height))
        return false; // e.g. saved on another iPad size: caller uses the preferred mode
    width = t.width;
    height = t.height;
    hz = t.hz;
    return true;
}

bool RestoreMttDisplayTopology(const std::string& logTag)
{
    std::lock_guard<std::recursive_mutex> topoLock(MttTopoMutex());
    MttTopology t{};
    if (!LoadTopologyFile(t) || !t.valid)
        return false;

    ExistingMonitor mon{};
    if (!FindMttVirtualMonitor(mon))
        return false;

    // FindMttVirtualMonitor matched MTT1337 / VDD by MTT (not DISPLAY index).
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    bool haveDm = EnumDisplaySettingsW(mon.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm) != 0;

    bool modeOk = true;
    if (t.width > 0 && t.height > 0) {
        // Only reapply the saved mode if this iPad's EDID offers it; otherwise
        // use the new preferred mode. Position restore below is unchanged.
        const std::vector<MttMode> offered = CurrentMttModes();
        if (!ModeOffered(offered, t.width, t.height)) {
            Logf(logTag, "mtt: saved mode %ux%u not offered for this iPad - using preferred %ux%u\n", t.width,
                 t.height, offered[0].width, offered[0].height);
            t.width = offered[0].width;
            t.height = offered[0].height;
            t.hz = offered[0].hz;
        }
        const uint32_t applyHz = t.hz ? t.hz : 60u;
        const bool already =
            haveDm && dm.dmPelsWidth == t.width && dm.dmPelsHeight == t.height &&
            (t.hz == 0 || dm.dmDisplayFrequency == static_cast<DWORD>(applyHz)) &&
            dm.dmDisplayOrientation == DMDO_DEFAULT;
        if (already) {
            Logf(logTag, "mtt: restoring saved mode %ux%u@%u (already current)\n", t.width, t.height,
                 applyHz);
        } else if (EnsureMonitorMode(mon.deviceName, t.width, t.height, applyHz)) {
            Logf(logTag, "mtt: restoring saved mode %ux%u@%u\n", t.width, t.height, applyHz);
            haveDm = EnumDisplaySettingsW(mon.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm) != 0;
        } else {
            Logf(logTag, "mtt: restore saved mode %ux%u@%u failed on %ls\n", t.width, t.height, applyHz,
                 mon.deviceName.c_str());
            modeOk = false;
        }
    }

    bool posOk = true;
    const uint32_t curW = haveDm ? dm.dmPelsWidth : t.width;
    const uint32_t curH = haveDm ? dm.dmPelsHeight : t.height;
    if (haveDm && dm.dmPosition.x == t.x && dm.dmPosition.y == t.y) {
        // already there
    } else if (SavedRectOverlapsOthers(mon.deviceName, t.x, t.y, curW, curH)) {
        // e.g. a stale (0,0) would land on top of the primary and Windows
        // would reshuffle everything - keep Windows' placement instead.
        Logf(logTag, "mtt: saved position (%d,%d) overlaps another display - keeping current placement\n",
             t.x, t.y);
    } else if (!SetMonitorDesktopPosition(mon.deviceName, t.x, t.y)) {
        Logf(logTag, "mtt: restore position (%d,%d) failed on %ls\n", t.x, t.y, mon.deviceName.c_str());
        posOk = false;
    } else {
        Logf(logTag, "mtt: restored position (%d,%d)\n", t.x, t.y);
    }
    return modeOk && posOk;
}

void PollMttDisplayTopology(const std::string& logTag, POINT& lastObserved)
{
    // Save-only observer: never restores. SaveMttDisplayTopology no-ops when
    // nothing changed vs mtt_display.json and refuses inactive/absent heads.
    if (SaveMttDisplayTopologyIfUserChange(logTag)) {
        ExistingMonitor mon{};
        RECT r{};
        if (FindMttVirtualMonitor(mon) && QueryMttActiveRect(mon.deviceName, r, nullptr))
            lastObserved = {r.left, r.top};
    }
}

void BeginMttShutdown()
{
    g_mttShuttingDown = true;
}

bool ShutdownMttVddForExit(const std::string& logTag)
{
    g_mttShuttingDown = true;
    CancelPendingMttVddTeardown(); // outside the topology lock (joins grace worker)
    std::lock_guard<std::recursive_mutex> topoLock(MttTopoMutex());
    ExistingMonitor mon{};
    if (!FindMttVirtualMonitor(mon)) {
        Logf(logTag, "mtt: exit - no MTT head on the desktop\n");
        return true;
    }
    Logf(logTag, "mtt: exit - tearing down MTT head %ls\n", mon.deviceName.c_str());
    (void)SaveMttDisplayTopology(logTag);
    return TearDownMttVdd(logTag);
}

void AdoptStaleMttHeadAtStartup(const std::string& logTag, int graceMs)
{
    ExistingMonitor mon{};
    if (!FindMttVirtualMonitor(mon))
        return;
    Logf(logTag,
         "mtt: MTT head %ls already on the desktop at startup (left over from a previous run) - "
         "adopting; removed in %d ms unless an iPad connects\n",
         mon.deviceName.c_str(), graceMs);
    g_mttPositionedThisAttach = false; // first Ensure applies the saved layout once
    RequestMttVddTeardown(logTag, MttTeardownReason::LinkLoss, graceMs);
}

} // namespace od
