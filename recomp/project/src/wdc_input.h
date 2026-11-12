// Entrada real do modo interativo (--play): teclado (via janela) e controle XInput, combinados num unico controle N64 (porta 0).
//
// Jogador 2 (--players 2): teclado numerico (8/2/4/6 = direcional + analogico, 0 = A, . = B, 7 = Start, 9 = Z, 1/3 = L/R) e o 2o controle XInput.
//
// Teclado:  setas = analogico | W A S D = direcional | X = A | C = B | Z = Z | Enter = Start | Q = L | E = R | I J K L = botoes C
//           Esc = sair | F11 = tela cheia
// XInput (esquema CLASSICO): analogico esquerdo = analogico | direcional | A = A | X ou B = B | LT ou RT = Z | LB = L | RB = R | Start = Start |
//           analogico direito = botoes C | Y = C-cima
// XInput (esquema MODERNO, so em corrida ao vivo; nos menus e na pausa = classico): RT = acelera (A) | LT = freia (B) | X (Quadrado) = desce marcha (Z) |
//           A (X) = sobe marcha (R) | Y (Triangulo) = troca a visao (C-cima) | LB/RB = descer/subir marcha tambem
#pragma once
#include <cstdint>
#include <string>

#include "wdc_config.h"

struct WdcInput {
    uint16_t buttons = 0;     // bits do OSContPad (0x8000 A, 0x4000 B, 0x2000 Z, 0x1000 Start, 0x0800.. direcional, 0x0020 L, 0x0010 R, 0x0008.. C)
    float x = 0.f, y = 0.f;   // analogico em [-1, 1] (y positivo = para cima)
    bool pad_connected = false;
};

void wdc_input_init();
void wdc_input_set_modern(bool modern);   // escolhe o esquema do XInput (wdc_config: [controls] scheme)
void wdc_input_configure(const WdcConfig& cfg);   // esquema + botoes personalizados ([controls.classic]/[controls.modern]); chamar depois de wdc_cfg_load

// ---- botoes personalizaveis do XInput (esquemas: 0 = classico, 1 = moderno) ----
// Uma "fonte" e um bit: os 16 bits baixos sao os de XINPUT_GAMEPAD.wButtons; LT e RT (gatilhos, > 50%) sao os bits 16 e 17.
// Uma acao pode ter varias fontes (qualquer uma aciona) ou nenhuma. Direcional, Start, analogicos e analogico direito (botoes C) nao sao personalizaveis.
enum WdcAction { WA_ACCEL = 0, WA_BRAKE, WA_SOFTBRAKE, WA_SHIFTUP, WA_SHIFTDOWN, WA_CAMERA, WA_COUNT };   // = N64 A, B, L, R, Z, C-cima (mesma ordem de kWdcActionKeys)
constexpr uint32_t WS_LT = 1u << 16, WS_RT = 1u << 17;
uint32_t wdc_input_default_binding(int scheme, int action);   // padrao do esquema (0 classico, 1 moderno)
uint32_t wdc_input_binding(int scheme, int action);           // valor em uso
void wdc_input_set_binding(int scheme, int action, uint32_t sources);   // altera em memoria (o setup grava no arquivo)
std::string wdc_input_binding_to_text(uint32_t sources);      // "B+X", "RT", "none"
bool wdc_input_binding_from_text(const std::string& text, uint32_t* out);   // false = texto invalido ("default"/vazio devolvem false: use o padrao)
const char* wdc_input_source_label(uint32_t single_source);   // "A", "LT"... (uma fonte)
extern const uint32_t kWdcPickableSources[];                   // fontes que o setup oferece (A B X Y LB RB LT RT L3 R3 BACK)
extern const int kWdcPickableCount;
// Primeiro controle XInput conectado: devolve true e as fontes pressionadas agora; false se nao ha controle. Usado pela tela de personalizacao.
bool wdc_input_probe_pad(uint32_t* pressed_sources);
WdcInput wdc_input_read();    // amostra teclado + controle agora (a rampa do teclado usa o tempo desde a ultima leitura)
WdcInput wdc_input_read_pad(unsigned xinput_index);   // jogadores 2..4 (--players): so o controle XInput do indice dado (0..3); sem controle: tudo zero

// Mapeamento do XInput (exposto para teste): estado cru -> botoes N64 + analogico.
struct WdcPadRaw { uint16_t xinput_buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; };
WdcInput wdc_input_map_pad(const WdcPadRaw& p);                      // esquema classico
WdcInput wdc_input_map_pad_modern(const WdcPadRaw& p, bool racing);   // esquema moderno; racing = em corrida ao vivo
