// Configuracao do jogo: modo Legacy (recompilacao pura, como no Nintendo 64) ou Enhanced (60 fps e melhorias opcionais).
// Precedencia (maior primeiro): linha de comando > arquivo <config>/wdc_config.toml > padrao do modo.
// Legacy NAO aceita nenhuma melhoria que altere a emulacao (60 fps, resolucao interna): sao ignoradas com aviso. As opcoes de [display]
// (v-sync, filtro, apresentador) so mudam como a imagem chega a tela e valem nos dois modos.
#pragma once
#include <filesystem>
#include <map>
#include <string>

enum class WdcVsync { On, Off };                       // Off = apresenta assim que o quadro fica pronto (com VRR/G-Sync, sem rasgos)
enum class WdcFilter { Nearest, Bilinear, Sharp, Bicubic };   // escalonamento da imagem na janela (Bicubic = Catmull-Rom, o mais suave)
enum class WdcAspect { Original, Stretch };           // Original = 4:3 com barras pretas; Stretch = preenche a janela (deforma a imagem)
enum class WdcBackend { D3D11, GDI };                  // apresentador em GPU (D3D11) ou o antigo GDI (sem v-sync)
enum class WdcControls { Classic, Modern };            // esquema de botoes do controle XInput (ver wdc_input.h)

struct WdcConfig {
    bool enhanced = false;          // modo Enhanced
    bool fps60 = false;             // efetivo: so em Enhanced. Corrida a 60 Hz (demo e replay continuam a 30 Hz)
    bool sharp_textures_choice = true;   // o que o arquivo/linha de comando pede (padrao ligado), valha ou nao no modo atual (o setup mostra isto)
    bool sharp_textures = false;    // efetivo: so em Enhanced. Textura amostrada por ponto (sem o filtro de 3 pontos do N64, que espalha texturas ampliadas em riscos)
    int internal_scale = 1;         // efetivo: 1 em Legacy; 1, 2, 4 ou 8 em Enhanced (resolucao interna do RDP)
    WdcVsync vsync = WdcVsync::On;
    WdcFilter filter = WdcFilter::Bilinear;
    WdcBackend backend = WdcBackend::D3D11;
    WdcControls controls = WdcControls::Classic;   // vale nos dois modos: so remapeia botoes no host
    int scale_choice = 0;           // o que o arquivo/linha de comando pede (0 = auto, 1, 2, 4, 8), valha ou nao no modo atual (o setup mostra isto)
    bool filter_explicit = false;   // display.filter veio do arquivo/linha de comando (senao o padrao depende do modo)
    bool scale_auto = false;        // enhanced.internal_scale = "auto": a escala e escolhida pela GPU (wdc_cfg_resolve_auto_scale, chamada pelo renderizador)
    WdcAspect aspect = WdcAspect::Original;
    bool fullscreen = false;        // abrir em tela cheia (F11 alterna)
    // Botoes personalizados do controle XInput, por esquema (0 = classico, 1 = moderno) e acao (ver wdc_input.h, WdcAction). Texto cru da chave
    // [controls.classic]/[controls.modern] (ex.: "RT", "B+X", "none"); vazio ou "default" = o padrao do esquema. Interpretado por wdc_input.cpp.
    std::string bind[2][6];
    // Mapas de botoes aprendidos de controles que nao sao XInput: chave "dev_<vid>_<pid>" -> "A:0,B:1,..." (ver wdc_joy.h; [gamepad] em wdc_config.toml).
    std::map<std::string, std::string> joymap;
};
constexpr int kWdcActions = 6;
extern const char* const kWdcActionKeys[kWdcActions];   // nomes das chaves: accelerate, brake, soft_brake, shift_up, shift_down, camera
void wdc_cfg_resolve_auto_scale(bool discrete_gpu);     // internal_scale = "auto": 4x em GPU dedicada, 2x em integrada (so Enhanced)

// Chaves aceitas: "mode", "enhanced.fps60", "enhanced.internal_scale", "display.vsync", "display.filter" (nearest|bilinear|sharp|bicubic), "display.aspect" (4:3|stretch), "display.fullscreen", "display.backend", "controls.scheme",
// "controls.classic.<acao>" e "controls.modern.<acao>"; "enhanced.internal_scale" aceita "auto" alem de 1|2|4|8.
void wdc_cfg_set_override(const std::string& key, const std::string& value);   // linha de comando (antes de wdc_cfg_load)
void wdc_cfg_load(const std::filesystem::path& config_dir);                    // le (ou cria) wdc_config.toml, resolve e imprime o resumo
// Grava uma chave em wdc_config.toml preservando os comentarios e as demais chaves (cria a secao/chave se faltar). Usado pelo setup.
bool wdc_cfg_save_value(const std::filesystem::path& config_dir, const std::string& section, const std::string& key, const std::string& value_text);
const WdcConfig& wdc_cfg();
const char* wdc_cfg_mode_name();                                               // "Legacy" ou "Enhanced"
