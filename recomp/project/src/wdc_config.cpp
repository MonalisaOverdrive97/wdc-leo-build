// Configuracao do jogo (ver wdc_config.h). Le um subconjunto de TOML: [secao], chave = valor (booleano, inteiro, "texto"), comentarios com #.
#include "wdc_config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace {
WdcConfig g_cfg;
std::map<std::string, std::string> g_overrides;

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// valor: remove aspas e comentario no fim da linha
std::string clean_value(const std::string& raw) {
    std::string v = trim(raw);
    if (!v.empty() && v[0] == '"') {
        const size_t e = v.find('"', 1);
        return e == std::string::npos ? v.substr(1) : v.substr(1, e - 1);
    }
    const size_t h = v.find('#');
    if (h != std::string::npos) v = trim(v.substr(0, h));
    return v;
}

std::map<std::string, std::string> read_file(const std::filesystem::path& path) {
    std::map<std::string, std::string> kv;
    std::ifstream f(path);
    std::string line, section;
    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t[0] == '[') {
            const size_t e = t.find(']');
            section = lower(trim(t.substr(1, (e == std::string::npos ? t.size() : e) - 1)));
            continue;
        }
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trim(t.substr(0, eq)));
        kv[section.empty() ? key : section + "." + key] = clean_value(t.substr(eq + 1));
    }
    return kv;
}

const char* kDefaultFile = R"(# Configuracao do World Driver Championship (recomp).
# Precedencia: linha de comando > este arquivo > padrao do modo.

# "legacy"   = recompilacao pura, como o jogo rodaria no Nintendo 64 (30 Hz em corrida, resolucao nativa).
# "enhanced" = 60 fps e melhorias opcionais abaixo. Cada modo usa o seu proprio Controller Pak (wdc_pak0.mpk / wdc_pak0_enhanced.mpk),
#              porque recordes e replays nao sao portaveis entre os modos.
mode = "legacy"

[enhanced]
# So valem com mode = "enhanced" (no Legacy sao ignoradas).
fps60 = true            # corrida a 60 Hz (demo da abertura e replays continuam a 30 Hz, como foram gravados)
internal_scale = "auto" # resolucao interna do RDP: "auto" (4x em placa de video dedicada, 2x em integrada), 1, 2, 4 ou 8
sharp_textures = true   # texturas nitidas: troca o filtro de 3 pontos do N64 por amostragem por ponto (tira o borrao/riscos no cenario em movimento; false = visual original)

[display]
# Valem nos dois modos: so mudam como a imagem chega a tela, nao a emulacao.
vsync = "on"            # "on" = sincroniza com o monitor (sem rasgos); "off" = apresenta assim que o quadro fica pronto (com VRR/G-Sync, sem rasgos)
filter = "sharp"        # escalonamento: "nearest", "bilinear", "sharp" (bilinear nitido) ou "bicubic" (o mais suave)
aspect = "4:3"          # "4:3" = proporcao original com barras pretas; "stretch" = preenche a janela (deforma a imagem)
fullscreen = false      # abrir em tela cheia (F11 alterna durante o jogo)
backend = "d3d11"       # "d3d11" (GPU, v-sync) ou "gdi" (antigo, sem v-sync)

[controls]
# Esquema de botoes do controle XInput (vale nos dois modos).
# "classic" = como no Nintendo 64 (A acelera, B freia, R/Z marchas, analogico direito = botoes C).
# "modern"  = em corrida: RT acelera, LT freia, X (Quadrado) desce marcha, A (X) sobe marcha, Y troca a visao; nos menus, igual ao classico.
scheme = "classic"      # "classic" | "modern"

# Botoes personalizados (opcional; so o que for diferente do padrao precisa aparecer). Valores: A B X Y LB RB LT RT L3 R3 BACK, juntos com "+" (ex.: "B+X"),
# "none" (sem botao) ou "default". [controls.classic] vale em todo o jogo; [controls.modern] so em corrida ao vivo (nos menus vale o classico).
# [controls.classic]
# accelerate = "A"        # A do N64: acelera
# brake      = "B+X"      # B do N64: freio forte e re
# soft_brake = "LB"       # L do N64: freio suave
# shift_up   = "RB"       # R do N64: marcha acima (cambio manual)
# shift_down = "LT+RT"    # Z do N64: marcha abaixo
# camera     = "Y"        # C-cima do N64: troca a visao
)";

bool parse_bool(const std::string& v, bool def) {
    const std::string s = lower(v);
    if (s == "true" || s == "on" || s == "1" || s == "yes") return true;
    if (s == "false" || s == "off" || s == "0" || s == "no") return false;
    return def;
}
}  // namespace

void wdc_cfg_set_override(const std::string& key, const std::string& value) { g_overrides[lower(key)] = value; }

void wdc_cfg_load(const std::filesystem::path& config_dir) {
    const auto path = config_dir / "wdc_config.toml";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::ofstream f(path);
        f << kDefaultFile;
        std::printf("[cfg] criado %s (modo padrao: legacy)\n", path.string().c_str());
    }
    std::map<std::string, std::string> kv = read_file(path);
    for (const auto& [k, v] : g_overrides) kv[k] = v;   // linha de comando vence
    auto get = [&](const char* key) -> const std::string* { auto it = kv.find(key); return it == kv.end() ? nullptr : &it->second; };

    WdcConfig c;
    if (const auto* m = get("mode")) {
        const std::string s = lower(*m);
        if (s == "enhanced") c.enhanced = true;
        else if (s != "legacy") std::printf("[cfg] mode=\"%s\" desconhecido: usando legacy\n", m->c_str());
    }
    // padroes do modo; so o Enhanced aceita as melhorias
    const bool want_fps = get("enhanced.fps60") ? parse_bool(*get("enhanced.fps60"), true) : true;
    const bool want_sharp = get("enhanced.sharp_textures") ? parse_bool(*get("enhanced.sharp_textures"), true) : true;
    int want_scale = 2;
    bool want_auto = true;   // padrao: automatico
    if (const auto* s = get("enhanced.internal_scale")) {
        if (lower(*s) == "auto" || *s == "0") {
            want_auto = true;
        } else {
            want_auto = false;
            want_scale = std::atoi(s->c_str());
            if (want_scale == 8) {   // 8x (2560x1920) travou o jogo nos testes (RTX 4050): limitado a 4x
                std::printf("[cfg] internal_scale=8 nao e suportado nesta versao: usando 4\n");
                want_scale = 4;
            } else if (want_scale != 1 && want_scale != 2 && want_scale != 4) {
                std::printf("[cfg] internal_scale=%s invalido: usando automatico\n", s->c_str());
                want_auto = true;
                want_scale = 2;
            }
        }
    }
    c.scale_choice = want_auto ? 0 : want_scale;
    c.sharp_textures_choice = want_sharp;
    c.filter_explicit = get("display.filter") != nullptr;
    if (c.enhanced) {
        c.fps60 = want_fps;
        c.sharp_textures = want_sharp;
        c.internal_scale = want_scale;   // se auto, o renderizador resolve (wdc_cfg_resolve_auto_scale) depois de escolher a GPU
        c.scale_auto = want_auto;
    } else {
        c.fps60 = false;
        c.sharp_textures = false;
        c.internal_scale = 1;
        if (g_overrides.count("enhanced.fps60") || g_overrides.count("enhanced.internal_scale"))
            std::printf("[cfg] AVISO: --fps60/--scale ignorados no modo Legacy (use --mode enhanced)\n");
    }
    if (const char* e = std::getenv("WDC_RDP_SCALE")) {   // ferramenta de desenvolvimento: vale em qualquer modo
        const int s = std::atoi(e);
        if (s == 1 || s == 2 || s == 4 || s == 8) { c.internal_scale = s; c.scale_auto = false; std::printf("[cfg] WDC_RDP_SCALE=%d sobrescreve a escala interna\n", s); }
    }
    if (const auto* v = get("display.vsync")) c.vsync = parse_bool(*v, true) ? WdcVsync::On : WdcVsync::Off;
    if (const auto* v = get("display.filter")) {
        const std::string s = lower(*v);
        c.filter = s == "nearest" ? WdcFilter::Nearest : s == "bilinear" ? WdcFilter::Bilinear : s == "sharp" ? WdcFilter::Sharp : s == "bicubic" ? WdcFilter::Bicubic
                 : (c.enhanced ? WdcFilter::Sharp : WdcFilter::Bilinear);
    } else {
        c.filter = c.enhanced ? WdcFilter::Sharp : WdcFilter::Bilinear;
    }
    if (const auto* v = get("display.backend")) c.backend = lower(*v) == "gdi" ? WdcBackend::GDI : WdcBackend::D3D11;
    if (const auto* v = get("display.aspect")) {
        const std::string s = lower(*v);
        c.aspect = (s == "stretch" || s == "fill" || s == "16:9") ? WdcAspect::Stretch : WdcAspect::Original;
    }
    if (const auto* v = get("display.fullscreen")) c.fullscreen = parse_bool(*v, false);
    if (const auto* v = get("controls.scheme")) c.controls = lower(*v) == "modern" ? WdcControls::Modern : WdcControls::Classic;
    for (int s = 0; s < 2; s++)
        for (int a = 0; a < kWdcActions; a++) {
            const std::string key = std::string(s == 0 ? "controls.classic." : "controls.modern.") + kWdcActionKeys[a];
            if (const auto* v = get(key.c_str())) c.bind[s][a] = *v;
        }
    for (const auto& [k, v] : kv)   // [gamepad] dev_<vid>_<pid> = mapa de botoes aprendido no setup
        if (k.rfind("gamepad.dev_", 0) == 0) c.joymap[k.substr(8)] = v;
    g_cfg = c;
    const char* fn = c.filter == WdcFilter::Nearest ? "nearest" : c.filter == WdcFilter::Bilinear ? "bilinear" : c.filter == WdcFilter::Sharp ? "sharp" : "bicubic";
    char sc[16];
    if (c.scale_auto) std::snprintf(sc, sizeof sc, "auto"); else std::snprintf(sc, sizeof sc, "%dx", c.internal_scale);
    std::printf("[cfg] modo=%s fps60=%s escala_interna=%s vsync=%s filtro=%s tela=%s%s apresentador=%s controles=%s (%s)\n", wdc_cfg_mode_name(), c.fps60 ? "on" : "off", sc,
                c.vsync == WdcVsync::On ? "on" : "off", fn, c.aspect == WdcAspect::Stretch ? "esticada" : "4:3", c.fullscreen ? " (tela cheia)" : "",
                c.backend == WdcBackend::D3D11 ? "d3d11" : "gdi", c.controls == WdcControls::Modern ? "modern" : "classic", path.string().c_str());
}

const char* const kWdcActionKeys[kWdcActions] = {"accelerate", "brake", "soft_brake", "shift_up", "shift_down", "camera"};

void wdc_cfg_resolve_auto_scale(bool discrete_gpu) {
    if (!g_cfg.enhanced || !g_cfg.scale_auto) return;
    g_cfg.internal_scale = discrete_gpu ? 4 : 2;
    g_cfg.scale_auto = false;
    std::printf("[cfg] escala interna automatica: %dx (GPU %s)\n", g_cfg.internal_scale, discrete_gpu ? "dedicada" : "integrada");
}

bool wdc_cfg_save_value(const std::filesystem::path& config_dir, const std::string& section, const std::string& key, const std::string& value_text) {
    const auto path = config_dir / "wdc_config.toml";
    std::vector<std::string> lines;
    {
        std::ifstream f(path);
        std::string l;
        while (std::getline(f, l)) lines.push_back(l);
    }
    const std::string want_section = lower(section);
    std::string cur;                       // secao corrente ("" = topo do arquivo)
    int section_end = -1;                  // indice da ultima linha de conteudo da secao pedida (para inserir uma chave nova)
    bool section_found = want_section.empty();
    if (want_section.empty()) section_end = -1;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string t = trim(lines[i]);
        if (!t.empty() && t[0] == '[') {
            const size_t e = t.find(']');
            cur = lower(trim(t.substr(1, (e == std::string::npos ? t.size() : e) - 1)));
            if (cur == want_section) { section_found = true; section_end = (int)i; }
            continue;
        }
        if (cur != want_section) continue;
        if (t.empty() || t[0] == '#') continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        if (lower(trim(t.substr(0, eq))) == lower(key)) {   // chave existe: troca o valor e mantem o comentario do fim da linha
            const size_t hash = lines[i].find('#', lines[i].find('=') + 1);
            lines[i] = key + " = " + value_text + (hash == std::string::npos ? "" : "   " + lines[i].substr(hash));
            std::ofstream o(path);
            for (const auto& s : lines) o << s << std::endl;
            return (bool)o;
        }
        section_end = (int)i;
    }
    const std::string new_line = key + " = " + value_text;
    if (want_section.empty()) {   // chave de topo: antes da primeira secao
        size_t pos = 0;
        while (pos < lines.size() && (trim(lines[pos]).empty() || trim(lines[pos])[0] != '[')) pos++;
        lines.insert(lines.begin() + (long)pos, new_line);
    } else if (section_found) {
        lines.insert(lines.begin() + section_end + 1, new_line);
    } else {
        lines.push_back("");
        lines.push_back("[" + section + "]");
        lines.push_back(new_line);
    }
    std::ofstream o(path);
    for (const auto& s : lines) o << s << std::endl;
    return (bool)o;
}

const WdcConfig& wdc_cfg() { return g_cfg; }
const char* wdc_cfg_mode_name() { return g_cfg.enhanced ? "Enhanced" : "Legacy"; }
