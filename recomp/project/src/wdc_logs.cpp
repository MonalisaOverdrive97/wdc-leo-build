#include "wdc_logs.h"

#include "wdc_version.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <windows.h>
#include <dxgi.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <io.h>
#include <shellapi.h>

namespace fs = std::filesystem;

namespace {
bool writable(const fs::path& d) {
    std::error_code ec;
    fs::create_directories(d, ec);
    const fs::path t = d / ".write_test";
    FILE* f = _wfopen(t.c_str(), L"w");
    const bool ok = f != nullptr;
    if (f) std::fclose(f);
    fs::remove(t, ec);
    return ok;
}

std::string reg_string(HKEY root, const char* key, const char* name) {
    HKEY h;
    if (RegOpenKeyExA(root, key, 0, KEY_READ | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS) return {};
    char buf[512] = {};
    DWORD sz = sizeof buf - 1, type = 0;
    std::string r;
    if (RegQueryValueExA(h, name, nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS && type == REG_SZ) r = buf;
    RegCloseKey(h);
    return r;
}

std::string utf8(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

BOOL CALLBACK mon_cb(HMONITOR hm, HDC, LPRECT, LPARAM) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(hm, &mi)) return TRUE;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    int hz = 0;
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) hz = (int)dm.dmDisplayFrequency;
    UINT dx = 0, dy = 0;
    using GetDpiForMonitor_t = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE sh = LoadLibraryW(L"shcore.dll")) {
        if (auto f = (GetDpiForMonitor_t)GetProcAddress(sh, "GetDpiForMonitor")) f(hm, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy);
        FreeLibrary(sh);
    }
    std::printf("[sys] monitor %s%s: %ldx%ld a %d Hz, area util %ldx%ld, DPI %u (%u%%)\n", utf8(mi.szDevice).c_str(), (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (principal)" : "",
                mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, hz, mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top, dx,
                dx ? dx * 100 / 96 : 0);
    return TRUE;
}
}  // namespace

fs::path wdc_logs_dir() {
    const char* la = std::getenv("LOCALAPPDATA");
    if (!la || !*la) return {};
    const fs::path d = fs::path(la) / "WorldDriverChampionship" / "logs";
    return writable(d) ? d : fs::path{};
}

fs::path wdc_logs_start(const fs::path& fallback) {
    fs::path dir = wdc_logs_dir();
    if (dir.empty()) {
        dir = fallback / "logs";
        if (!writable(dir)) return {};
    }
    std::error_code ec;
    // guarda so os 9 logs mais recentes (o nome tem a data) para o novo ser o 10o
    std::vector<fs::path> old;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("wdc_log_", 0) == 0 && e.path().extension() == ".txt") old.push_back(e.path());
    }
    std::sort(old.begin(), old.end());
    while (old.size() > 9) { fs::remove(old.front(), ec); old.erase(old.begin()); }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char name[64];
    std::snprintf(name, sizeof name, "wdc_log_%04d%02d%02d-%02d%02d%02d.txt", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const fs::path p = dir / name;
    if (!_wfreopen(p.c_str(), L"w", stdout)) return {};
    _dup2(_fileno(stdout), _fileno(stderr));
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    return p;
}

// O cabecalho precisa das resolucoes e do DPI REAIS dos monitores: sem declarar consciencia de DPI o Windows devolve valores virtualizados (ex.: 1707x1067 a 96 dpi num 2560x1600 a 150%).
// A consciencia de DPI vale para o processo todo (a primeira chamada vence); o setup e a janela do jogo usam a mesma por-monitor.
static void dpi_awareness_early() {
    using SetCtx_t = BOOL(WINAPI*)(HANDLE);
    if (auto f = (SetCtx_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")) { if (f((HANDLE)-4 /*PER_MONITOR_AWARE_V2*/)) return; }
    using SetAw_t = HRESULT(WINAPI*)(int);
    if (HMODULE sh = LoadLibraryW(L"shcore.dll")) {
        auto f = (SetAw_t)GetProcAddress(sh, "SetProcessDpiAwareness");
        if (f && SUCCEEDED(f(2 /*PROCESS_PER_MONITOR_DPI_AWARE*/))) return;
        FreeLibrary(sh);
    }
    SetProcessDPIAware();
}

void wdc_logs_system_info(const fs::path& exe_dir, const fs::path& config_dir) {
    dpi_awareness_early();
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::printf("[sys] %s  (gerado em %04d-%02d-%02d %02d:%02d:%02d)\n", WDC_BUILD_LABEL, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    // Windows
    using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof vi;
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto f = (RtlGetVersion_t)GetProcAddress(nt, "RtlGetVersion")) f(&vi);
    const std::string prod = reg_string(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName");
    const std::string disp = reg_string(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion");
    const bool wine = GetModuleHandleW(L"ntdll.dll") && GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    std::printf("[sys] Windows: %s %s (versao %lu.%lu, compilacao %lu)%s\n", prod.c_str(), disp.c_str(), vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber, wine ? " [Wine]" : "");
    // CPU / memoria
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    const std::string cpu = reg_string(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString");
    std::printf("[sys] CPU: %s (%lu processadores logicos)\n", cpu.c_str(), si.dwNumberOfProcessors);
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms))
        std::printf("[sys] memoria: %llu MB fisica (%llu MB livres), %llu MB de limite de commit (%llu MB livres)\n", (unsigned long long)(ms.ullTotalPhys >> 20), (unsigned long long)(ms.ullAvailPhys >> 20),
                    (unsigned long long)(ms.ullTotalPageFile >> 20), (unsigned long long)(ms.ullAvailPageFile >> 20));
    // GPUs (DXGI)
    IDXGIFactory1* fac = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac)) && fac) {
        IDXGIAdapter1* ad = nullptr;
        for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d{};
            ad->GetDesc1(&d);
            LARGE_INTEGER umd{};
            const bool have_umd = SUCCEEDED(ad->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
            char drv[64] = "?";
            if (have_umd) std::snprintf(drv, sizeof drv, "%u.%u.%u.%u", (unsigned)(umd.QuadPart >> 48) & 0xFFFF, (unsigned)(umd.QuadPart >> 32) & 0xFFFF, (unsigned)(umd.QuadPart >> 16) & 0xFFFF, (unsigned)umd.QuadPart & 0xFFFF);
            std::printf("[sys] placa de video %u: %s (fornecedor 0x%04X, dispositivo 0x%04X), VRAM dedicada %llu MB, compartilhada %llu MB, driver %s%s\n", i, utf8(d.Description).c_str(), d.VendorId, d.DeviceId,
                        (unsigned long long)(d.DedicatedVideoMemory >> 20), (unsigned long long)(d.SharedSystemMemory >> 20), drv, (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " [software]" : "");
            ad->Release();
        }
        fac->Release();
    }
    // monitores
    std::printf("[sys] DPI do sistema: %d\n", [] { HDC dc = GetDC(nullptr); const int v = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc); return v; }());
    EnumDisplayMonitors(nullptr, nullptr, mon_cb, 0);
    // audio padrao
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* en = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) && en) {
            IMMDevice* dev = nullptr;
            if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev) {
                IPropertyStore* ps = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) std::printf("[sys] saida de audio padrao: %s\n", utf8(pv.pwszVal).c_str());
                    PropVariantClear(&pv);
                    ps->Release();
                }
                dev->Release();
            } else {
                std::printf("[sys] saida de audio padrao: NENHUM dispositivo de reproducao\n");
            }
            en->Release();
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }
    // processo / pastas
    HANDLE tok = nullptr;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION te{};
        DWORD sz = 0;
        if (GetTokenInformation(tok, TokenElevation, &te, sizeof te, &sz)) elevated = te.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    wchar_t loc[LOCALE_NAME_MAX_LENGTH] = L"?";
    GetUserDefaultLocaleName(loc, LOCALE_NAME_MAX_LENGTH);
    std::printf("[sys] executavel: %s | pasta de config: %s | administrador: %s | idioma: %s\n", exe_dir.string().c_str(), config_dir.string().c_str(), elevated ? "sim" : "nao", utf8(loc).c_str());
}

bool wdc_logs_open_folder() {
    fs::path d = wdc_logs_dir();
    if (d.empty()) return false;
    return (INT_PTR)ShellExecuteW(nullptr, L"open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
}
