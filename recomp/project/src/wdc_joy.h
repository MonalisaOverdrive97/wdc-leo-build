// Controles que NAO sao XInput (PlayStation DualShock 4 / DualSense, Switch Pro, 8BitDo, genericos HID...) via DirectInput 8, convertidos para o mesmo formato do XInput
// (WdcPadRaw) para que o mapeamento, os esquemas e a personalizacao de botoes valham igual. Controles XInput (Xbox e compativeis) continuam pelo XInput e sao ignorados aqui.
// Perfis por VID/PID (Sony, Nintendo, 8BitDo/Android, generico) e, para qualquer outro modelo, um MAPA APRENDIDO no setup ("Mapear controle"), gravado em
// wdc_config.toml: [gamepad] dev_<vid>_<pid> = "A:0,B:1,X:3,Y:4,LB:6,RB:7,LT:8,RT:9,BACK:10,START:11,L3:13,R3:14" (LT/RT podem ser eixos: "LT:ax4m").
// Hot-plug: o jogo reprocura controles a cada poucos segundos.
#pragma once
#include <cstdint>
#include <map>
#include <string>

struct WdcPadRaw;

bool wdc_joy_read(unsigned player, WdcPadRaw* out);   // controle DirectInput do jogador (0-based, entre os nao-XInput); false se nao ha
bool wdc_joy_probe(uint32_t* pressed_sources);        // primeiro controle DirectInput: fontes pressionadas agora (a tela de personalizar usa); false se nao ha
void wdc_joy_test(int seconds);                       // diagnostico (--joytest): lista os controles e imprime os botoes/eixos crus quando mudam

// Mapas aprendidos (config): chave "dev_<vid hex minusculo 4>_<pid hex 4>" -> texto do mapa. Chamado por wdc_input_configure.
void wdc_joy_set_maps(const std::map<std::string, std::string>& maps);

// Assistente "Mapear controle" (setup): estado cru do primeiro controle DirectInput.
struct WdcJoySnap {
    uint16_t vid = 0, pid = 0;
    char name[128] = {};
    bool buttons[32] = {};
    long axes[8] = {};      // X, Y, Z, Rz, Rx, Ry, slider0, slider1 (intervalo -32768..32767)
    uint32_t pov = 0xFFFFFFFFu;
};
bool wdc_joy_snapshot(WdcJoySnap* s);                                   // false se nao ha controle
std::string wdc_joy_map_key(uint16_t vid, uint16_t pid);                // "dev_2dc8_6012"
std::string wdc_joy_map_text(uint16_t vid, uint16_t pid);               // mapa em uso (aprendido ou do perfil), no formato do config
void wdc_joy_apply_map(uint16_t vid, uint16_t pid, const std::string& text);   // usa este mapa agora (o setup grava no arquivo)
