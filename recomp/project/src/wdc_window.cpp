// Janela do modo interativo (ver wdc_window.h).
#include "wdc_window.h"
#include "wdc_config.h"
#include "wdc_present.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
constexpr UINT WM_WDC_PRESENT = WM_APP + 1;
constexpr UINT WM_WDC_TITLE = WM_APP + 2;
constexpr UINT WM_WDC_QUIT = WM_APP + 3;

std::thread g_thread;
HWND g_hwnd = nullptr;
std::atomic<bool> g_closed{false};
std::atomic<bool> g_keys[256];
std::mutex g_mu;
// Ultimo quadro entregue pelo renderizador (BGRA, de cima para baixo). IMUTAVEL depois de publicado: a pintura GDI (lenta em tela cheia) trabalha numa
// copia do ponteiro e NAO segura g_mu, senao a thread do RDP (que entrega os quadros e tambem avisa o jogo no SYNC_FULL) ficava presa esperando a janela
// e o jogo congelava (tarefas graficas caindo para ~1/s) ao ir para tela cheia.
std::shared_ptr<const uint32_t[]> g_frame;
unsigned g_bw = 0, g_bh = 0;                 // dimensoes da imagem (antes do esticamento do campo)
std::atomic<bool> g_present_pending{false};  // ha um WM_WDC_PRESENT na fila: nao empilhar mais um por quadro
std::string g_title, g_suffix;
bool g_fullscreen = false;
WINDOWPLACEMENT g_prev_place = [] { WINDOWPLACEMENT p{}; p.length = sizeof(p); return p; }();
LONG g_prev_style = 0;

void toggle_fullscreen(HWND h) {
    if (!g_fullscreen) {
        g_prev_style = GetWindowLongW(h, GWL_STYLE);
        GetWindowPlacement(h, &g_prev_place);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(h, GWL_STYLE, g_prev_style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = true;
    } else {
        SetWindowLongW(h, GWL_STYLE, g_prev_style);
        SetWindowPlacement(h, &g_prev_place);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

void paint(HWND h) {
    std::shared_ptr<const uint32_t[]> frame;
    unsigned fw = 0, fh = 0;
    { std::lock_guard<std::mutex> l(g_mu); frame = g_frame; fw = g_bw; fh = g_bh; }   // so o ponteiro: a pintura abaixo roda sem o mutex
    const auto t_start = std::chrono::steady_clock::now();
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (frame && fw && fh && cw > 0 && ch > 0) {
        // proporcao de tela 4:3 (campo de 640x240 vira 640x480)
        const int dh = fh < fw / 2 ? (int)fh * 2 : (int)fh;
        int w = cw, hh = (int)((long long)cw * dh / (int)fw);
        if (hh > ch) { hh = ch; w = (int)((long long)ch * (int)fw / dh); }
        const int x = (cw - w) / 2, y = (ch - hh) / 2;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = (LONG)fw;
        bi.bmiHeader.biHeight = -(LONG)fh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, x, y, w, hh, 0, 0, (int)fw, (int)fh, frame.get(), &bi, DIB_RGB_COLORS, SRCCOPY);
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT r1{0, 0, cw, y}, r2{0, y + hh, cw, ch}, r3{0, y, x, y + hh}, r4{x + w, y, cw, y + hh};
        FillRect(dc, &r1, black); FillRect(dc, &r2, black); FillRect(dc, &r3, black); FillRect(dc, &r4, black);
    } else {
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    EndPaint(h, &ps);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_start).count();
    static int slow_prints = 0;
    if (ms > 40.0 && slow_prints < 20) { slow_prints++; std::printf("[win] pintura lenta: %.0f ms (cliente %dx%d, imagem %ux%u)\n", ms, cw, ch, fw, fh); }
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (w < 256) g_keys[w] = true;
        if (w == VK_ESCAPE) g_closed = true;
        if (w == VK_F11 && !(l & (1 << 30))) toggle_fullscreen(h);
        if (m == WM_KEYDOWN) return 0;
        break;   // Alt+F4 etc.: DefWindowProc
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (w < 256) g_keys[w] = false;
        if (m == WM_KEYUP) return 0;
        break;
    case WM_KILLFOCUS:
        for (auto& k : g_keys) k = false;
        return 0;
    case WM_CLOSE:
        g_closed = true;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (wdc_present_active()) { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }   // o apresentador em GPU desenha; so valida a regiao
        paint(h);
        return 0;
    case WM_WDC_PRESENT:
        g_present_pending = false;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_WDC_TITLE: {
        std::string t;
        { std::lock_guard<std::mutex> lk(g_mu); t = g_title + g_suffix; }
        SetWindowTextA(h, t.c_str());
        return 0;
    }
    case WM_WDC_QUIT:
        DestroyWindow(h);   // so a thread da janela pode destrui-la
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void window_thread(std::string title, int cw, int ch, std::promise<bool> ready) {
    SetProcessDPIAware();
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"WdcRecompWindow";
    RegisterClassW(&wc);
    RECT r{0, 0, cw, ch};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    const std::wstring wt(title.begin(), title.end());
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, wt.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (g_hwnd && wdc_cfg().fullscreen) toggle_fullscreen(g_hwnd);   // display.fullscreen: abre em tela cheia (antes de o apresentador criar a swapchain)
    ready.set_value(g_hwnd != nullptr);
    if (!g_hwnd) return;
    SetForegroundWindow(g_hwnd);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_hwnd = nullptr;
}
}  // namespace

bool wdc_window_create(const char* title, int client_w, int client_h) {
    { std::lock_guard<std::mutex> l(g_mu); g_title = title; }
    std::promise<bool> p;
    auto f = p.get_future();
    g_thread = std::thread(window_thread, std::string(title), client_w, client_h, std::move(p));
    const bool ok = f.get();
    if (ok && wdc_cfg().backend == WdcBackend::D3D11 && !wdc_present_init(g_hwnd, wdc_cfg()))
        std::printf("[window] apresentador D3D11 indisponivel: usando o GDI (sem v-sync)\n");
    return ok;
}

void wdc_window_destroy() {
    wdc_present_print_stats();
    wdc_present_shutdown();   // antes de destruir a janela: o apresentador usa o HWND
    if (g_hwnd) PostMessageW(g_hwnd, WM_WDC_QUIT, 0, 0);
    if (g_thread.joinable()) g_thread.join();
}

void wdc_window_present(const uint8_t* rgba, unsigned w, unsigned h, int64_t t_vi_us) {
    if (!w || !h) return;
    if (wdc_present_active()) { wdc_present_frame(rgba, w, h, t_vi_us); return; }   // apresentador em GPU (v-sync, escala por shader)
    // GDI (fallback): converte para um buffer novo SEM o mutex e so entao o publica (troca de ponteiro): esta funcao roda na thread do RDP e nunca espera a pintura.
    const size_t n = (size_t)w * h;
    std::shared_ptr<uint32_t[]> buf = std::make_shared_for_overwrite<uint32_t[]>(n);
    for (size_t i = 0; i < n; i++) {
        const uint8_t* p = rgba + i * 4;
        buf[i] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];   // BGRA em memoria = 0x00RRGGBB
    }
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_frame = std::move(buf);
        g_bw = w;
        g_bh = h;
    }
    if (g_hwnd && !g_present_pending.exchange(true)) PostMessageW(g_hwnd, WM_WDC_PRESENT, 0, 0);
}

bool wdc_window_closed() { return g_closed.load(); }
bool wdc_window_key(int vk) { return vk >= 0 && vk < 256 && g_keys[vk].load(); }

void wdc_window_set_title_suffix(const char* s) {
    { std::lock_guard<std::mutex> l(g_mu); g_suffix = s ? s : ""; }
    if (g_hwnd) PostMessageW(g_hwnd, WM_WDC_TITLE, 0, 0);
}
