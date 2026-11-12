// Teste do mapeamento XInput -> N64 (wdc_input_map_pad) e da rampa/botoes do teclado (wdc_input_read com teclas simuladas).
// Build: clang-cl /std:c++20 /EHsc -I recomp/project/src recomp/project/test/test_input_map.cpp recomp/project/src/wdc_input.cpp user32.lib
#include <cmath>
#include <cstdio>
#include <set>
#include <windows.h>
#include "wdc_input.h"

static std::set<int> g_down;
bool wdc_window_key(int vk) { return g_down.count(vk) != 0; }   // substitui a janela
uint8_t* wdc_get_rdram() { return nullptr; }                     // substitui main.cpp (o esquema moderno consulta a RDRAM)

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FALHA linha %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    // gamepad
    WdcPadRaw p{};
    p.xinput_buttons = 0x1000;                      // A
    CHECK(wdc_input_map_pad(p).buttons == 0x8000);
    p.xinput_buttons = 0x4000;                      // X -> B
    CHECK(wdc_input_map_pad(p).buttons == 0x4000);
    p.xinput_buttons = 0x2000;                      // B -> B
    CHECK(wdc_input_map_pad(p).buttons == 0x4000);
    p.xinput_buttons = 0x0010 | 0x0100 | 0x0200;    // Start, LB, RB
    CHECK(wdc_input_map_pad(p).buttons == (0x1000 | 0x0020 | 0x0010));
    p.xinput_buttons = 0x0001 | 0x0002 | 0x0004 | 0x0008;   // dpad U D L R
    CHECK(wdc_input_map_pad(p).buttons == (0x0800 | 0x0400 | 0x0200 | 0x0100));
    p = {}; p.lt = 200;                             // gatilho esquerdo -> Z
    CHECK(wdc_input_map_pad(p).buttons == 0x2000);
    p = {}; p.rt = 100;                             // abaixo do limiar
    CHECK(wdc_input_map_pad(p).buttons == 0);
    p = {}; p.ry = 30000; p.rx = -30000;            // C-up + C-left
    CHECK(wdc_input_map_pad(p).buttons == (0x0008 | 0x0002));
    p = {}; p.ry = -30000; p.rx = 30000;            // C-down + C-right
    CHECK(wdc_input_map_pad(p).buttons == (0x0004 | 0x0001));
    // analogico: zona morta e escala
    p = {}; p.lx = 4000;                            // ~12%: dentro da zona morta
    CHECK(wdc_input_map_pad(p).x == 0.f);
    p = {}; p.lx = 32767;
    CHECK(std::fabs(wdc_input_map_pad(p).x - 1.f) < 1e-3);
    p = {}; p.ly = -32767;
    CHECK(std::fabs(wdc_input_map_pad(p).y + 1.f) < 1e-3);
    p = {}; p.lx = 20000; p.ly = 20000;             // diagonal forte: modulo <= 1
    { auto r = wdc_input_map_pad(p); CHECK(std::sqrt(r.x * r.x + r.y * r.y) <= 1.0001f); CHECK(r.x > 0 && r.y > 0); }

    // teclado
    wdc_input_init();
    g_down = {'X', VK_RETURN, 'W', 'I', 'Q'};
    auto k = wdc_input_read();
    CHECK(k.buttons == (0x8000 | 0x1000 | 0x0800 | 0x0008 | 0x0020));
    g_down = {'C', 'Z', 'S', 'K', 'E', 'A', 'J', 'D', 'L'};
    k = wdc_input_read();
    CHECK(k.buttons == (0x4000 | 0x2000 | 0x0400 | 0x0004 | 0x0010 | 0x0200 | 0x0002 | 0x0100 | 0x0001));
    // rampa do volante: seta direita segurada sobe gradualmente e satura em 1
    g_down = {VK_RIGHT};
    float prev = 0.f; bool monotone = true;
    for (int i = 0; i < 40; i++) { Sleep(16); k = wdc_input_read(); if (k.x < prev - 1e-6f) monotone = false; prev = k.x; }
    CHECK(monotone && k.x > 0.9f && k.x <= 1.f);
    g_down.clear();
    for (int i = 0; i < 40; i++) { Sleep(16); k = wdc_input_read(); }
    CHECK(std::fabs(k.x) < 1e-3f);
    // ---- botoes personalizados (wdc_input_binding_*) ----
    {
        uint32_t m = 0;
        CHECK(wdc_input_binding_from_text("RT", &m) && m == WS_RT);
        CHECK(wdc_input_binding_from_text("b+x", &m) && m == (0x2000u | 0x4000u));
        CHECK(wdc_input_binding_from_text(" LT + RT ", &m) && m == (WS_LT | WS_RT));
        CHECK(wdc_input_binding_from_text("none", &m) && m == 0);
        CHECK(!wdc_input_binding_from_text("ZZ", &m));          // invalido
        CHECK(!wdc_input_binding_from_text("default", &m));     // "default" e vazio = usar o padrao
        CHECK(!wdc_input_binding_from_text("", &m));
        CHECK(wdc_input_binding_to_text(0) == "none");
        CHECK(wdc_input_binding_to_text(WS_LT | WS_RT) == "LT+RT");
        CHECK(wdc_input_binding_to_text(0x2000u | 0x4000u) == "B+X");
        // padroes
        CHECK(wdc_input_default_binding(0, WA_ACCEL) == 0x1000u && wdc_input_default_binding(1, WA_ACCEL) == WS_RT);
        CHECK(wdc_input_default_binding(1, WA_SOFTBRAKE) == 0);
        // personalizado: no classico, o gatilho direito acelera e A deixa de acelerar
        wdc_input_set_binding(0, WA_ACCEL, WS_RT);
        WdcPadRaw q{}; q.rt = 200;
        CHECK(wdc_input_map_pad(q).buttons == (0x8000 | 0x2000));   // o mapa nao tira conflito: RT continua tambem como marcha abaixo (o setup e que remove a duplicata)
        q = {}; q.xinput_buttons = 0x1000;
        CHECK(wdc_input_map_pad(q).buttons == 0);
        // volta ao padrao via configure() com texto da configuracao (vazio/"default" = padrao; valido = usa)
        WdcConfig cfg;
        cfg.controls = WdcControls::Modern;
        cfg.bind[0][WA_ACCEL] = "default";
        cfg.bind[1][WA_SHIFTUP] = "Y";            // moderno: sobe marcha no Y (que tambem e a camera: ambos disparam)
        cfg.bind[1][WA_BRAKE] = "lixo";           // invalido: padrao (LT)
        wdc_input_configure(cfg);
        q = {}; q.xinput_buttons = 0x1000;
        CHECK(wdc_input_map_pad(q).buttons == 0x8000);                 // classico voltou ao padrao
        q = {}; q.xinput_buttons = 0x8000;                              // Y
        CHECK(wdc_input_map_pad_modern(q, true).buttons == (0x0008 | 0x0010));   // C-cima + R
        q = {}; q.lt = 255;
        CHECK(wdc_input_map_pad_modern(q, true).buttons == 0x4000);    // LT continua freando
        q = {}; q.xinput_buttons = 0x1000;
        CHECK(wdc_input_map_pad_modern(q, false).buttons == 0x8000);   // fora de corrida: tabela classica
        CHECK(wdc_input_map_pad_modern(q, true).buttons == 0);         // em corrida: A nao faz mais nada no moderno (sobe marcha passou para Y)
    }
    std::printf(fails ? "%d FALHAS\n" : "todos os testes de entrada passaram\n", fails);
    return fails ? 1 : 0;
}
