// Backend de RDP com ParaLLEl-RDP (Vulkan). Ver wdc_rdp_vk.h.
//
// Fluxo: a thread de RSP (wdc_rsp_hw.cpp, DPC_END) entrega comandos RDP completos -> fila -> thread de RDP -> CommandProcessor.
// SYNC_FULL: enqueue_command + wait_for_timeline(signal_timeline()) e so entao OS_EVENT_DP (a GPU terminou; o framebuffer/Z em RDRAM,
// que e o proprio RDRAM do jogo importado via VK_EXT_external_memory_host, esta coerente). Scanout: registradores de VI do runtime.
#include "wdc_rdp_vk.h"
#include "wdc_config.h"
#include "wdc_logs.h"
#include "wdc_present.h"
#include "wdc_window.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "context.hpp"
#include "device.hpp"
#include "rdp_device.hpp"
#include "thread_id.hpp"
#include "miniz.h"

bool wdc_post_dp();     // wdc_runtime_stubs.cpp
uint64_t wdc_vi_ticks();           // main.cpp
uint64_t wdc_vi_updates_done();    // main.cpp
double wdc_now_s();     // main.cpp

namespace RDP { extern bool g_wdc_point_sampling; }   // rdp_device.cpp (patch WDC)

namespace {
struct Job {
    enum Kind { Cmds, Vi, Fb, Quit } kind = Cmds;
    std::string path;               // Fb
    uint32_t fb_addr = 0, fb_w = 0, fb_h = 0, fb_bits = 0;
    std::vector<uint32_t> words;
    uint32_t vi[14] = {};
    int64_t t_us = 0;               // Vi: instante em que o retrace entrou na fila (wdc_present_now_us)
};

std::unique_ptr<Vulkan::Context> g_ctx;
std::unique_ptr<Vulkan::Device> g_dev;
std::unique_ptr<RDP::CommandProcessor> g_proc;

std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<Job> g_queue;

// PORTAO DE BUFFER (modo Enhanced, 60 Hz). O jogo tem 2 framebuffers. Com o laco a 60 Hz ele redesenha no buffer X_i enquanto X_i ainda aguarda o seu scanout
// (o ultramodern exibe um buffer um retraco DEPOIS do osViSwapBuffer): quadros "intrusos" (meio quadro novo, cheio de lixo) piscando na tela. No original a 30 Hz
// a 2a espera de VI de func_800013DC impedia isso. Aqui: osViSwapBuffer marca o buffer como pendente; um lote de comandos com SET_COLOR_IMAGE num buffer pendente
// so executa depois do scanout dele (o Vi correspondente e executado antes, fora de ordem, se ja estiver na fila; senao espera ate 30 ms).
std::vector<uint32_t> g_pending_scan;       // enderecos fisicos (24 bits) trocados e ainda nao exibidos; protegido por g_mutex
// MEIO-QUADRO (Enhanced): o scanout (foto da tela, feito a cada retraco de VI) pode cair no meio do desenho do quadro seguinte no MESMO buffer: aparece o carro so
// com as rodas, o fundo incompleto, "chiados". A thread do RDP sabe quando esta desenhando (do 1o comando de desenho ate o SYNC_FULL): se o scanout pede o buffer em
// obras, reapresenta o quadro anterior (no maximo 2 vezes seguidas, para nunca congelar a tela).
// FILTRO DE QUADRO INTRUSO (Enhanced). A 60 Hz o jogo desenha o proximo quadro no buffer que o VI ainda esta exibindo (a 2a espera de VI foi removida) e a GPU
// pode ainda estar no meio do quadro no instante do scanout: aparece um quadro pela metade (so o ceu, so o fundo verde, carro so com as rodas) por 1/60 s.
// A thread do RDP segura cada quadro escaneado por UM quadro e o compara com o anterior (P) e o seguinte (F): se X difere muito de P e de F, e P ~ F
// (padrao P,X,F de um quadro isolado), X e uma piscada e P e reexibido no lugar. Custo: 1 quadro (16,7 ms) de latencia; o apresentador ganha 1 quadro de folga.
// WDC_NO_DEGLITCH=1 desliga (para comparar).
bool g_deglitch = false;
struct HeldFrame { std::vector<RDP::RGBA> px; std::vector<uint8_t> grid; unsigned w = 0, h = 0; int64_t t_us = 0; bool valid = false; };
HeldFrame g_dg_prev, g_dg_held;           // P (ultimo exibido) e X (segurado)
std::atomic<uint64_t> g_deglitched{0};
double g_dg_T = [] { const char* e = std::getenv("WDC_DEGLITCH_T"); return e ? std::atof(e) : 8.0; }();
std::atomic<bool> g_gate_on{false};
// O origin do VI aponta para o buffer (ou uma linha adiante, em campos): casa se estiver nos primeiros 16 KB do buffer (os dois buffers distam ~300 KB).
inline bool origin_in_buffer(uint32_t origin, uint32_t buf) { return (uint32_t)(origin - buf) < 0x4000u; }
std::atomic<uint64_t> g_gate_holds{0}, g_gate_reorders{0}, g_gate_timeouts{0};
std::thread g_thread;
std::atomic<bool> g_active{false};

std::string g_shot_path;
double g_shot_after = 0.0;
std::atomic<bool> g_shot_pending{false}, g_shot_done{false};
int g_shot_remaining = 0;          // capturas adicionais (alem da primeira)
double g_shot_interval = 1.0;      // segundos entre capturas
int g_shot_index = 0;
std::string shot_name() {          // 1a captura: caminho como pedido; as seguintes: sufixo _NN antes da extensao
    if (g_shot_index == 0) return g_shot_path;
    std::string p = g_shot_path;
    const size_t dot = p.rfind('.');
    char suf[16];
    std::snprintf(suf, sizeof(suf), "_%02d", g_shot_index);
    p.insert(dot == std::string::npos ? p.size() : dot, suf);
    return p;
}

std::atomic<uint64_t> g_cmds{0}, g_syncs{0}, g_frames{0}, g_scanouts{0};
double g_sync_wait_total = 0.0, g_sync_wait_max = 0.0;
// Travadas (diagnostico para o log): histograma do intervalo entre quadros (SYNC_FULL a SYNC_FULL) e das CPU-ms gastas por quadro so enfileirando comandos (inclui compilacao de pipelines).
// Faixas de intervalo em ms: <20, 20-30, 30-40, 40-60, 60-100, 100-200, >=200.
uint64_t g_gap_hist[7] = {0, 0, 0, 0, 0, 0, 0};
uint64_t g_slow_cpu_frames = 0;
double g_cpu_ms_max = 0.0;
// Custo da thread do RDP (para achar o gargalo em PCs fracos): scanout (VI na GPU + leitura de volta + entrega), CPU enfileirando e espera da GPU por quadro.
double g_vi_ms_total = 0.0, g_vi_ms_max = 0.0, g_scan_ms_total = 0.0, g_scan_ms_max = 0.0, g_enq_ms_total = 0.0;
uint64_t g_busy_hist[7] = {0, 0, 0, 0, 0, 0, 0};   // CPU enfileirando + espera da GPU por quadro (ms): <8, <16, <24, <30, <36, <50, >=50
uint64_t g_vi_ticks = 0, g_vi_dup_candidates = 0;
uint32_t g_vi_latest[14] = {};
uint8_t* g_rdram_ptr = nullptr;
// Cache de pipelines do Vulkan em disco (%LOCALAPPDATA%/WorldDriverChampionship/cache, um arquivo por placa): as proximas execucoes nao recompilam os shaders do RDP que ja foram usados
// (compilar um shader novo no meio da corrida e uma causa classica de travadas raras). O arquivo e validado pelo UUID do driver e por um hash; se nao servir, o Vulkan cria um cache novo.
std::filesystem::path pipeline_cache_file() {
    const char* la = std::getenv("LOCALAPPDATA");
    if (!la || !*la || !g_dev) return {};
    const auto& pr = g_dev->get_gpu_properties();
    char n[64];
    std::snprintf(n, sizeof n, "pipeline_cache_%04x_%04x.bin", (unsigned)pr.vendorID, (unsigned)pr.deviceID);
    return std::filesystem::path(la) / "WorldDriverChampionship" / "cache" / n;
}
void load_pipeline_cache() {
    const auto p = pipeline_cache_file();
    if (p.empty()) return;
    std::ifstream f(p, std::ios::binary);
    if (!f) {   // primeira execucao: cria o cache vazio (sem ele o Vulkan nao guarda nada e nao ha o que gravar)
        std::printf("[rdp-vk] cache de pipelines: sem arquivo anterior (%s); criando um novo\n", p.string().c_str());
        g_dev->init_pipeline_cache(nullptr, 0);
        return;
    }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (g_dev->init_pipeline_cache(d.data(), d.size())) std::printf("[rdp-vk] cache de pipelines carregado: %zu KB\n", d.size() / 1024);
}
void save_pipeline_cache() {
    const auto p = pipeline_cache_file();
    if (p.empty()) return;
    const size_t sz = g_dev->get_pipeline_cache_size();
    if (!sz) return;
    std::vector<uint8_t> d(sz);
    if (!g_dev->get_pipeline_cache_data(d.data(), d.size())) return;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    const auto tmp = std::filesystem::path(p).concat(".tmp");
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return;
        o.write((const char*)d.data(), (std::streamsize)d.size());
    }
    std::filesystem::rename(tmp, p, ec);
    std::printf("[rdp-vk] cache de pipelines gravado: %zu KB\n", d.size() / 1024);
}
inline uint32_t rdram_word(uint32_t phys) { return g_rdram_ptr ? *(const volatile uint32_t*)(g_rdram_ptr + phys) : 0u; }
bool g_have_vi = false;

// =================================================================================================================================
// DESEMPENHO EM PCs FRACOS
// (1) REPETICAO DE SCANOUT. O VI roda a 60 Hz e, em Legacy, o jogo desenha 30 quadros/s: metade dos scanouts reapresenta a MESMA imagem. Cada scanout e caro (passe de VI na
//     GPU, espera, leitura de volta de ~1 MB, copia e envio ao apresentador: 2-3 ms numa GPU rapida, bem mais numa fraca). Se os registradores de VI sao os mesmos, nenhum comando
//     de desenho foi para o buffer exibido desde o ultimo scanout e o conteudo desse buffer na RDRAM (hash) nao mudou, a imagem e identica: o quadro anterior e reapresentado
//     sem refazer nada. WDC_NO_SCAN_REPEAT=1 desliga; WDC_SCAN_VERIFY=1 refaz o scanout mesmo assim e compara os pixels (teste: tem que dar 0 diferentes).
// (2) GOVERNADOR DE QUADROS (display.performance = "auto"). Se a GPU/CPU nao acompanha, a thread do RDP bloqueia o jogo em cada SYNC_FULL e ele desacelera/engasga. O governador
//     mede quanto custa desenhar um quadro e, se nao cabe no periodo do jogo, PULA o desenho de alguns quadros (os comandos de estado e de carga de textura passam, os de
//     desenho nao; o SYNC_FULL e atendido na hora): o jogo segue em tempo real e a tela mostra o ultimo quadro desenhado. Num PC que acompanha, nunca entra em acao.
// =================================================================================================================================
using PresentRepeatFn = void (*)(int64_t);
PresentRepeatFn g_present_repeat = nullptr;
const bool g_scan_repeat_enabled = std::getenv("WDC_NO_SCAN_REPEAT") == nullptr;
const bool g_scan_verify = std::getenv("WDC_SCAN_VERIFY") != nullptr;
const double g_fake_frame_ms = std::getenv("WDC_FAKE_FRAME_MS") ? std::atof(std::getenv("WDC_FAKE_FRAME_MS")) : 0.0;   // teste: GPU lenta simulada (ms por quadro desenhado)
const double g_fake_scan_ms = std::getenv("WDC_FAKE_SCAN_MS") ? std::atof(std::getenv("WDC_FAKE_SCAN_MS")) : 0.0;     // teste: ms extras por scanout real
const double g_gov_force = std::getenv("WDC_GOV_FORCE") ? std::atof(std::getenv("WDC_GOV_FORCE")) : 0.0;              // teste: fracao fixa de quadros desenhados (0 = automatico)
size_t g_rdram_size = 0x400000;

struct ScanMemo { bool valid = false; uint32_t regs[14] = {}; uint64_t hash = 0; std::vector<RDP::RGBA> px; unsigned w = 0, h = 0; uint64_t cmds = 0; bool mid_frame = false; } g_memo;
uint32_t g_dirty[8]; int g_dirty_n = 0; bool g_dirty_all = false;   // color images (24 bits) que receberam desenho desde o ultimo scanout real
uint32_t g_stale[8]; int g_stale_n = 0;                            // color images cujo ultimo quadro foi PULADO (conteudo velho: nao serve de scanout)
uint32_t g_cur_cimg = 0;                                           // SET_COLOR_IMAGE vigente no fluxo de comandos
bool g_verify_candidate = false;
int g_repeat_run = 0;   // repeticoes seguidas: no maximo 1 (o par de ticks de VI de cada quadro de 30 Hz); limita a um tick qualquer erro de repeticao
uint64_t g_scan_repeats = 0, g_scan_verify_ok = 0, g_scan_verify_bad = 0, g_stale_ticks = 0, g_gov_skipped_cmds = 0, g_verify_ndiff = 0;
unsigned g_verify_maxd = 0;
uint64_t g_verify_big = 0;

// Bytes do framebuffer exibido (so o que o VI le: largura x linhas x bytes por pixel, mais uma folga para os filtros). 0 = VI apagado.
uint32_t vi_fb_span(const uint32_t* r) {
    const uint32_t bpp = (r[0] & 3) == 3 ? 4u : (r[0] & 3) == 2 ? 2u : 0u;   // VI_STATUS: tipo de pixel (2 = 16 bits, 3 = 32 bits)
    const uint32_t width = r[2] & 0xFFF;
    if (!bpp || !width) return 0;
    const int half = (int)(r[10] & 0x3FF) - (int)((r[10] >> 16) & 0x3FF);   // V_START em meias-linhas
    const uint32_t lines = (uint32_t)(half > 0 ? half >> 1 : 0) * (r[13] & 0xFFF) / 1024u + 8u;   // Y_SCALE em ponto fixo 2.10
    const uint32_t origin = r[1] & 0xFFFFFF;
    uint32_t span = width * bpp * lines;
    if (origin >= g_rdram_size) return 0;
    if (span > g_rdram_size - origin) span = (uint32_t)(g_rdram_size - origin);
    return span;
}
uint64_t vi_fb_hash(const uint32_t* r, uint32_t span) {
    const uint32_t origin = (r[1] & 0xFFFFFF) & ~7u;
    if (!g_rdram_ptr || origin >= g_rdram_size) return 0;
    if (span > g_rdram_size - origin) span = (uint32_t)(g_rdram_size - origin);
    const uint8_t* p = g_rdram_ptr + origin;
    constexpr uint64_t K = 0x9E3779B97F4A7C15ull;
    uint64_t h0 = 0x243F6A8885A308D3ull, h1 = 0x13198A2E03707344ull, h2 = 0xA4093822299F31D0ull, h3 = 0x082EFA98EC4E6C89ull;
    size_t n = span / 32;
    for (; n; n--, p += 32) {
        uint64_t a, b, c, d;
        std::memcpy(&a, p, 8); std::memcpy(&b, p + 8, 8); std::memcpy(&c, p + 16, 8); std::memcpy(&d, p + 24, 8);
        h0 = (h0 ^ a) * K; h0 ^= h0 >> 32;
        h1 = (h1 ^ b) * K; h1 ^= h1 >> 32;
        h2 = (h2 ^ c) * K; h2 ^= h2 >> 32;
        h3 = (h3 ^ d) * K; h3 ^= h3 >> 32;
    }
    for (size_t t = (span % 32) / 8; t; t--, p += 8) { uint64_t a; std::memcpy(&a, p, 8); h0 = (h0 ^ a) * K; h0 ^= h0 >> 32; }
    return h0 ^ (h1 * 3) ^ (h2 * 5) ^ (h3 * 7) ^ span;
}
inline bool vi_same_regs(const uint32_t* a, const uint32_t* b) {   // V_CURRENT (4) so conta a paridade (campo, em modo entrelacado)
    for (int i = 0; i < 14; i++) {
        if (i == 4) { if ((a[4] ^ b[4]) & 1u) return false; continue; }
        if (a[i] != b[i]) return false;
    }
    return true;
}
inline bool near_buf(uint32_t a, uint32_t b, uint32_t span) { return (int32_t)(a - b) < (int32_t)span && (int32_t)(b - a) < (int32_t)span; }   // dois buffers do mesmo formato se sobrepoem
void dirty_add(uint32_t c) {
    for (int i = 0; i < g_dirty_n; i++) if (g_dirty[i] == c) return;
    if (g_dirty_n < 8) g_dirty[g_dirty_n++] = c; else g_dirty_all = true;
}
bool dirty_hits(uint32_t origin, uint32_t span) {
    if (g_dirty_all) return true;
    for (int i = 0; i < g_dirty_n; i++) if (near_buf(g_dirty[i], origin, span)) return true;
    return false;
}
void stale_add(uint32_t c) {
    for (int i = 0; i < g_stale_n; i++) if (g_stale[i] == c) return;
    if (g_stale_n == 8) { std::memmove(g_stale, g_stale + 1, 7 * sizeof(uint32_t)); g_stale_n--; }
    g_stale[g_stale_n++] = c;
}
void stale_remove(uint32_t c) {
    for (int i = 0; i < g_stale_n; i++) if (g_stale[i] == c) { std::memmove(g_stale + i, g_stale + i + 1, (size_t)(g_stale_n - i - 1) * sizeof(uint32_t)); g_stale_n--; return; }
}
bool stale_hits(uint32_t origin) {   // o origin do VI e a base do buffer (ou uma linha adiante, em campos): casamento exato; um falso positivo congelaria a tela
    for (int i = 0; i < g_stale_n; i++) if ((uint32_t)(origin - g_stale[i]) < 0x4000u) return true;
    return false;
}

void write_png(const char* path, const std::vector<RDP::RGBA>& px, unsigned w, unsigned h) {
    std::vector<uint8_t> rgba(w * h * 4);
    for (size_t i = 0; i < px.size(); i++) { rgba[4 * i] = px[i].r; rgba[4 * i + 1] = px[i].g; rgba[4 * i + 2] = px[i].b; rgba[4 * i + 3] = 255; }
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory(rgba.data(), (int)w, (int)h, 4, &len);
    if (!png) { std::printf("[rdp-vk] falha ao codificar PNG\n"); return; }
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(png, 1, len, f); std::fclose(f); std::printf("[rdp-vk %7.2fs] captura: %s (%ux%u, %zu bytes)\n", wdc_now_s(), path, w, h, len); }
    else std::printf("[rdp-vk] nao consegui abrir %s\n", path);
    mz_free(png);
    if (h < w / 2) {   // scanout de um campo (ex. 640x240): grava tambem uma versao 4:3 com as linhas duplicadas
        std::vector<uint8_t> tall(w * h * 2 * 4);
        for (unsigned y = 0; y < h; y++)
            for (int k = 0; k < 2; k++) std::memcpy(&tall[((size_t)(2 * y + k) * w) * 4], &rgba[(size_t)y * w * 4], (size_t)w * 4);
        size_t len2 = 0;
        void* png2 = tdefl_write_image_to_png_file_in_memory(tall.data(), (int)w, (int)(h * 2), 4, &len2);
        std::string p2 = std::string(path);
        const size_t dot = p2.rfind('.');
        p2.insert(dot == std::string::npos ? p2.size() : dot, "_4x3");
        if (png2) {
            if (FILE* f = std::fopen(p2.c_str(), "wb")) { std::fwrite(png2, 1, len2, f); std::fclose(f); std::printf("[rdp-vk] captura 4:3: %s (%ux%u)\n", p2.c_str(), w, h * 2); }
            mz_free(png2);
        }
    }
}

using PresentFn = void (*)(const uint8_t*, unsigned, unsigned, int64_t);
PresentFn g_present = nullptr;              // modo interativo: recebe cada quadro do scanout (RGBA8)

// Governador de quadros (ver o comentario do bloco de desempenho). Roda so na thread do RDP.
struct Governor {
    bool user_enabled = true;                  // display.performance = "auto"
    double period_ms = 1000.0 / 30.0;          // periodo do quadro do jogo (Legacy 30 Hz; Enhanced 60 Hz)
    double frac = 1.0, credit = 1.0;           // fracao dos quadros que sao desenhados; acumulador
    double cost_ema = 0.0, scan_ema = 0.0, spf = 1.0;   // custo de um quadro desenhado (ms), de um scanout real (ms), scanouts reais por quadro desenhado
    int cost_n = 0, scan_n = 0;
    bool in_frame = false, skip = false;
    std::chrono::steady_clock::time_point frame_start;
    uint32_t cimgs[8]; int cimg_n = 0;         // color images vistos no quadro corrente
    int since_update = 0;
    uint64_t rendered = 0, skipped = 0, rendered_at_update = 0, scans_at_update = 0;
    double min_frac = 1.0;
    double gap_ema = 0.0;                      // intervalo medio (relogio real, ms) entre quadros do jogo (SYNC_FULL a SYNC_FULL)
    std::chrono::steady_clock::time_point last_end;
    bool have_last = false;
    int log_lines = 0;
    double logged_frac = 1.0;
} g_gov;

bool gov_decide_skip() {
    if (!g_gov.user_enabled || !g_present) return false;
    if (g_gov_force > 0.0 && g_gov_force < 1.0) g_gov.frac = g_gov_force;
    if (g_gov.frac >= 0.999) { g_gov.credit = 1.0; return false; }
    g_gov.credit += g_gov.frac;
    if (g_gov.credit >= 1.0) { g_gov.credit -= 1.0; return false; }
    return true;
}

void gov_scan_sample(double ms) {
    g_gov.scan_ema = g_gov.scan_n == 0 ? ms : g_gov.scan_ema + 0.1 * (std::min(ms, 100.0) - g_gov.scan_ema);
    g_gov.scan_n++;
}

void gov_update() {
    if (!g_gov.user_enabled || !g_present || g_gov_force > 0.0) return;
    if (++g_gov.since_update < 8 || g_gov.cost_n < 8 || g_syncs.load() < 150) return;   // so depois de ~5 s: a abertura (shaders, carga) nao conta
    g_gov.since_update = 0;
    // So entra em acao se o jogo ja esta ATRASADO (intervalo medio > 10% acima do periodo): o RDP trabalha em paralelo com o jogo, e custo alto sem atraso nao pede nada.
    // Ja em acao (frac < 1), quem decide e o modelo de custo ate ele dizer que cabe tudo.
    if (g_gov.frac >= 1.0 && g_gov.gap_ema < 1.10 * g_gov.period_ms) return;
    const uint64_t dr = g_gov.rendered - g_gov.rendered_at_update, ds = g_scanouts.load() - g_gov.scans_at_update;
    g_gov.rendered_at_update = g_gov.rendered; g_gov.scans_at_update = g_scanouts.load();
    if (dr) g_gov.spf += 0.3 * (std::max(0.5, std::min(2.5, (double)ds / (double)dr)) - g_gov.spf);
    const double unit = g_gov.cost_ema + g_gov.scan_ema * g_gov.spf;       // custo na thread do RDP por quadro desenhado
    const double budget = 0.78 * g_gov.period_ms;                           // folga de 22% para o resto (RSP, audio, apresentacao)
    double target = unit > 0.01 ? budget / unit : 1.0;
    target = std::max(0.34, std::min(1.0, target));
    if (target >= 0.97) target = 1.0;
    const double before = g_gov.frac;
    g_gov.frac = target < g_gov.frac ? target : std::min(target, g_gov.frac + 0.05);   // desce na hora, sobe devagar
    if (g_gov.frac < g_gov.min_frac) g_gov.min_frac = g_gov.frac;
    if (std::fabs(g_gov.frac - g_gov.logged_frac) >= 0.1 || (g_gov.frac >= 1.0 && g_gov.logged_frac < 1.0) || (before >= 1.0 && g_gov.frac < 1.0)) {
        if (g_gov.log_lines++ < 60)
            std::printf("[gov %7.2fs] desenhando %.0f%% dos quadros (custo do quadro %.1f ms + scanout %.1f ms x %.2f; orcamento %.1f ms de %.1f)\n", wdc_now_s(), g_gov.frac * 100.0,
                        g_gov.cost_ema, g_gov.scan_ema, g_gov.spf, budget, g_gov.period_ms);
        g_gov.logged_frac = g_gov.frac;
    }
}

// Fim de um quadro (SYNC_FULL atendido). L = do primeiro lote do quadro ate a GPU terminar; W = espera da GPU no SYNC_FULL; enq = CPU enfileirando o ultimo lote.
// ---------------------------------------------------------------------------------------------------------------------------------
// ORDEM DE EXIBICAO (Enhanced). Sem a 2a espera de VI (hook de func_800013DC) o jogo desenha o proximo quadro no buffer que o VI ainda exibe. A 60 Hz isso so produz
// quadros pela metade (g_deglitch); nas telas de 30 Hz (abertura, replay, demo) produz algo pior: cada buffer fica 2 ticks na tela e o quadro novo e desenhado nele no
// SEGUNDO tick, entao o primeiro tick de cada par mostra o quadro de DOIS quadros atras: a imagem anda para frente e para tras (F0 F2 F1 F3 F2 F4...), o "pipocar" e o motion
// blur estranho da abertura e dos replays. (Medido na abertura: 68 trocas de buffer a cada tick contra 1 no Legacy; cada buffer = 941 comandos no 2o tick.)
// Correcao: cada quadro COMPLETO (SYNC_FULL) recebe um numero de serie, gravado nos color images que ele usou; se o VI pede um buffer de serie MENOR que a do ultimo quadro
// exibido, reapresenta-se o ultimo quadro em vez de voltar no tempo (no maximo 1 vez seguida, para nunca congelar a tela por engano). WDC_NO_ORDER=1 desliga (comparar).
// ---------------------------------------------------------------------------------------------------------------------------------
struct BufSerial { uint32_t base; uint64_t serial; };
BufSerial g_bufser[8]; int g_bufser_n = 0;
uint64_t g_frame_serial = 0, g_last_shown_serial = 0, g_back_repeats = 0;
int g_back_run = 0;
bool g_order_on = false;
std::vector<RDP::RGBA> g_last_px; unsigned g_last_w = 0, g_last_h = 0;
void bufser_set(uint32_t base, uint64_t serial) {
    for (int i = 0; i < g_bufser_n; i++) if (g_bufser[i].base == base) { g_bufser[i].serial = serial; return; }
    if (g_bufser_n < 8) g_bufser[g_bufser_n++] = {base, serial};
}
uint64_t bufser_get(uint32_t origin) {   // o origin do VI e a base do buffer (ou uma linha adiante, em campos)
    for (int i = 0; i < g_bufser_n; i++) if ((uint32_t)(origin - g_bufser[i].base) < 0x4000u) return g_bufser[i].serial;
    return 0;
}

void gov_frame_end(double L_ms, double W_ms, double enq_ms) {
    {
        const auto now = std::chrono::steady_clock::now();
        if (g_gov.have_last) {
            const double gap = std::min(200.0, std::chrono::duration<double, std::milli>(now - g_gov.last_end).count());
            g_gov.gap_ema = g_gov.gap_ema == 0.0 ? gap : g_gov.gap_ema + 0.1 * (gap - g_gov.gap_ema);
        }
        g_gov.last_end = now; g_gov.have_last = true;
    }
    if (g_gov.skip) {
        g_gov.skipped++;
        for (int k = 0; k < g_gov.cimg_n; k++) stale_add(g_gov.cimgs[k]);
    } else {
        g_gov.rendered++;
        for (int k = 0; k < g_gov.cimg_n; k++) stale_remove(g_gov.cimgs[k]);
        if (g_order_on) { ++g_frame_serial; for (int k = 0; k < g_gov.cimg_n; k++) bufser_set(g_gov.cimgs[k], g_frame_serial); }
        // So custa ao governador o que PULAR quadros alivia: se a GPU acompanhou a producao dos comandos (W ~ 0), o tempo L e do RSP/jogo, nao do RDP.
        const double sample = std::min(W_ms >= 1.5 ? L_ms : W_ms + enq_ms, 2.0 * g_gov.period_ms);   // uma travada isolada (compilacao de shader) nao vale por muitos quadros
        g_gov.cost_ema = g_gov.cost_n == 0 ? sample : g_gov.cost_ema + 0.12 * (sample - g_gov.cost_ema);
        g_gov.cost_n++;
    }
    g_gov.in_frame = false;
    g_gov.cimg_n = 0;
    gov_update();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Detector de "quadro intruso" (WDC_FLICKER=<pasta>): faz o scanout de TODO quadro, reduz cada um a uma grade 80x60 de luminancia e procura o padrao
// A, X, A' (X difere muito de A e de A', mas A ~ A'): um quadro isolado diferente dos vizinhos. Grava os 3 quadros em PNG e o log de VI/tempo.
// ---------------------------------------------------------------------------------------------------------------------------------
const char* g_flick_dir = std::getenv("WDC_FLICKER");
const double g_flick_T = [] { const char* e = std::getenv("WDC_FLICKER_T"); return e ? std::atof(e) : 8.0; }();
constexpr int kGW = 80, kGH = 60;
struct FlickFrame { std::vector<RDP::RGBA> px; std::vector<uint8_t> grid; unsigned w = 0, h = 0; uint32_t origin = 0, status = 0; double t = 0; uint64_t n = 0; std::vector<uint32_t> drawn; uint32_t cimg_cur = 0; uint64_t ndraw = 0; };
uint32_t g_cimg_cur = 0; uint64_t g_ndraw_since = 0;   // color image corrente e comandos de desenho (triangulos, rects, fill) desde o scanout anterior
std::vector<uint32_t> g_cimg_since;   // enderecos de SET_COLOR_IMAGE vistos nos comandos processados desde o scanout anterior (so a thread do RDP)
FlickFrame g_ff[3];            // [0] = N-2, [1] = N-1, [2] = N (mais recente)
uint32_t g_tl_tri = 0, g_tl_rect = 0, g_tl_timg = 0;   // WDC_TRILOG: triangulos/retangulos/SET_TEX_IMAGE por quadro (diagnostico de LOD)
uint64_t g_flick_frames = 0, g_flick_found = 0, g_flick_big = 0, g_flick_found2 = 0;
uint64_t g_origin_runs[6] = {0, 0, 0, 0, 0, 0};   // quantos ticks de VI seguidos mostram o MESMO buffer (origin): 1, 2, 3, 4, 5, 6+ (Legacy: 2; corrida a 60 Hz: 1)
uint32_t g_origin_prev = 0xFFFFFFFFu; int g_origin_run = 0;

std::vector<uint8_t> make_grid(const std::vector<RDP::RGBA>& px, unsigned w, unsigned h) {
    std::vector<uint8_t> g((size_t)kGW * kGH, 0);
    for (int gy = 0; gy < kGH; gy++)
        for (int gx = 0; gx < kGW; gx++) {
            const unsigned x0 = gx * w / kGW, x1 = (gx + 1) * w / kGW, y0 = gy * h / kGH, y1 = (gy + 1) * h / kGH;
            unsigned long sum = 0, n = 0;
            const unsigned sy = std::max(1u, (y1 - y0) / 4), sx = std::max(2u, (x1 - x0) / 8);   // no maximo ~4x8 amostras por celula (resolucao interna alta nao custa mais)
            for (unsigned y = y0; y < y1; y += sy)
                for (unsigned x = x0; x < x1; x += sx) {
                    const RDP::RGBA& c = px[(size_t)y * w + x];
                    sum += (unsigned long)(c.r * 77 + c.g * 150 + c.b * 29) >> 8;
                    n++;
                }
            g[(size_t)gy * kGW + gx] = n ? (uint8_t)(sum / n) : 0;
        }
    return g;
}
double grid_mad(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    unsigned long s = 0;
    for (size_t i = 0; i < a.size(); i++) s += (unsigned long)std::abs((int)a[i] - (int)b[i]);
    return (double)s / (double)a.size();
}
void flicker_feed(const std::vector<RDP::RGBA>& px, unsigned w, unsigned h) {
    g_ff[0] = std::move(g_ff[1]);
    g_ff[1] = std::move(g_ff[2]);
    FlickFrame& f = g_ff[2];
    f.px = px; f.w = w; f.h = h; f.grid = make_grid(px, w, h);
    f.origin = g_vi_latest[1]; f.status = g_vi_latest[0]; f.t = wdc_now_s(); f.n = ++g_flick_frames; f.drawn = g_cimg_since; g_cimg_since.clear(); f.cimg_cur = g_cimg_cur; f.ndraw = g_ndraw_since; g_ndraw_since = 0;
    {   // padrao de DOIS quadros intrusos (A,X,Y,B: X e Y diferem muito de A e de B, A ~ B, X ~ Y): o filtro de 1 quadro nao pega esse caso
        static std::vector<uint8_t> r4g[4];
        static std::vector<RDP::RGBA> r4p[4];
        static unsigned r4w[4] = {0, 0, 0, 0}, r4h[4] = {0, 0, 0, 0};
        for (int k = 0; k < 3; k++) { r4g[k] = std::move(r4g[k + 1]); r4p[k] = std::move(r4p[k + 1]); r4w[k] = r4w[k + 1]; r4h[k] = r4h[k + 1]; }
        r4g[3] = f.grid; r4p[3] = px; r4w[3] = w; r4h[3] = h;
        if (!r4g[0].empty() && r4w[0] == w && r4w[1] == w && r4w[2] == w && r4h[0] == h && r4h[1] == h && r4h[2] == h) {
            const double dax = grid_mad(r4g[0], r4g[1]), dby = grid_mad(r4g[2], r4g[3]), dab = grid_mad(r4g[0], r4g[3]), dxy = grid_mad(r4g[1], r4g[2]);
            if (dax > g_flick_T && dby > g_flick_T && dab < 0.5 * std::min(dax, dby) && dxy < 0.5 * std::min(dax, dby)) {
                g_flick_found2++;
                std::printf("[flicker2 %7.2fs] DOIS quadros intrusos #%llu: d(A,X)=%.1f d(Y,B)=%.1f d(A,B)=%.1f d(X,Y)=%.1f\n", wdc_now_s(), (unsigned long long)g_flick_found2, dax, dby, dab, dxy);
                if (g_flick_found2 <= 20) {
                    char path[600];
                    const char* nm[4] = {"a", "x", "y", "b"};
                    for (int k = 0; k < 4; k++) {
                        std::snprintf(path, sizeof path, "%s/flicker2_%04llu_%s.png", g_flick_dir, (unsigned long long)g_flick_found2, nm[k]);
                        write_png(path, r4p[k], r4w[k], r4h[k]);
                    }
                }
            }
        }
    }
    if (g_ff[0].grid.empty() || g_ff[0].w != w || g_ff[1].w != w || g_ff[0].h != h || g_ff[1].h != h) return;
    const double d21 = grid_mad(g_ff[0].grid, g_ff[1].grid), d10 = grid_mad(g_ff[1].grid, g_ff[2].grid), d20 = grid_mad(g_ff[0].grid, g_ff[2].grid);
    {   // WDC_FLICKER_TRACE=t0,t1: uma linha por tick de VI nessa janela de tempo (origin, color images desenhadas desde o scanout anterior, comandos de desenho)
        static double tr0 = -1, tr1 = -1;
        static bool tr_init = false;
        if (!tr_init) { tr_init = true; if (const char* e = std::getenv("WDC_FLICKER_TRACE")) std::sscanf(e, "%lf,%lf", &tr0, &tr1); }
        if (tr0 >= 0 && g_ff[2].t >= tr0 && g_ff[2].t <= tr1) {
            std::printf("[tick %7.2fs] #%llu origin=0x%06X luma(grade)=%.1f desenhado:", g_ff[2].t, (unsigned long long)g_ff[2].n, g_ff[2].origin & 0xFFFFFF, grid_mad(g_ff[1].grid, g_ff[2].grid));
            for (uint32_t a : g_ff[2].drawn) std::printf(" %06X", a & 0xFFFFFF);
            std::printf(" (cimg %06X, %llu cmds)\n", g_ff[2].cimg_cur & 0xFFFFFF, (unsigned long long)g_ff[2].ndraw);
        }
    }
    {
        const uint32_t ob = (g_ff[2].origin & 0xFFFFFF) >> 12;   // buffer = origin sem a linha de campo
        if (ob == g_origin_prev) g_origin_run++;
        else { if (g_origin_run > 0) g_origin_runs[std::min(g_origin_run, 6) - 1]++; g_origin_prev = ob; g_origin_run = 1; }
    }
    if (d21 > g_flick_T) {
        g_flick_big++;
        static const bool jl = std::getenv("WDC_FLICKER_LOG") != nullptr;
        if (jl && g_flick_big <= 400) {
            std::printf("[flick-jump %7.2fs] quadro #%llu: salto %.1f (origin ant=0x%06X atual=0x%06X, STATUS 0x%X); desenhado antes deste scanout:", g_ff[1].t, (unsigned long long)g_ff[1].n, d21, g_ff[0].origin & 0xFFFFFF, g_ff[1].origin & 0xFFFFFF, g_ff[1].status);
            for (uint32_t a : g_ff[1].drawn) std::printf(" %06X", a & 0xFFFFFF);
            std::printf(" (cimg %06X, %llu cmds de desenho)\n", g_ff[1].cimg_cur & 0xFFFFFF, (unsigned long long)g_ff[1].ndraw);
        }
    }
    if (d21 > g_flick_T && d10 > g_flick_T && d20 < 0.5 * std::min(d21, d10)) {
        g_flick_found++;
        std::printf("[flicker %7.2fs] quadro #%llu intruso: d(ant,x)=%.1f d(x,prox)=%.1f d(ant,prox)=%.1f; VI_ORIGIN ant=0x%06X x=0x%06X prox=0x%06X STATUS x=0x%X\n",
                    g_ff[1].t, (unsigned long long)g_ff[1].n, d21, d10, d20, g_ff[0].origin & 0xFFFFFF, g_ff[1].origin & 0xFFFFFF, g_ff[2].origin & 0xFFFFFF, g_ff[1].status);
        for (int k = 1; k <= 2; k++) {
            std::printf("[flicker]    color images desenhadas ANTES do scanout do quadro %s:", k == 1 ? "x" : "prox");
            for (uint32_t a : g_ff[k].drawn) std::printf(" %06X", a & 0xFFFFFF);
            std::printf("  | color image corrente 0x%06X, %llu comandos de desenho", g_ff[k].cimg_cur & 0xFFFFFF, (unsigned long long)g_ff[k].ndraw);
            std::printf("\n");
        }
        if (g_flick_found <= 40) {
            char path[600];
            const char* nm[3] = {"ant", "x", "prox"};
            for (int k = 0; k < 3; k++) {
                std::snprintf(path, sizeof path, "%s/flicker_%04llu_%s.png", g_flick_dir, (unsigned long long)g_flick_found, nm[k]);
                write_png(path, g_ff[k].px, g_ff[k].w, g_ff[k].h);
            }
        }
    }
}

// Entrega um quadro ao apresentador. Com o filtro ligado, o quadro fica um quadro atrasado: ver g_deglitch.
void deliver(std::vector<RDP::RGBA>& px, unsigned w, unsigned h, int64_t t_vi_us) {
    if (!w || !h) return;
    auto out = [&](const std::vector<RDP::RGBA>& p, unsigned pw, unsigned ph, int64_t t) {
        if (g_flick_dir) flicker_feed(p, pw, ph);   // o detector enxerga o que o jogador veria
        if (g_present) g_present((const uint8_t*)p.data(), pw, ph, t);
    };
    if (!g_deglitch) { out(px, w, h, t_vi_us); return; }
    HeldFrame f;
    f.grid = make_grid(px, w, h);
    f.px = std::move(px);
    f.w = w; f.h = h; f.t_us = t_vi_us; f.valid = true;
    if (g_dg_held.valid) {
        HeldFrame& x = g_dg_held;
        bool glitch = false;
        if (g_dg_prev.valid && g_dg_prev.w == w && g_dg_prev.h == h && x.w == w && x.h == h) {
            const double dpx = grid_mad(g_dg_prev.grid, x.grid), dxf = grid_mad(x.grid, f.grid), dpf = grid_mad(g_dg_prev.grid, f.grid);
            glitch = dpx > g_dg_T && dxf > g_dg_T && dpf < 0.5 * std::min(dpx, dxf);
        }
        if (glitch) {
            g_deglitched++;
            out(g_dg_prev.px, g_dg_prev.w, g_dg_prev.h, x.t_us);   // reexibe P no lugar da piscada
        } else {
            out(x.px, x.w, x.h, x.t_us);
            g_dg_prev = std::move(x);
        }
    }
    g_dg_held = std::move(f);
}

void do_scanout(bool want_shot, int64_t t_vi_us) {
    for (int r = 0; r < 14; r++) g_proc->set_vi_register((RDP::VIRegister)r, g_vi_latest[r]);
    RDP::ScanoutOptions opts;
    opts.persist_frame_on_invalid_input = true;
    {   // Filtros do VI que o ParaLLEl-RDP reproduz (o N64 aplicava todos). WDC_VI_OFF=aa,scale,serrate,dither,divot,gamma desliga os listados (ferramenta de teste, vale em qualquer modo)
        static const char* off = std::getenv("WDC_VI_OFF");
        auto has = [&](const char* k) { return off && std::strstr(off, k) != nullptr; };
        if (has("aa")) opts.vi.aa = false;
        if (has("scale")) opts.vi.scale = false;
        if (has("serrate")) opts.vi.serrate = false;
        if (has("dither")) opts.vi.dither_filter = false;
        if (has("divot")) opts.vi.divot_filter = false;
        if (has("gamma")) opts.vi.gamma_dither = false;
        if (std::getenv("WDC_VI_WEAVE")) opts.upscale_deinterlacing = false;
    }
    std::vector<RDP::RGBA> px;
    unsigned w = 0, h = 0;
    const auto ts0 = std::chrono::steady_clock::now();
    if (g_fake_scan_ms > 0.0) std::this_thread::sleep_for(std::chrono::microseconds((long long)(g_fake_scan_ms * 1000.0)));
    g_proc->scanout_sync(px, w, h, opts);
    {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts0).count();
        g_scan_ms_total += ms;
        if (ms > g_scan_ms_max) g_scan_ms_max = ms;
    }
    g_scanouts++;
    if (g_order_on && w && h) { g_last_px = px; g_last_w = w; g_last_h = h; }
    if (w && h && g_scan_repeat_enabled) {   // memoria do ultimo scanout real (ver REPETICAO DE SCANOUT)
        const uint32_t span = vi_fb_span(g_vi_latest);
        if (span) {
            if (g_scan_verify && g_memo.valid && g_verify_candidate) {   // teria sido repetido: a imagem tem que ser identica
                const bool dims = g_memo.w == w && g_memo.h == h && g_memo.px.size() == px.size();
                unsigned maxd = 0;
                size_t nd = 0;
                if (dims) {
                    const uint8_t* a = (const uint8_t*)g_memo.px.data();
                    const uint8_t* b = (const uint8_t*)px.data();
                    for (size_t q = 0; q < px.size(); q++) {
                        unsigned d = 0;
                        for (int c = 0; c < 3; c++) d = std::max<unsigned>(d, (unsigned)std::abs((int)a[4 * q + c] - (int)b[4 * q + c]));
                        if (d) { nd++; maxd = std::max(maxd, d); }
                    }
                }
                if (dims && !nd) g_scan_verify_ok++;
                else {
                    g_scan_verify_bad++; g_verify_ndiff += nd; g_verify_maxd = std::max(g_verify_maxd, dims ? maxd : 999u);
                    static int dbg = 0;
                    if (!dims || maxd > 2) g_verify_big++;   // ruido de dither do VI (+-1) nao conta
                    if (dbg < 6 && (!dims || maxd > 2)) {   // depuracao: os dois quadros e os registradores de VI (WDC_VERIFY_DIR = pasta)
                        const char* dir = std::getenv("WDC_VERIFY_DIR");
                        std::printf("[verify] ndiff=%zu maxd=%u dims=%d span=%u cmds=%llu (memo em %llu; memo no meio de um quadro: %d; agora no meio de um quadro: %d; sujos: %d%s; cimg atual %06X)\n  regs novo: ", nd, maxd, (int)dims, span,
                                    (unsigned long long)g_cmds.load(), (unsigned long long)g_memo.cmds, (int)g_memo.mid_frame, (int)g_gov.in_frame, g_dirty_n, g_dirty_all ? "+tudo" : "", g_cur_cimg);
                        for (int q = 0; q < 14; q++) std::printf("%08X ", g_vi_latest[q]);
                        std::printf("\n  regs memo: ");
                        for (int q = 0; q < 14; q++) std::printf("%08X ", g_memo.regs[q]);
                        std::printf("\n");
                        if (dir) {
                            char pa[512], pb[512];
                            std::snprintf(pa, sizeof pa, "%s/bad%d_memo.png", dir, dbg);
                            std::snprintf(pb, sizeof pb, "%s/bad%d_new.png", dir, dbg);
                            write_png(pa, g_memo.px, g_memo.w, g_memo.h);
                            write_png(pb, px, w, h);
                        }
                        dbg++;
                    }
                }
            }
            std::memcpy(g_memo.regs, g_vi_latest, sizeof g_memo.regs);
            g_memo.hash = vi_fb_hash(g_vi_latest, span);
            g_memo.w = w; g_memo.h = h; g_memo.valid = true; g_memo.cmds = g_cmds.load(); g_memo.mid_frame = g_gov.in_frame;
            if (g_scan_verify) g_memo.px = px;
            g_dirty_n = 0; g_dirty_all = false;
        } else g_memo.valid = false;
    } else g_memo.valid = false;
    if (want_shot && w && h) {
        write_png(shot_name().c_str(), px, w, h);
        g_shot_index++;
        if (g_shot_remaining > 0) { g_shot_remaining--; g_shot_after = wdc_now_s() + g_shot_interval; }
        else { g_shot_pending = false; g_shot_done = true; }
    }
    deliver(px, w, h, t_vi_us);   // por ultimo: pode consumir px
}

void process_vi(Job& j) {   // Vi: fim de quadro
    {   // diagnostico (WDC_VI_LOG=1): registra cada mudanca de modo do VI (STATUS, WIDTH, X/Y scale...)
        static const bool vl = std::getenv("WDC_VI_LOG") != nullptr;
        static uint32_t last[14] = {};
        static int lines = 0;
        uint32_t cur_cmp[14]; std::memcpy(cur_cmp, j.vi, sizeof cur_cmp); cur_cmp[1] = 0; cur_cmp[4] = 0;   // o ORIGIN (troca de buffer) e a linha atual nao contam como mudanca de modo
        if (vl && lines < 200 && std::memcmp(last, cur_cmp, sizeof(last)) != 0) {
            lines++;
            std::printf("[vi %7.2fs] STATUS=%08X ORIGIN=%08X WIDTH=%u V_INTR=%u V_CUR=%u BURST=%08X V_SYNC=%u H_SYNC=%08X LEAP=%08X H_START=%08X V_START=%08X V_BURST=%08X X_SCALE=%08X Y_SCALE=%08X\n",
                        wdc_now_s(), j.vi[0], j.vi[1], j.vi[2], j.vi[3], j.vi[4], j.vi[5], j.vi[6], j.vi[7], j.vi[8], j.vi[9], j.vi[10], j.vi[11], j.vi[12], j.vi[13]);
            std::memcpy(last, cur_cmp, sizeof(last));
        }
    }
    std::memcpy(g_vi_latest, j.vi, sizeof(g_vi_latest));
    g_have_vi = true;
    g_frames++;
    const bool shot = g_shot_pending.load() && wdc_now_s() >= g_shot_after && g_syncs.load() > 0;
    g_vi_ticks++;
    bool repeated = false;
    if (g_present_repeat && g_present && !shot && !g_flick_dir) {
        const uint32_t origin = j.vi[1] & 0xFFFFFF, span = vi_fb_span(j.vi);
        if (span && g_stale_n && stale_hits(origin)) {   // governador: este buffer guarda um quadro pulado (velho): continua a imagem anterior
            g_present_repeat(j.t_us);
            g_stale_ticks++;
            repeated = true;
        } else if (g_scan_repeat_enabled && !g_deglitch && span && g_memo.valid && g_repeat_run < 1 && vi_same_regs(g_memo.regs, j.vi) && !dirty_hits(origin, span)) {
            g_vi_dup_candidates++;
            if (vi_fb_hash(j.vi, span) == g_memo.hash) {
                if (g_scan_verify) g_verify_candidate = true;   // modo de teste: faz o scanout de verdade e compara
                else { g_present_repeat(j.t_us); g_scan_repeats++; g_repeat_run++; repeated = true; }
            }
        }
    }
    if (g_order_on && !repeated && !shot && (g_present || g_flick_dir)) {   // ordem de exibicao: nunca voltar no tempo (ver ORDEM DE EXIBICAO)
        static uint32_t prev_mode[6] = {};
        const uint32_t cur_mode[6] = {j.vi[0] & ~0x10000u, j.vi[2], j.vi[9], j.vi[10], j.vi[12], j.vi[13]};   // o bit 16 do STATUS e o campo
        if (std::memcmp(prev_mode, cur_mode, sizeof prev_mode) != 0) { std::memcpy(prev_mode, cur_mode, sizeof prev_mode); g_last_shown_serial = 0; }   // modo de video novo: recomeca
        const uint64_t s = bufser_get(j.vi[1] & 0xFFFFFF);
        if (s && s < g_last_shown_serial && g_back_run < 1 && !g_last_px.empty() && g_last_w && g_last_h) {
            std::vector<RDP::RGBA> c = g_last_px;
            deliver(c, g_last_w, g_last_h, j.t_us);   // reapresenta o ultimo quadro (passa pelo mesmo filtro que os reais)
            g_back_run++; g_back_repeats++;
            repeated = true;
        } else if (s) g_last_shown_serial = s;
    }
    if (!repeated && (shot || g_present || g_flick_dir)) {
        const auto tv0 = std::chrono::steady_clock::now();
        do_scanout(shot, j.t_us);
        g_verify_candidate = false;
        g_repeat_run = 0;
        g_back_run = 0;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tv0).count();
        g_vi_ms_total += ms;
        if (ms > g_vi_ms_max) g_vi_ms_max = ms;
        gov_scan_sample(ms);
    }
    g_proc->begin_frame_context();
    if (g_gate_on.load()) {   // o buffer exibido deixa de estar pendente
        const uint32_t origin = j.vi[1] & 0xFFFFFF;
        std::lock_guard<std::mutex> l(g_mutex);
        g_pending_scan.erase(std::remove_if(g_pending_scan.begin(), g_pending_scan.end(), [&](uint32_t a) { return origin_in_buffer(origin, a); }), g_pending_scan.end());
    }
}

// Segura o lote de comandos que desenharia num buffer ainda pendente de scanout (ver g_pending_scan).
void gate_wait(const Job& j) {
    uint32_t targets[8];
    int nt = 0;
    for (size_t i = 0; i < j.words.size();) {
        const unsigned n = j.words[i];
        const uint32_t* w = &j.words[i + 1];
        if (((w[0] >> 24) & 0x3F) == 0x3F && n >= 2 && nt < 8) targets[nt++] = w[1] & 0xFFFFFF;   // SET_COLOR_IMAGE
        i += n + 1;
    }
    if (!nt) return;
    const auto t0 = std::chrono::steady_clock::now();
    bool held = false;
    std::unique_lock<std::mutex> l(g_mutex);
    while (true) {
        uint32_t hit = 0;
        bool any = false;
        for (uint32_t a : g_pending_scan) for (int k = 0; k < nt; k++) if (a == targets[k]) { hit = a; any = true; }
        if (!any) break;
        held = true;
        auto it = std::find_if(g_queue.begin(), g_queue.end(), [&](const Job& q) { return q.kind == Job::Vi && origin_in_buffer(q.vi[1] & 0xFFFFFF, hit); });
        if (it != g_queue.end()) {   // o scanout ja chegou: executa-o AGORA (antes deste lote)
            Job vi = std::move(*it);
            g_queue.erase(it);
            l.unlock();
            process_vi(vi);
            l.lock();
            g_gate_reorders++;
            continue;
        }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(30)) {   // o scanout nao veio (modo de video mudou, tela apagada): libera
            g_pending_scan.erase(std::remove(g_pending_scan.begin(), g_pending_scan.end(), hit), g_pending_scan.end());
            if (++g_gate_timeouts > 40 && g_gate_on.exchange(false)) std::printf("[rdp-vk] portao de buffer DESLIGADO (scanouts nao chegam)\n");
            continue;
        }
        g_cv.wait_for(l, std::chrono::milliseconds(2));
    }
    if (held) g_gate_holds++;
}

void thread_main() {
    wdc_set_thread_name(L"wdc-rdp");
    wdc_thread_above_normal();   // o jogo espera por esta thread em cada SYNC_FULL
    Util::register_thread_index(0);   // so esta thread usa o dispositivo depois da init; sem registro o parallel-rdp loga um erro a cada chamada
    while (true) {
        Job j;
        {
            std::unique_lock<std::mutex> l(g_mutex);
            g_cv.wait(l, [] { return !g_queue.empty(); });
            j = std::move(g_queue.front());
            g_queue.pop_front();
        }
        if (j.kind == Job::Quit) return;
        if (j.kind == Job::Fb) {
            std::vector<RDP::RGBA> px((size_t)j.fb_w * j.fb_h);
            for (uint32_t y = 0; y < j.fb_h; y++)
                for (uint32_t x = 0; x < j.fb_w; x++) {
                    RDP::RGBA c{};
                    if (j.fb_bits == 16) {   // RGBA5551; palavras do RDRAM guardadas invertidas por byte: meia-palavra com ^2
                        const uint32_t off = (j.fb_addr + (y * j.fb_w + x) * 2) & 0x7FFFFF;
                        const uint16_t v = (uint16_t)((g_rdram_ptr[(off ^ 2) + 1] << 8) | g_rdram_ptr[(off ^ 2)]);
                        c.r = (uint8_t)(((v >> 11) & 31) * 255 / 31); c.g = (uint8_t)(((v >> 6) & 31) * 255 / 31); c.b = (uint8_t)(((v >> 1) & 31) * 255 / 31); c.a = 255;
                    } else {                 // RGBA8888: a palavra lida em ordem do host ja e o valor big-endian
                        const uint32_t off = (j.fb_addr + (y * j.fb_w + x) * 4) & 0x7FFFFC;
                        const uint32_t v = *(const uint32_t*)(g_rdram_ptr + off);
                        c.r = (uint8_t)(v >> 24); c.g = (uint8_t)(v >> 16); c.b = (uint8_t)(v >> 8); c.a = 255;
                    }
                    px[(size_t)y * j.fb_w + x] = c;
                }
            write_png(j.path.c_str(), px, j.fb_w, j.fb_h);
            continue;
        }
        if (j.kind == Job::Cmds) {
            const auto tj0 = std::chrono::steady_clock::now();
            if (g_gate_on.load()) gate_wait(j);
            size_t i = 0;
            while (i < j.words.size()) {
                const unsigned n = j.words[i];
                const uint32_t* w = &j.words[i + 1];
                {   // diagnostico (WDC_RDPDUMP=<pasta>@<segundo>): grava em texto os comandos do RDP de 2 quadros a partir do instante dado (opcode + palavras dos comandos de estado)
                    static const char* dd = std::getenv("WDC_RDPDUMP");
                    static FILE* df = nullptr;
                    static double t_start = -1.0;
                    static int frames_left = 2;
                    if (dd && frames_left > 0) {
                        if (t_start < 0.0) { const char* at = std::strchr(dd, '@'); t_start = at ? std::atof(at + 1) : 20.0; }
                        if (!df && wdc_now_s() >= t_start) {
                            std::string pth(dd, std::strchr(dd, '@') ? (size_t)(std::strchr(dd, '@') - dd) : std::strlen(dd));
                            df = std::fopen((pth + "/rdp_dump.txt").c_str(), "w");
                        }
                        if (df) {
                            static const char* nm[64] = {};
                            nm[0x08] = "TRI"; nm[0x09] = "TRI_Z"; nm[0x0A] = "TRI_TEX"; nm[0x0B] = "TRI_TEX_Z"; nm[0x0C] = "TRI_SHADE"; nm[0x0D] = "TRI_SHADE_Z"; nm[0x0E] = "TRI_SHADE_TEX";
                            nm[0x0F] = "TRI_SHADE_TEX_Z"; nm[0x24] = "TEXRECT"; nm[0x25] = "TEXRECT_FLIP"; nm[0x26] = "SYNC_LOAD"; nm[0x27] = "SYNC_PIPE"; nm[0x28] = "SYNC_TILE";
                            nm[0x29] = "SYNC_FULL"; nm[0x2D] = "SET_SCISSOR"; nm[0x2F] = "SET_OTHER_MODES"; nm[0x30] = "LOAD_TLUT"; nm[0x32] = "SET_TILE_SIZE"; nm[0x33] = "LOAD_BLOCK";
                            nm[0x34] = "LOAD_TILE"; nm[0x35] = "SET_TILE"; nm[0x36] = "FILL_RECT"; nm[0x37] = "SET_FILL_COLOR"; nm[0x38] = "SET_FOG_COLOR"; nm[0x39] = "SET_BLEND_COLOR";
                            nm[0x3A] = "SET_PRIM_COLOR"; nm[0x3B] = "SET_ENV_COLOR"; nm[0x3C] = "SET_COMBINE"; nm[0x3D] = "SET_TEX_IMAGE"; nm[0x3E] = "SET_Z_IMAGE"; nm[0x3F] = "SET_COLOR_IMAGE";
                            const uint32_t op = (w[0] >> 24) & 0x3F;
                            std::fprintf(df, "%-16s n=%u", nm[op] ? nm[op] : "?", n);
                            const unsigned nw = op >= 0x08 && op <= 0x0F ? n : std::min(n, 4u);
                            for (unsigned q = 0; q < nw && q < n; q++) std::fprintf(df, " %08X", w[q]);
                            std::fprintf(df, "\n");
                            if (op == 0x29 && --frames_left == 0) { std::fclose(df); df = nullptr; std::printf("[rdpdump] gravado\n"); }
                        }
                    }
                }
                {   // diagnostico (WDC_FBTEX=1): quantos desenhos por segundo usam um FRAMEBUFFER como textura (efeitos de blur/feedback do jogo)
                    static const bool fbdbg = std::getenv("WDC_FBTEX") != nullptr;
                    if (fbdbg) {
                        const uint32_t op = (w[0] >> 24) & 0x3F;
                        static uint32_t timg = 0, cimg = 0;
                        static uint64_t n_fb = 0, n_other = 0;
                        static double last_print = 0.0;
                        if (op == 0x3F && n >= 2) cimg = w[1] & 0xFFFFFF;
                        if (op == 0x3D && n >= 2) timg = w[1] & 0xFFFFFF;
                        const bool tex_fb = timg && ((timg - 0x0E3880u) < 0x4B000u || (timg - 0x12E880u) < 0x4B000u);
                        if (op == 0x24 || op == 0x25 || (op >= 0x0A && op <= 0x0F && (op & 2))) { if (tex_fb) n_fb++; else n_other++; }
                        const double now = wdc_now_s();
                        if (now - last_print >= 1.0) {
                            if (last_print > 0.0 && (n_fb || n_other)) std::printf("[fbtex %6.1fs] desenhos texturizados: %llu com FRAMEBUFFER como textura, %llu com outras; ultimo cimg=%06X timg=%06X\n", now, (unsigned long long)n_fb, (unsigned long long)n_other, cimg, timg);
                            last_print = now;
                            n_fb = n_other = 0;
                        }
                    }
                }
                if (g_flick_dir && ((w[0] >> 24) & 0x3F) == 0x3F && n >= 2) {   // SET_COLOR_IMAGE
                    const uint32_t a = w[1] & 0xFFFFFF;
                    if (g_cimg_since.empty() || g_cimg_since.back() != a) g_cimg_since.push_back(a);
                    g_cimg_cur = a;
                }
                if (g_flick_dir) { const uint32_t op = (w[0] >> 24) & 0x3F; if ((op >= 0x08 && op <= 0x0F) || op == 0x24 || op == 0x25 || op == 0x36) g_ndraw_since++; }
                const uint32_t op_g = (w[0] >> 24) & 0x3F;
                { static const bool tl = std::getenv("WDC_TRILOG") != nullptr; if (tl) { if (op_g >= 0x08 && op_g <= 0x0F) g_tl_tri++; else if (op_g == 0x24 || op_g == 0x25) g_tl_rect++; else if (op_g == 0x3D) g_tl_timg++; } }
                if (!g_gov.in_frame) { g_gov.in_frame = true; g_gov.frame_start = tj0; g_gov.cimg_n = 0; g_gov.skip = gov_decide_skip(); }
                if (op_g == 0x3F && n >= 2) {   // SET_COLOR_IMAGE
                    g_cur_cimg = w[1] & 0xFFFFFF;
                    if (g_gov.cimg_n < 8) { bool seen = false; for (int k = 0; k < g_gov.cimg_n; k++) seen |= g_gov.cimgs[k] == g_cur_cimg; if (!seen) g_gov.cimgs[g_gov.cimg_n++] = g_cur_cimg; }
                }
                if ((op_g >= 0x08 && op_g <= 0x0F) || op_g == 0x24 || op_g == 0x25 || op_g == 0x36) {   // desenho: triangulos, retangulos de textura, retangulo de preenchimento
                    if (g_gov.skip) { g_gov_skipped_cmds++; i += n + 1; continue; }   // quadro pulado: estado e texturas passam, o desenho nao
                    dirty_add(g_cur_cimg);
                }
                g_proc->enqueue_command(n, w);
                g_cmds++;
                if (op_g == 0x29) {   // SYNC_FULL: esperar a GPU e so entao avisar o jogo
                    const double cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tj0).count();   // CPU so enfileirando os comandos deste quadro
                    if (cpu_ms > g_cpu_ms_max) g_cpu_ms_max = cpu_ms;
                    const double t0 = wdc_now_s();
                    if (g_fake_frame_ms > 0.0 && !g_gov.skip) std::this_thread::sleep_for(std::chrono::microseconds((long long)(g_fake_frame_ms * 1000.0)));   // teste: GPU lenta simulada
                    g_proc->wait_for_timeline(g_proc->signal_timeline());
                    const double dt = wdc_now_s() - t0;
                    const double gov_L_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_gov.frame_start).count();
                    g_sync_wait_total += dt;
                    g_enq_ms_total += cpu_ms;
                    {
                        const double busy = cpu_ms + dt * 1000.0;
                        g_busy_hist[busy < 8 ? 0 : busy < 16 ? 1 : busy < 24 ? 2 : busy < 30 ? 3 : busy < 36 ? 4 : busy < 50 ? 5 : 6]++;
                    }
                    if (dt > g_sync_wait_max) g_sync_wait_max = dt;
                    g_syncs++;
                    if (g_syncs.load() == 6000) save_pipeline_cache();   // uma vez no meio da sessao, para nao perder se o programa for encerrado a forca
                    {   // diagnostico de TRAVADAS: intervalo longo entre dois quadros (SYNC_FULL) fica registrado com o que se sabe na hora
                        static double last_sync_t = 0.0;
                        static int hitch_lines = 0;
                        const double now = wdc_now_s();
                        if (last_sync_t > 0.0 && g_syncs.load() > 300) {
                            const double gap = (now - last_sync_t) * 1000.0;
                            g_gap_hist[gap < 20 ? 0 : gap < 30 ? 1 : gap < 40 ? 2 : gap < 60 ? 3 : gap < 100 ? 4 : gap < 200 ? 5 : 6]++;
                            if (cpu_ms > 20.0) g_slow_cpu_frames++;
                            if ((gap > 60.0 || cpu_ms > 30.0) && hitch_lines < 120) {   // quadro longo (> 60 ms entre quadros) ou CPU do RDP lenta (> 30 ms so enfileirando)
                                hitch_lines++;
                                size_t ql = 0;
                                { std::lock_guard<std::mutex> lk(g_mutex); ql = g_queue.size(); }
                                std::printf("[hitch %7.2fs] %.0f ms entre dois quadros (CPU do RDP neste quadro: %.0f ms; espera da GPU: %.0f ms; fila do RDP: %zu; estado do jogo: %u; corrida: %u; janela em primeiro plano: %s)\n", now,
                                            gap, cpu_ms, dt * 1000.0, ql, rdram_word(0x923E4), rdram_word(0x7DF94), wdc_window_foreground() ? "sim" : "NAO");
                            }
                        }
                        last_sync_t = now;
                    }
                    wdc_post_dp();
                    gov_frame_end(gov_L_ms, dt * 1000.0, cpu_ms);
                    { static const bool tl = std::getenv("WDC_TRILOG") != nullptr; if (tl) { std::printf("[tri %7.3fs] triangulos=%u retangulos=%u texturas=%u\n", wdc_now_s(), g_tl_tri, g_tl_rect, g_tl_timg); g_tl_tri = g_tl_rect = g_tl_timg = 0; } }
                }
                i += n + 1;
            }
        } else {
            process_vi(j);
        }
    }
}
}  // namespace

bool wdc_rdp_vk_init(uint8_t* rdram, size_t rdram_size, int gpu_index) {
    Util::register_thread_index(0);   // a thread que cria o dispositivo e a primaria (indice 0); depois a thread do RDP assume o mesmo indice (nunca as duas ao mesmo tempo)
    if (!Vulkan::Context::init_loader(nullptr)) { std::printf("[rdp-vk] sem loader Vulkan (vulkan-1.dll)\n"); return false; }
    g_ctx.reset(new Vulkan::Context);
    if (!g_ctx->init_instance(nullptr, 0)) { std::printf("[rdp-vk] falha ao criar instancia Vulkan\n"); return false; }
    VkInstance inst = g_ctx->get_instance();
    auto gipa = Vulkan::Context::get_instance_proc_addr();
    auto enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    auto get_props = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    uint32_t n = 0;
    enumerate(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    enumerate(inst, &n, gpus.data());
    int pick = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p{};
        get_props(gpus[i], &p);
        std::printf("[rdp-vk] GPU %u: %s (tipo %d)\n", i, p.deviceName, (int)p.deviceType);
        if (pick < 0 && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pick = (int)i;
    }
    if (gpu_index >= 0 && (uint32_t)gpu_index < n) pick = gpu_index;
    if (pick < 0) pick = 0;
    if (n == 0) { std::printf("[rdp-vk] nenhuma GPU Vulkan\n"); return false; }
    std::printf("[rdp-vk] usando GPU %d\n", pick);
    {
        VkPhysicalDeviceProperties pp{};
        get_props(gpus[pick], &pp);
        wdc_cfg_resolve_auto_scale(pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);   // escala interna "auto": 4x em GPU dedicada, 2x em integrada
    }
    if (!g_ctx->init_device(gpus[pick], VK_NULL_HANDLE, nullptr, 0, 0)) { std::printf("[rdp-vk] falha ao criar dispositivo Vulkan\n"); return false; }
    g_dev.reset(new Vulkan::Device);
    g_dev->set_context(*g_ctx);
    load_pipeline_cache();
    // Resolucao interna (modo Enhanced: enhanced.internal_scale = 1|2|4|8; Legacy = 1; WDC_RDP_SCALE sobrescreve, ver wdc_config.cpp): upscaling do ParaLLEl-RDP.
    // Experimental: o jogo le o framebuffer de volta em alguns efeitos? (WDC_RDP_SSREADBACK liga a leitura super-amostrada.)
    RDP::CommandProcessorFlags rdp_flags = 0;
    {
        const int sc = wdc_cfg().internal_scale;
        rdp_flags = sc >= 8 ? RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_8X_BIT : sc >= 4 ? RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_4X_BIT
                  : sc >= 2 ? RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_2X_BIT : 0;
        if (rdp_flags && std::getenv("WDC_RDP_SSREADBACK")) rdp_flags |= RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_READ_BACK_BIT;
    }
    g_proc.reset(new RDP::CommandProcessor(*g_dev, rdram, 0, rdram_size, rdram_size / 8, rdp_flags));
    if (!g_proc->device_is_supported()) { std::printf("[rdp-vk] dispositivo nao suporta o ParaLLEl-RDP\n"); g_proc.reset(); return false; }
    g_rdram_ptr = rdram;
    g_gate_on = wdc_cfg().fps60 && std::getenv("WDC_GATE") != nullptr;   // portao de buffer: experimental, desligado por padrao
    // Enhanced (enhanced.sharp_textures): textura por ponto no lugar do filtro de 3 pontos do N64. As texturas do jogo sao minusculas (32x32/64x32) e ficam muito ampliadas;
    // o filtro de 3 pontos as espalha na diagonal, o que parece "motion blur" no cenario em movimento (parede, pista). Legacy nao muda. WDC_POINT_TEX=1 forca em qualquer modo (teste).
    RDP::g_wdc_point_sampling = wdc_cfg().sharp_textures || std::getenv("WDC_POINT_TEX") != nullptr;
    g_deglitch = wdc_cfg().fps60 && !std::getenv("WDC_NO_DEGLITCH");   // filtro de quadro intruso (so Enhanced a 60 Hz)
    g_rdram_size = rdram_size;
    g_order_on = wdc_cfg().fps60 && !std::getenv("WDC_NO_ORDER");
    g_gov.user_enabled = wdc_cfg().perf_auto && !wdc_cfg().fps60 && !std::getenv("WDC_NO_GOV");   // so no Legacy: no Enhanced (60 Hz so em corrida, 30 Hz nos menus) nao ha periodo unico, e quem quer qualidade e escala alta aceita o custo
    g_gov.period_ms = wdc_cfg().fps60 ? 1000.0 / 60.0 : 1000.0 / 30.0;
    g_active = true;
    g_thread = std::thread(thread_main);
    g_thread.detach();
    return true;
}

bool wdc_rdp_vk_active() { return g_active.load(); }

// Barreira de ORDEM entre o scanout e os comandos do quadro seguinte.
// O ultramodern, a cada tick de VI, (1) enfileira ScreenUpdate (que escaneia o buffer que ACABOU de ser liberado), (2) troca os registradores de VI e (3) manda a
// mensagem de retrace ao jogo, que passa a desenhar nesse mesmo buffer. O ScreenUpdate e consumido pela thread de graficos (prioridade normal) e vira um Vi job na fila do
// RDP, enquanto os comandos do quadro seguinte vem da thread de RSP (o ucode e executado la): se a thread de graficos atrasa, os comandos do jogo entram na fila ANTES do
// Vi job e o scanout mostra o quadro novo pela metade (preto/parcial). Com o RT64 isso nao ocorre porque ele renderiza as listas na propria thread de graficos, em ordem.
// Aqui: antes de enfileirar comandos, espera (ate 20 ms) que todo tick de VI ja concluido tenha virado Vi job. So espera se houver tick pendente RECENTE (no maximo 3).
// WDC_NO_VI_BARRIER=1 desliga (para comparar).
const bool g_vi_barrier_off = std::getenv("WDC_NO_VI_BARRIER") != nullptr;
std::atomic<uint64_t> g_barrier_waits{0}, g_barrier_timeouts{0};
void wdc_vi_barrier() {
    if (g_vi_barrier_off) return;
    const uint64_t ticks = wdc_vi_ticks();
    uint64_t done = wdc_vi_updates_done();
    if (done >= ticks || ticks - done > 3) return;
    g_barrier_waits++;
    const auto t0 = std::chrono::steady_clock::now();
    while ((done = wdc_vi_updates_done()) < ticks) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(20)) { g_barrier_timeouts++; return; }
        std::this_thread::yield();
    }
}

void wdc_rdp_vk_commands(const uint32_t* batch, size_t nwords) {
    if (!g_active.load() || nwords == 0) return;
    wdc_vi_barrier();
    Job j;
    j.kind = Job::Cmds;
    j.words.assign(batch, batch + nwords);
    {
        std::lock_guard<std::mutex> l(g_mutex);
        g_queue.push_back(std::move(j));
    }
    g_cv.notify_one();
}

void wdc_rdp_vk_vi(const uint32_t regs[14]) {
    if (!g_active.load()) return;
    Job j;
    j.kind = Job::Vi;
    j.t_us = wdc_present_now_us();
    std::memcpy(j.vi, regs, sizeof(j.vi));
    {
        std::lock_guard<std::mutex> l(g_mutex);
        g_queue.push_back(std::move(j));
    }
    g_cv.notify_one();
}

void wdc_rdp_vk_note_swap(uint32_t fb_addr) {   // chamado por osViSwapBuffer (hook): o buffer passa a aguardar o scanout
    if (!g_gate_on.load() || !g_active.load()) return;
    const uint32_t a = fb_addr & 0xFFFFFF;
    std::lock_guard<std::mutex> l(g_mutex);
    if (std::find(g_pending_scan.begin(), g_pending_scan.end(), a) == g_pending_scan.end()) g_pending_scan.push_back(a);
    while (g_pending_scan.size() > 3) g_pending_scan.erase(g_pending_scan.begin());
}

void wdc_rdp_vk_dump_fb(const char* path, uint32_t addr, uint32_t width, uint32_t height, uint32_t size_bits) {
    if (!g_active.load()) return;
    Job j;
    j.kind = Job::Fb;
    j.path = path; j.fb_addr = addr; j.fb_w = width; j.fb_h = height; j.fb_bits = size_bits;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        g_queue.push_back(std::move(j));
    }
    g_cv.notify_one();
}

void wdc_rdp_vk_set_present(void (*fn)(const uint8_t*, unsigned, unsigned, int64_t)) { g_present = fn; }
void wdc_rdp_vk_set_present_repeat(void (*fn)(int64_t)) { g_present_repeat = fn; }
void wdc_rdp_vk_request_shot(const char* path, double after_seconds) {
    g_shot_path = path;
    g_shot_after = after_seconds;
    g_shot_pending = true;
}
void wdc_rdp_vk_shot_sequence(int extra_shots, double interval) { g_shot_remaining = extra_shots; g_shot_interval = interval; }
bool wdc_rdp_vk_shot_done() { return g_shot_done.load(); }

void wdc_rdp_vk_print_stats() {
    if (!g_active.load()) return;
    save_pipeline_cache();
    std::printf("[rdp-vk] barreira de VI: %llu esperas, %llu estouros de 20 ms%s\n", (unsigned long long)g_barrier_waits.load(), (unsigned long long)g_barrier_timeouts.load(), g_vi_barrier_off ? " (DESLIGADA)" : "");
    if (g_deglitch) std::printf("[rdp-vk] quadros intrusos filtrados (reexibido o anterior): %llu\n", (unsigned long long)g_deglitched.load());
    if (g_gate_on.load()) std::printf("[rdp-vk] portao de buffer: %llu lotes retidos, %llu scanouts adiantados, %llu estouros de 30 ms\n", (unsigned long long)g_gate_holds.load(), (unsigned long long)g_gate_reorders.load(), (unsigned long long)g_gate_timeouts.load());
    if (g_flick_dir) std::printf("[flicker] quadros analisados: %llu; com salto > %.1f em relacao ao anterior: %llu; QUADROS INTRUSOS (A,X,A'): %llu; DOIS quadros (A,X,Y,B): %llu\n",
                                 (unsigned long long)g_flick_frames, g_flick_T, (unsigned long long)g_flick_big, (unsigned long long)g_flick_found, (unsigned long long)g_flick_found2);
    if (g_flick_dir) std::printf("[flicker] ticks seguidos no MESMO buffer (1 / 2 / 3 / 4 / 5 / 6+): %llu / %llu / %llu / %llu / %llu / %llu\n", (unsigned long long)g_origin_runs[0], (unsigned long long)g_origin_runs[1],
                                 (unsigned long long)g_origin_runs[2], (unsigned long long)g_origin_runs[3], (unsigned long long)g_origin_runs[4], (unsigned long long)g_origin_runs[5]);
    std::printf("[rdp-vk] intervalo entre quadros (ms): <20=%llu 20-30=%llu 30-40=%llu 40-60=%llu 60-100=%llu 100-200=%llu >=200=%llu; quadros com > 20 ms de CPU so enfileirando: %llu (maximo %.1f ms)\n",
                (unsigned long long)g_gap_hist[0], (unsigned long long)g_gap_hist[1], (unsigned long long)g_gap_hist[2], (unsigned long long)g_gap_hist[3], (unsigned long long)g_gap_hist[4],
                (unsigned long long)g_gap_hist[5], (unsigned long long)g_gap_hist[6], (unsigned long long)g_slow_cpu_frames, g_cpu_ms_max);
    std::printf("[rdp-vk] custo por quadro do RDP (CPU enfileirando + espera da GPU, ms): <8=%llu <16=%llu <24=%llu <30=%llu <36=%llu <50=%llu >=50=%llu; CPU enfileirando: media %.2f ms\n",
                (unsigned long long)g_busy_hist[0], (unsigned long long)g_busy_hist[1], (unsigned long long)g_busy_hist[2], (unsigned long long)g_busy_hist[3],
                (unsigned long long)g_busy_hist[4], (unsigned long long)g_busy_hist[5], (unsigned long long)g_busy_hist[6], g_syncs.load() ? g_enq_ms_total / (double)g_syncs.load() : 0.0);
    if (g_scanouts.load())
        std::printf("[rdp-vk] scanouts: %llu de %llu ticks de VI; scanout_sync: media %.2f ms, maximo %.2f ms; scanout completo (com entrega): media %.2f ms, maximo %.2f ms; candidatos a repeticao: %llu\n",
                    (unsigned long long)g_scanouts.load(), (unsigned long long)g_vi_ticks, g_scan_ms_total / (double)g_scanouts.load(), g_scan_ms_max,
                    g_vi_ms_total / (double)g_scanouts.load(), g_vi_ms_max, (unsigned long long)g_vi_dup_candidates);
    std::printf("[rdp-vk] repeticao de scanout: %llu reapresentados sem refazer (%llu candidatos)%s; ", (unsigned long long)g_scan_repeats, (unsigned long long)g_vi_dup_candidates, g_scan_repeat_enabled ? "" : " (DESLIGADA)");
    if (g_scan_verify) std::printf("verificacao: %llu iguais, %llu diferentes (diferenca maxima por canal %u; media de %.0f pixels por quadro), das quais %llu com diferenca > 2 (ruido de dither e +-1); ", (unsigned long long)g_scan_verify_ok,
                                   (unsigned long long)g_scan_verify_bad, g_verify_maxd, g_scan_verify_bad ? (double)g_verify_ndiff / (double)g_scan_verify_bad : 0.0, (unsigned long long)g_verify_big);
    std::printf("governador (%s): %llu quadros desenhados, %llu pulados (%llu comandos de desenho), menor fracao %.2f, custo %.1f ms + scanout %.1f ms; ticks de VI com buffer velho: %llu\n",
                g_gov.user_enabled ? "auto" : "desligado", (unsigned long long)g_gov.rendered, (unsigned long long)g_gov.skipped, (unsigned long long)g_gov_skipped_cmds, g_gov.min_frac,
                g_gov.cost_ema, g_gov.scan_ema, (unsigned long long)g_stale_ticks);
    if (g_order_on) std::printf("[rdp-vk] ordem de exibicao: %llu quadros repetidos para nao voltar no tempo (de %llu quadros completos)\n", (unsigned long long)g_back_repeats, (unsigned long long)g_frame_serial);
    std::printf("[rdp-vk] comandos enviados a GPU: %llu; SYNC_FULL: %llu (espera media %.2f ms, maxima %.2f ms); quadros: %llu; scanouts: %llu\n",
                (unsigned long long)g_cmds.load(), (unsigned long long)g_syncs.load(),
                g_syncs.load() ? 1000.0 * g_sync_wait_total / (double)g_syncs.load() : 0.0, 1000.0 * g_sync_wait_max,
                (unsigned long long)g_frames.load(), (unsigned long long)g_scanouts.load());
}
