#include "wdc_joy.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "wdc_input.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

namespace {
// bits de XINPUT_GAMEPAD.wButtons
constexpr uint16_t XB_UP = 0x0001, XB_DOWN = 0x0002, XB_LEFT = 0x0004, XB_RIGHT = 0x0008, XB_START = 0x0010, XB_BACK = 0x0020, XB_L3 = 0x0040, XB_R3 = 0x0080,
                   XB_LB = 0x0100, XB_RB = 0x0200, XB_A = 0x1000, XB_B = 0x2000, XB_X = 0x4000, XB_Y = 0x8000;

// ---- mapa: botao logico (estilo Xbox) -> fonte(s) fisica(s) do controle ----
// Ordem dos botoes logicos; LT e RT podem ter botao e/ou eixo.
constexpr int kLog = 12;
const char* const kName[kLog] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "BACK", "START", "L3", "R3"};
constexpr uint16_t kBit[kLog] = {XB_A, XB_B, XB_X, XB_Y, XB_LB, XB_RB, 0, 0, XB_BACK, XB_START, XB_L3, XB_R3};

struct Src {
    int btn = -1;            // indice do botao (0..31) ou -1
    int axis = -1;           // eixo 0..7 (X, Y, Z, Rz, Rx, Ry, slider0, slider1) ou -1 (so LT/RT)
    char kind = 'm';         // eixo: 'm' repouso no minimo, 'M' repouso no maximo, 'p' positivo a partir do centro, 'n' negativo a partir do centro
};
struct Map { Src s[kLog]; };

// "A:0,B:1,LT:6+ax4m": botoes por indice; LT/RT aceitam botao e/ou eixo (axN + m/M/p/n)
Map parse_map(const std::string& text) {
    Map m;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find(',', pos);
        if (e == std::string::npos) e = text.size();
        std::string tok;
        for (size_t k = pos; k < e; k++) if (text[k] != ' ' && text[k] != '\t') tok += text[k];   // tolera espacos escritos a mao no arquivo
        pos = e + 1;
        const size_t c = tok.find(':');
        if (c == std::string::npos) continue;
        std::string nm = tok.substr(0, c);
        for (auto& ch : nm) ch = (char)std::toupper((unsigned char)ch);
        int li = -1;
        for (int i = 0; i < kLog; i++) if (nm == kName[i]) li = i;
        if (li < 0) continue;
        std::string v = tok.substr(c + 1);
        size_t q = 0;
        while (q <= v.size()) {
            size_t p2 = v.find('+', q);
            if (p2 == std::string::npos) p2 = v.size();
            const std::string part = v.substr(q, p2 - q);
            q = p2 + 1;
            if (part.empty()) continue;
            if (part.rfind("ax", 0) == 0 && part.size() >= 3) {
                m.s[li].axis = std::atoi(part.c_str() + 2);
                m.s[li].kind = part.size() > 3 ? part[3] : 'm';
                if (m.s[li].axis < 0 || m.s[li].axis > 7) m.s[li].axis = -1;
            } else {
                const int b = std::atoi(part.c_str());
                m.s[li].btn = (b >= 0 && b < 32) ? b : -1;
            }
        }
    }
    return m;
}

std::string format_map(const Map& m) {
    std::string t;
    for (int i = 0; i < kLog; i++) {
        std::string v;
        if (m.s[i].btn >= 0) v += std::to_string(m.s[i].btn);
        if (m.s[i].axis >= 0) { if (!v.empty()) v += "+"; v += "ax" + std::to_string(m.s[i].axis) + std::string(1, m.s[i].kind); }
        if (v.empty()) continue;
        if (!t.empty()) t += ",";
        t += std::string(kName[i]) + ":" + v;
    }
    return t;
}

enum class Profile { Generic, Sony, Nintendo, Android };
const char* profile_name(Profile p) { return p == Profile::Sony ? "PlayStation" : p == Profile::Nintendo ? "Nintendo" : p == Profile::Android ? "8BitDo/Android" : "generico"; }
const char* default_text(Profile p) {
    switch (p) {
    case Profile::Sony: return "A:1,B:2,X:0,Y:3,LB:4,RB:5,LT:6+ax4m,RT:7+ax5m,BACK:8,START:9,L3:10,R3:11";     // quadrado 0, cruz 1, bola 2, triangulo 3; gatilhos analogicos em Rx/Ry
    case Profile::Nintendo: return "A:0,B:1,X:2,Y:3,LB:4,RB:5,LT:6,RT:7,BACK:8,START:9,L3:10,R3:11";          // Switch Pro: B (baixo) = A do Xbox, A (direita) = B do Xbox
    case Profile::Android: return "A:0,B:1,X:3,Y:4,LB:6,RB:7,LT:8,RT:9,BACK:10,START:11,L3:13,R3:14";        // 8BitDo e controles HID "estilo Android"
    default: return "A:0,B:1,X:2,Y:3,LB:4,RB:5,BACK:6,START:7,L3:8,R3:9";                                     // generico (estilo Logitech/Xbox em modo DirectInput)
    }
}

struct Joy {
    IDirectInputDevice8W* dev = nullptr;
    GUID instance{};
    Profile prof = Profile::Generic;
    uint16_t vid = 0, pid = 0;
    std::wstring name;
    Map map;
    int fails = 0;
};

IDirectInput8W* g_di = nullptr;
std::vector<Joy> g_joys;
std::map<std::string, std::string> g_maps;   // mapas aprendidos (config)
std::chrono::steady_clock::time_point g_next_scan{};

std::string key_of(uint16_t vid, uint16_t pid) {
    char b[32];
    std::snprintf(b, sizeof b, "dev_%04x_%04x", vid, pid);
    return b;
}
void load_map(Joy& j) {
    auto it = g_maps.find(key_of(j.vid, j.pid));
    j.map = parse_map(it != g_maps.end() ? it->second : std::string(default_text(j.prof)));
}

// Dispositivos XInput aparecem tambem no DirectInput; o caminho HID deles tem "IG_". Esses ficam com o XInput (senao o botao contaria em dobro).
bool is_xinput_device(const GUID& product) {
    UINT n = 0;
    if (GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || n == 0) return false;
    std::vector<RAWINPUTDEVICELIST> list(n);
    if (GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) return false;
    for (const auto& d : list) {
        if (d.dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT sz = sizeof(info);
        if (GetRawInputDeviceInfoA(d.hDevice, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1) continue;
        if ((DWORD)MAKELONG(info.hid.dwVendorId, info.hid.dwProductId) != product.Data1) continue;
        char nm[512];
        UINT nsz = sizeof(nm);
        if (GetRawInputDeviceInfoA(d.hDevice, RIDI_DEVICENAME, nm, &nsz) == (UINT)-1) continue;
        if (std::strstr(nm, "IG_")) return true;
    }
    return false;
}

BOOL CALLBACK axis_cb(const DIDEVICEOBJECTINSTANCEW* o, void* ctx) {
    auto* dev = (IDirectInputDevice8W*)ctx;
    if (o->dwType & DIDFT_AXIS) {
        DIPROPRANGE r{};
        r.diph.dwSize = sizeof(r);
        r.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        r.diph.dwHow = DIPH_BYID;
        r.diph.dwObj = o->dwType;
        r.lMin = -32768;
        r.lMax = 32767;
        dev->SetProperty(DIPROP_RANGE, &r.diph);
        DIPROPDWORD dz{};
        dz.diph.dwSize = sizeof(dz);
        dz.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        dz.diph.dwHow = DIPH_BYID;
        dz.diph.dwObj = o->dwType;
        dz.dwData = 0;   // a zona morta e aplicada depois, igual para todos os controles (wdc_input.cpp)
        dev->SetProperty(DIPROP_DEADZONE, &dz.diph);
    }
    return DIENUM_CONTINUE;
}

BOOL CALLBACK enum_cb(const DIDEVICEINSTANCEW* inst, void* ctx) {
    auto* out = (std::vector<DIDEVICEINSTANCEW>*)ctx;
    if (!is_xinput_device(inst->guidProduct)) out->push_back(*inst);
    return DIENUM_CONTINUE;
}

void scan() {
    if (!g_di && FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&g_di, nullptr))) { g_di = nullptr; return; }
    std::vector<DIDEVICEINSTANCEW> found;
    g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, enum_cb, &found, DIEDFL_ATTACHEDONLY);
    for (const auto& f : found) {   // novos dispositivos
        bool have = false;
        for (const auto& j : g_joys) if (IsEqualGUID(j.instance, f.guidInstance)) { have = true; break; }
        if (have) continue;
        Joy j;
        j.instance = f.guidInstance;
        j.vid = LOWORD(f.guidProduct.Data1);
        j.pid = HIWORD(f.guidProduct.Data1);
        j.name = f.tszProductName;
        j.prof = j.vid == 0x054C ? Profile::Sony : j.vid == 0x057E ? Profile::Nintendo : j.vid == 0x2DC8 ? Profile::Android : Profile::Generic;
        load_map(j);
        if (FAILED(g_di->CreateDevice(f.guidInstance, &j.dev, nullptr)) || !j.dev) continue;
        if (FAILED(j.dev->SetDataFormat(&c_dfDIJoystick2)) || FAILED(j.dev->SetCooperativeLevel(GetDesktopWindow(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) { j.dev->Release(); continue; }
        j.dev->EnumObjects(axis_cb, j.dev, DIDFT_AXIS);
        j.dev->Acquire();
        std::printf("[input] controle DirectInput: %ls (VID %04X PID %04X, perfil %s%s)\n", j.name.c_str(), j.vid, j.pid, profile_name(j.prof),
                    g_maps.count(key_of(j.vid, j.pid)) ? ", mapa aprendido" : "");
        g_joys.push_back(std::move(j));
    }
    for (size_t i = 0; i < g_joys.size();) {   // desconectados
        bool still = false;
        for (const auto& f : found) if (IsEqualGUID(g_joys[i].instance, f.guidInstance)) { still = true; break; }
        if (!still) {
            std::printf("[input] controle DirectInput desconectado: %ls\n", g_joys[i].name.c_str());
            if (g_joys[i].dev) { g_joys[i].dev->Unacquire(); g_joys[i].dev->Release(); }
            g_joys.erase(g_joys.begin() + (long)i);
        } else {
            i++;
        }
    }
}

void maybe_scan() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_next_scan) return;
    g_next_scan = now + std::chrono::seconds(g_joys.empty() ? 2 : 5);
    scan();
}

bool poll(Joy& j, DIJOYSTATE2& s) {
    HRESULT hr = j.dev->Poll();
    if (FAILED(hr)) {
        hr = j.dev->Acquire();
        while (hr == DIERR_INPUTLOST) hr = j.dev->Acquire();
        if (FAILED(hr)) { j.fails++; return false; }
        j.dev->Poll();
    }
    hr = j.dev->GetDeviceState(sizeof(s), &s);
    if (FAILED(hr)) { j.fails++; return false; }
    j.fails = 0;
    return true;
}

int16_t clamp16(long v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

long axis_val(const DIJOYSTATE2& s, int i) {
    switch (i) {
    case 0: return s.lX;
    case 1: return s.lY;
    case 2: return s.lZ;
    case 3: return s.lRz;
    case 4: return s.lRx;
    case 5: return s.lRy;
    case 6: return s.rglSlider[0];
    default: return s.rglSlider[1];
    }
}
uint8_t axis_trigger(long v, char kind) {   // eixo de gatilho -> 0..255
    long t = 0;
    switch (kind) {
    case 'M': t = 32767 - v; break;
    case 'p': t = v * 2; break;
    case 'n': t = -v * 2; break;
    default: t = v + 32768; break;
    }
    t >>= 8;
    return (uint8_t)(t < 0 ? 0 : t > 255 ? 255 : t);
}

// DIJOYSTATE2 -> formato XInput. Sticks: X/Y esquerdo, Z/Rz direito (padrao dos controles HID); Y para cima positivo (no DirectInput e para baixo).
WdcPadRaw convert(const Joy& j, const DIJOYSTATE2& s) {
    WdcPadRaw p{};
    p.lx = clamp16(s.lX);
    p.ly = clamp16(-(long)s.lY);
    p.rx = clamp16(s.lZ);
    p.ry = clamp16(-(long)s.lRz);
    uint16_t b = 0;
    for (int i = 0; i < kLog; i++) {
        const Src& sc = j.map.s[i];
        bool on = sc.btn >= 0 && (s.rgbButtons[sc.btn] & 0x80);
        if (i == 6 || i == 7) {
            uint8_t v = on ? 255 : 0;
            if (sc.axis >= 0) v = std::max<uint8_t>(v, axis_trigger(axis_val(s, sc.axis), sc.kind));
            (i == 6 ? p.lt : p.rt) = v;
        } else if (on) {
            b |= kBit[i];
        }
    }
    const DWORD pov = s.rgdwPOV[0];   // direcional (hat): centesimos de grau, 0 = cima; neutro = 0xFFFF
    if (LOWORD(pov) != 0xFFFF) {
        const int a = (int)(pov / 100);
        if (a >= 315 || a <= 45) b |= XB_UP;
        if (a >= 45 && a <= 135) b |= XB_RIGHT;
        if (a >= 135 && a <= 225) b |= XB_DOWN;
        if (a >= 225 && a <= 315) b |= XB_LEFT;
    }
    p.xinput_buttons = b;
    return p;
}
}  // namespace

bool wdc_joy_read(unsigned player, WdcPadRaw* out) {
    maybe_scan();
    if (player >= g_joys.size()) return false;
    Joy& j = g_joys[player];
    DIJOYSTATE2 s{};
    if (!poll(j, s)) {
        if (j.fails > 120) g_next_scan = std::chrono::steady_clock::time_point{};   // some ha 2 s: reprocura (o scan remove os desconectados)
        return false;
    }
    if (out) *out = convert(j, s);
    return true;
}

bool wdc_joy_probe(uint32_t* pressed) {
    WdcPadRaw p{};
    if (!wdc_joy_read(0, &p)) return false;
    if (pressed) *pressed = (uint32_t)p.xinput_buttons | (p.lt > 128 ? WS_LT : 0u) | (p.rt > 128 ? WS_RT : 0u);
    return true;
}

void wdc_joy_set_maps(const std::map<std::string, std::string>& maps) {
    g_maps = maps;
    for (auto& j : g_joys) load_map(j);
}

bool wdc_joy_snapshot(WdcJoySnap* out) {
    maybe_scan();
    if (g_joys.empty()) return false;
    Joy& j = g_joys[0];
    DIJOYSTATE2 s{};
    if (!poll(j, s)) return false;
    if (out) {
        *out = WdcJoySnap{};
        out->vid = j.vid;
        out->pid = j.pid;
        WideCharToMultiByte(CP_UTF8, 0, j.name.c_str(), -1, out->name, (int)sizeof(out->name) - 1, nullptr, nullptr);
        for (int i = 0; i < 32; i++) out->buttons[i] = (s.rgbButtons[i] & 0x80) != 0;
        for (int i = 0; i < 8; i++) out->axes[i] = axis_val(s, i);
        out->pov = s.rgdwPOV[0];
    }
    return true;
}

std::string wdc_joy_map_key(uint16_t vid, uint16_t pid) { return key_of(vid, pid); }

std::string wdc_joy_map_text(uint16_t vid, uint16_t pid) {
    auto it = g_maps.find(key_of(vid, pid));
    if (it != g_maps.end()) return it->second;
    const Profile p = vid == 0x054C ? Profile::Sony : vid == 0x057E ? Profile::Nintendo : vid == 0x2DC8 ? Profile::Android : Profile::Generic;
    return default_text(p);
}

void wdc_joy_apply_map(uint16_t vid, uint16_t pid, const std::string& text) {
    g_maps[key_of(vid, pid)] = format_map(parse_map(text));   // normaliza
    for (auto& j : g_joys) if (j.vid == vid && j.pid == pid) load_map(j);
}

void wdc_joy_test(int seconds) {
    std::printf("[joytest] procurando controles DirectInput (nao-XInput)... aperte botoes e mexa nos analogicos por %d s\n", seconds);
    g_next_scan = std::chrono::steady_clock::time_point{};
    maybe_scan();
    if (g_joys.empty()) std::printf("[joytest] nenhum controle DirectInput encontrado (controles Xbox/XInput nao aparecem aqui)\n");
    DIJOYSTATE2 last[8]{};
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds)) {
        maybe_scan();
        for (size_t k = 0; k < g_joys.size() && k < 8; k++) {
            DIJOYSTATE2 s{};
            if (!poll(g_joys[k], s)) continue;
            bool changed = false;
            for (int i = 0; i < 32; i++) if ((s.rgbButtons[i] & 0x80) != (last[k].rgbButtons[i] & 0x80)) changed = true;
            auto big = [](long a, long b) { return std::labs(a - b) > 4000; };
            if (big(s.lX, last[k].lX) || big(s.lY, last[k].lY) || big(s.lZ, last[k].lZ) || big(s.lRz, last[k].lRz) || big(s.lRx, last[k].lRx) || big(s.lRy, last[k].lRy) ||
                s.rgdwPOV[0] != last[k].rgdwPOV[0]) changed = true;
            if (!changed) continue;
            last[k] = s;
            std::printf("[joytest] #%zu VID %04X PID %04X  X=%6ld Y=%6ld Z=%6ld Rz=%6ld Rx=%6ld Ry=%6ld POV=%5lu botoes:", k, g_joys[k].vid, g_joys[k].pid, s.lX, s.lY, s.lZ, s.lRz, s.lRx, s.lRy,
                        (unsigned long)s.rgdwPOV[0]);
            for (int i = 0; i < 32; i++) if (s.rgbButtons[i] & 0x80) std::printf(" %d", i);
            const WdcPadRaw p = convert(g_joys[k], s);
            std::printf("  => xinput=0x%04X lt=%u rt=%u\n", p.xinput_buttons, p.lt, p.rt);
        }
        Sleep(8);
    }
    std::printf("[joytest] fim\n");
}
