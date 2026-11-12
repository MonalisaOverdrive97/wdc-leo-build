// Entrada real (ver wdc_input.h).
#include "wdc_input.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "wdc_joy.h"
#include "wdc_window.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
// bits do XINPUT_GAMEPAD.wButtons (xinput.h)
constexpr uint16_t XB_DPAD_UP = 0x0001, XB_DPAD_DOWN = 0x0002, XB_DPAD_LEFT = 0x0004, XB_DPAD_RIGHT = 0x0008, XB_START = 0x0010,
                   XB_LB = 0x0100, XB_RB = 0x0200, XB_A = 0x1000, XB_B = 0x2000, XB_X = 0x4000, XB_Y = 0x8000;
// bits do OSContPad.button
constexpr uint16_t N_A = 0x8000, N_B = 0x4000, N_Z = 0x2000, N_START = 0x1000, N_DU = 0x0800, N_DD = 0x0400, N_DL = 0x0200, N_DR = 0x0100,
                   N_L = 0x0020, N_R = 0x0010, N_CU = 0x0008, N_CD = 0x0004, N_CL = 0x0002, N_CR = 0x0001;

struct XState { DWORD packet; uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; };
using XInputGetState_t = DWORD(WINAPI*)(DWORD, XState*);
XInputGetState_t g_xget = nullptr;
int g_pad_index = -1;                      // controle ativo (primeiro conectado)
std::chrono::steady_clock::time_point g_last_read = std::chrono::steady_clock::now();
float g_kx = 0.f, g_ky = 0.f;              // analogico do teclado com rampa
bool g_log = false;
}  // namespace

static bool ensure_xinput(bool verbose) {
    static bool tried = false;
    if (g_xget || tried) return g_xget != nullptr;
    tried = true;
    for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        if (HMODULE m = LoadLibraryW(dll)) {
            g_xget = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
            if (g_xget) { if (verbose) std::printf("[input] XInput carregado (%ls)\n", dll); break; }
        }
    }
    if (!g_xget && verbose) std::printf("[input] XInput indisponivel: so teclado\n");
    return g_xget != nullptr;
}

void wdc_input_init() {
    g_log = std::getenv("WDC_INPUTLOG") != nullptr;
    ensure_xinput(true);
}

// Esquemas do controle XInput (so remapeiam botoes no host; nao mudam a emulacao):
//  - Classico: igual ao N64. A = A, B ou X = B, LT ou RT = Z, LB = L, RB = R, Start, direcional, analogico esquerdo = analogico,
//    ANALOGICO DIREITO = botoes C (e Y = C-cima, atalho da camera).
//  - Moderno: nos menus e na pausa, igual ao classico. Em CORRIDA ao vivo (simulacao andando): RT = A (acelera), LT = B (freia),
//    X (Quadrado) = Z (desce marcha), A (X) = R (sobe marcha), Y (Triangulo) = C-cima (troca a visao); LB/RB repetem descer/subir marcha;
//    o resto (analogico, direcional, Start, analogico direito = botoes C) continua igual.
// Funcoes dos botoes do N64 no jogo: A acelera, B freia (medido), R sobe e Z desce marcha com cambio manual, C-cima troca a visao, Start pausa
// (manual do jogo, via busca; ver ENHANCED_MODE.md / TESTES.md).
namespace {
bool g_modern = false;

// ---- tabela de botoes personalizaveis ----
// fonte = bit de wButtons (16 baixos) | LT (bit 16) | RT (bit 17)
constexpr uint32_t S_LB = XB_LB, S_RB = XB_RB, S_A = XB_A, S_B = XB_B, S_X = XB_X, S_Y = XB_Y, S_L3 = 0x0040, S_R3 = 0x0080, S_BACK = 0x0020;
struct SrcInfo { const char* key; uint32_t bit; };
constexpr SrcInfo kSrc[] = {{"A", S_A}, {"B", S_B}, {"X", S_X}, {"Y", S_Y}, {"LB", S_LB}, {"RB", S_RB}, {"LT", WS_LT}, {"RT", WS_RT}, {"L3", S_L3}, {"R3", S_R3}, {"BACK", S_BACK}};
uint32_t g_bind[2][WA_COUNT];
bool g_bind_init = false;

uint32_t pad_sources(const WdcPadRaw& p) {
    return (uint32_t)p.xinput_buttons | (p.lt > 128 ? WS_LT : 0u) | (p.rt > 128 ? WS_RT : 0u);
}
void ensure_bindings() {
    if (g_bind_init) return;
    g_bind_init = true;
    for (int s = 0; s < 2; s++) for (int a = 0; a < WA_COUNT; a++) g_bind[s][a] = wdc_input_default_binding(s, a);
}
}  // namespace

const uint32_t kWdcPickableSources[] = {S_A, S_B, S_X, S_Y, S_LB, S_RB, WS_LT, WS_RT, S_L3, S_R3, S_BACK};
const int kWdcPickableCount = (int)(sizeof(kWdcPickableSources) / sizeof(kWdcPickableSources[0]));

uint32_t wdc_input_default_binding(int scheme, int action) {
    if (scheme == 0) {   // classico: A acelera, B ou X freia, LB freio suave, RB sobe, LT/RT descem, Y troca a visao
        switch (action) {
        case WA_ACCEL: return S_A;
        case WA_BRAKE: return S_B | S_X;
        case WA_SOFTBRAKE: return S_LB;
        case WA_SHIFTUP: return S_RB;
        case WA_SHIFTDOWN: return WS_LT | WS_RT;
        case WA_CAMERA: return S_Y;
        }
    } else {             // moderno (so em corrida): RT acelera, LT freia, A/RB sobem, X/LB descem, Y troca a visao
        switch (action) {
        case WA_ACCEL: return WS_RT;
        case WA_BRAKE: return WS_LT;
        case WA_SOFTBRAKE: return 0;
        case WA_SHIFTUP: return S_A | S_RB;
        case WA_SHIFTDOWN: return S_X | S_LB;
        case WA_CAMERA: return S_Y;
        }
    }
    return 0;
}

uint32_t wdc_input_binding(int scheme, int action) {
    ensure_bindings();
    return (scheme >= 0 && scheme < 2 && action >= 0 && action < WA_COUNT) ? g_bind[scheme][action] : 0;
}
void wdc_input_set_binding(int scheme, int action, uint32_t sources) {
    ensure_bindings();
    if (scheme >= 0 && scheme < 2 && action >= 0 && action < WA_COUNT) g_bind[scheme][action] = sources;
}

const char* wdc_input_source_label(uint32_t src) {
    for (const auto& s : kSrc) if (s.bit == src) return s.key;
    return "?";
}

std::string wdc_input_binding_to_text(uint32_t sources) {
    std::string t;
    for (const auto& s : kSrc)
        if (sources & s.bit) { if (!t.empty()) t += "+"; t += s.key; }
    return t.empty() ? "none" : t;
}

bool wdc_input_binding_from_text(const std::string& text, uint32_t* out) {
    std::string t;
    for (char c : text) if (c != ' ' && c != '\t') t += (char)std::toupper((unsigned char)c);
    if (t.empty() || t == "DEFAULT") return false;
    if (t == "NONE" || t == "-") { *out = 0; return true; }
    uint32_t mask = 0;
    size_t pos = 0;
    while (pos <= t.size()) {
        size_t e = t.find_first_of("+,", pos);
        if (e == std::string::npos) e = t.size();
        const std::string tok = t.substr(pos, e - pos);
        bool found = false;
        for (const auto& s : kSrc) if (tok == s.key) { mask |= s.bit; found = true; break; }
        if (!found) return false;
        pos = e + 1;
    }
    *out = mask;
    return true;
}

void wdc_input_configure(const WdcConfig& cfg) {
    g_modern = cfg.controls == WdcControls::Modern;
    wdc_joy_set_maps(cfg.joymap);
    ensure_bindings();
    for (int s = 0; s < 2; s++)
        for (int a = 0; a < WA_COUNT; a++) {
            uint32_t m = 0;
            if (wdc_input_binding_from_text(cfg.bind[s][a], &m)) {
                g_bind[s][a] = m;
            } else {
                const std::string& t = cfg.bind[s][a];
                if (!t.empty() && t != "default") std::printf("[input] [controls.%s] %s = \"%s\" invalido: usando o padrao\n", s == 0 ? "classic" : "modern", kWdcActionKeys[a], t.c_str());
                g_bind[s][a] = wdc_input_default_binding(s, a);
            }
        }
}

bool wdc_input_probe_pad(uint32_t* pressed) {
    if (!ensure_xinput(false)) return wdc_joy_probe(pressed);
    for (DWORD i = 0; i < 4; i++) {
        XState st{};
        if (g_xget(i, &st) == ERROR_SUCCESS) {
            if (pressed) *pressed = pad_sources({st.buttons, st.lt, st.rt, st.lx, st.ly, st.rx, st.ry});
            return true;
        }
    }
    return wdc_joy_probe(pressed);   // sem XInput: controles DirectInput (PlayStation, Switch Pro, 8BitDo, genericos)
}

// Estado do jogo na RDRAM (endereco KSEG0; as palavras ficam na ordem do host: a leitura direta ja devolve o valor logico).
extern uint8_t* wdc_get_rdram();                  // main.cpp
namespace {
uint32_t ram_word(uint32_t addr) {
    const uint8_t* ram = wdc_get_rdram();
    return ram ? *(const volatile uint32_t*)(ram + (addr & 0x7FFFFF)) : 0;
}
// "Em corrida ao vivo, simulacao andando": modo de replay = 1 (grava; as corridas ao vivo gravam o replay), nao congelado (0x8007D794 != 0 na pausa) e
// o contador de ticks da simulacao (0x8007E530, +1 por tick) mudou nos ultimos 300 ms (telas de resultado e menus nao o incrementam).
bool racing_now() {
    static uint32_t last_ticks = 0;
    static std::chrono::steady_clock::time_point last_change = std::chrono::steady_clock::time_point{};
    if (!g_modern) return false;
    if (ram_word(0x8007DF94) != 1 || ram_word(0x8007D794) != 0) return false;
    const uint32_t t = ram_word(0x8007E530);
    const auto now = std::chrono::steady_clock::now();
    if (t != last_ticks) { last_ticks = t; last_change = now; }
    return now - last_change < std::chrono::milliseconds(300);
}
}  // namespace

void wdc_input_set_modern(bool modern) { g_modern = modern; }

static WdcInput map_pad(const WdcPadRaw& p, bool modern_race) {
    WdcInput r;
    r.pad_connected = true;
    uint16_t b = 0;
    ensure_bindings();
    const uint32_t* t = g_bind[modern_race ? 1 : 0];   // moderno so em corrida ao vivo; menus e pausa usam a tabela classica
    const uint32_t src = pad_sources(p);
    if (src & t[WA_ACCEL]) b |= N_A;          // acelera
    if (src & t[WA_BRAKE]) b |= N_B;          // freio forte e re
    if (src & t[WA_SOFTBRAKE]) b |= N_L;      // freio suave
    if (src & t[WA_SHIFTUP]) b |= N_R;        // marcha acima
    if (src & t[WA_SHIFTDOWN]) b |= N_Z;      // marcha abaixo
    if (src & t[WA_CAMERA]) b |= N_CU;        // troca a visao
    if (p.xinput_buttons & XB_START) b |= N_START;
    if (p.xinput_buttons & XB_DPAD_UP) b |= N_DU;
    if (p.xinput_buttons & XB_DPAD_DOWN) b |= N_DD;
    if (p.xinput_buttons & XB_DPAD_LEFT) b |= N_DL;
    if (p.xinput_buttons & XB_DPAD_RIGHT) b |= N_DR;
    // analogico direito -> botoes C (limiar de 40%)
    constexpr int kC = 13107;
    if (p.ry > kC) b |= N_CU;
    if (p.ry < -kC) b |= N_CD;
    if (p.rx < -kC) b |= N_CL;
    if (p.rx > kC) b |= N_CR;
    r.buttons = b;
    // analogico esquerdo: zona morta radial de 20%, reescalada
    float x = p.lx / 32767.f, y = p.ly / 32767.f;
    const float mag = std::sqrt(x * x + y * y);
    if (mag > 0.2f) {
        const float s = std::min(1.f, (mag - 0.2f) / 0.8f) / mag;
        r.x = x * s;
        r.y = y * s;
    }
    return r;
}

WdcInput wdc_input_map_pad(const WdcPadRaw& p) { return map_pad(p, false); }                    // classico (teste)
WdcInput wdc_input_map_pad_modern(const WdcPadRaw& p, bool racing) { return map_pad(p, racing); }   // moderno (teste): racing = em corrida ao vivo

WdcInput wdc_input_read() {
    WdcInput out;
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(0.1f, std::chrono::duration<float>(now - g_last_read).count());
    g_last_read = now;

    // teclado
    uint16_t b = 0;
    auto key = [](int vk) { return wdc_window_key(vk); };
    if (key('X')) b |= N_A;
    if (key('C')) b |= N_B;
    if (key('Z')) b |= N_Z;
    if (key(VK_RETURN)) b |= N_START;
    if (key('Q')) b |= N_L;
    if (key('E')) b |= N_R;
    if (key('W')) b |= N_DU;
    if (key('S')) b |= N_DD;
    if (key('A')) b |= N_DL;
    if (key('D')) b |= N_DR;
    if (key('I')) b |= N_CU;
    if (key('K')) b |= N_CD;
    if (key('J')) b |= N_CL;
    if (key('L')) b |= N_CR;
    // analogico por setas, com rampa (2,5 unidades/s ao subir = ~0,4 s ate o fundo de escala, 8 ao voltar a zero) para nao dar volante cravado de uma vez
    const float tx = (key(VK_RIGHT) ? 1.f : 0.f) - (key(VK_LEFT) ? 1.f : 0.f);
    const float ty = (key(VK_UP) ? 1.f : 0.f) - (key(VK_DOWN) ? 1.f : 0.f);
    auto ramp = [dt](float cur, float target) {
        const float rate = (target == 0.f || (target * cur < 0.f)) ? 8.f : 2.5f;
        const float d = target - cur;
        return std::fabs(d) <= rate * dt ? target : cur + std::copysign(rate * dt, d);
    };
    g_kx = ramp(g_kx, tx);
    g_ky = ramp(g_ky, ty);
    out.buttons = b;
    out.x = g_kx;
    out.y = g_ky;

    // controle
    if (g_xget) {
        XState st{};
        bool ok = false;
        if (g_pad_index >= 0) ok = g_xget((DWORD)g_pad_index, &st) == ERROR_SUCCESS;
        if (!ok) {
            g_pad_index = -1;
            for (DWORD i = 0; i < 4 && !ok; i++) {
                if (g_xget(i, &st) == ERROR_SUCCESS) { g_pad_index = (int)i; ok = true; if (g_log) std::printf("[input] controle XInput %lu conectado\n", i); }
            }
        }
        if (ok) {
            const WdcInput p = map_pad({st.buttons, st.lt, st.rt, st.lx, st.ly, st.rx, st.ry}, racing_now());
            out.pad_connected = true;
            out.buttons |= p.buttons;
            if (p.x != 0.f || p.y != 0.f) { out.x = p.x; out.y = p.y; }
        }
    }
    if (!out.pad_connected) {   // sem XInput: primeiro controle DirectInput (DualSense, DualShock 4, Switch Pro, 8BitDo, genericos)
        WdcPadRaw r{};
        if (wdc_joy_read(0, &r)) {
            const WdcInput p = map_pad(r, racing_now());
            out.pad_connected = true;
            out.buttons |= p.buttons;
            if (p.x != 0.f || p.y != 0.f) { out.x = p.x; out.y = p.y; }
        }
    }
    return out;
}

WdcInput wdc_input_read_pad(unsigned xinput_index) {
    WdcInput out;
    // Jogador 2 (indice 1) tambem tem teclado: teclado numerico. 8/2/4/6 = direcional + analogico (com rampa), 0 = A, ponto = B, 7 = Start,
    // 9 = Z, 1/3 = L/R. Os demais jogadores so usam o controle XInput de mesmo indice.
    if (xinput_index == 1) {
        static std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        static float kx = 0.f, ky = 0.f;
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
        last = now;
        auto key = [](int vk) { return wdc_window_key(vk); };
        uint16_t b = 0;
        if (key(VK_NUMPAD0)) b |= N_A;
        if (key(VK_DECIMAL)) b |= N_B;
        if (key(VK_NUMPAD9)) b |= N_Z;
        if (key(VK_NUMPAD7)) b |= N_START;
        if (key(VK_NUMPAD1)) b |= N_L;
        if (key(VK_NUMPAD3)) b |= N_R;
        if (key(VK_NUMPAD8)) b |= N_DU;
        if (key(VK_NUMPAD2)) b |= N_DD;
        if (key(VK_NUMPAD4)) b |= N_DL;
        if (key(VK_NUMPAD6)) b |= N_DR;
        const float tx = (key(VK_NUMPAD6) ? 1.f : 0.f) - (key(VK_NUMPAD4) ? 1.f : 0.f);
        const float ty = (key(VK_NUMPAD8) ? 1.f : 0.f) - (key(VK_NUMPAD2) ? 1.f : 0.f);
        auto ramp = [dt](float cur, float target) {
            const float rate = (target == 0.f || (target * cur < 0.f)) ? 8.f : 2.5f;
            const float d = target - cur;
            return std::fabs(d) <= rate * dt ? target : cur + std::copysign(rate * dt, d);
        };
        kx = ramp(kx, tx);
        ky = ramp(ky, ty);
        out.buttons = b;
        out.x = kx;
        out.y = ky;
    }
    XState st{};
    if (!g_xget || xinput_index > 3 || g_xget((DWORD)xinput_index, &st) != ERROR_SUCCESS) {   // sem XInput para este jogador: controle DirectInput de mesmo indice
        WdcPadRaw r{};
        if (wdc_joy_read(xinput_index, &r)) {
            const WdcInput p = map_pad(r, racing_now());
            out.pad_connected = true;
            out.buttons |= p.buttons;
            if (p.x != 0.f || p.y != 0.f) { out.x = p.x; out.y = p.y; }
        }
        return out;
    }
    const WdcInput p = map_pad({st.buttons, st.lt, st.rt, st.lx, st.ly, st.rx, st.ry}, racing_now());
    out.pad_connected = true;
    out.buttons |= p.buttons;
    if (p.x != 0.f || p.y != 0.f) { out.x = p.x; out.y = p.y; }
    return out;
}
