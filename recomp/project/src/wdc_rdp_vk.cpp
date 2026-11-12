// Backend de RDP com ParaLLEl-RDP (Vulkan). Ver wdc_rdp_vk.h.
//
// Fluxo: a thread de RSP (wdc_rsp_hw.cpp, DPC_END) entrega comandos RDP completos -> fila -> thread de RDP -> CommandProcessor.
// SYNC_FULL: enqueue_command + wait_for_timeline(signal_timeline()) e so entao OS_EVENT_DP (a GPU terminou; o framebuffer/Z em RDRAM,
// que e o proprio RDRAM do jogo importado via VK_EXT_external_memory_host, esta coerente). Scanout: registradores de VI do runtime.
#include "wdc_rdp_vk.h"
#include "wdc_config.h"
#include "wdc_present.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
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
uint32_t g_vi_latest[14] = {};
uint8_t* g_rdram_ptr = nullptr;
bool g_have_vi = false;

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
uint64_t g_flick_frames = 0, g_flick_found = 0, g_flick_big = 0;

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
    if (g_ff[0].grid.empty() || g_ff[0].w != w || g_ff[1].w != w || g_ff[0].h != h || g_ff[1].h != h) return;
    const double d21 = grid_mad(g_ff[0].grid, g_ff[1].grid), d10 = grid_mad(g_ff[1].grid, g_ff[2].grid), d20 = grid_mad(g_ff[0].grid, g_ff[2].grid);
    if (d21 > g_flick_T) g_flick_big++;
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
    std::vector<RDP::RGBA> px;
    unsigned w = 0, h = 0;
    g_proc->scanout_sync(px, w, h, opts);
    g_scanouts++;
    if (want_shot && w && h) {
        write_png(shot_name().c_str(), px, w, h);
        g_shot_index++;
        if (g_shot_remaining > 0) { g_shot_remaining--; g_shot_after = wdc_now_s() + g_shot_interval; }
        else { g_shot_pending = false; g_shot_done = true; }
    }
    deliver(px, w, h, t_vi_us);   // por ultimo: pode consumir px
}

void process_vi(Job& j) {   // Vi: fim de quadro
    std::memcpy(g_vi_latest, j.vi, sizeof(g_vi_latest));
    g_have_vi = true;
    g_frames++;
    const bool shot = g_shot_pending.load() && wdc_now_s() >= g_shot_after && g_syncs.load() > 0;
    if (shot || g_present || g_flick_dir) do_scanout(shot, j.t_us);
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
            if (g_gate_on.load()) gate_wait(j);
            size_t i = 0;
            while (i < j.words.size()) {
                const unsigned n = j.words[i];
                const uint32_t* w = &j.words[i + 1];
                {
                    const uint32_t op = (w[0] >> 24) & 0x3F;
                    (void)op;
                }
                if (g_flick_dir && ((w[0] >> 24) & 0x3F) == 0x3F && n >= 2) {   // SET_COLOR_IMAGE
                    const uint32_t a = w[1] & 0xFFFFFF;
                    if (g_cimg_since.empty() || g_cimg_since.back() != a) g_cimg_since.push_back(a);
                    g_cimg_cur = a;
                }
                if (g_flick_dir) { const uint32_t op = (w[0] >> 24) & 0x3F; if ((op >= 0x08 && op <= 0x0F) || op == 0x24 || op == 0x25 || op == 0x36) g_ndraw_since++; }
                g_proc->enqueue_command(n, w);
                g_cmds++;
                if (((w[0] >> 24) & 0x3F) == 0x29) {   // SYNC_FULL: esperar a GPU e so entao avisar o jogo
                    const double t0 = wdc_now_s();
                    g_proc->wait_for_timeline(g_proc->signal_timeline());
                    const double dt = wdc_now_s() - t0;
                    g_sync_wait_total += dt;
                    if (dt > g_sync_wait_max) g_sync_wait_max = dt;
                    g_syncs++;
                    wdc_post_dp();
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
    g_deglitch = wdc_cfg().fps60 && !std::getenv("WDC_NO_DEGLITCH");   // filtro de quadro intruso (so Enhanced a 60 Hz)
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
void wdc_rdp_vk_request_shot(const char* path, double after_seconds) {
    g_shot_path = path;
    g_shot_after = after_seconds;
    g_shot_pending = true;
}
void wdc_rdp_vk_shot_sequence(int extra_shots, double interval) { g_shot_remaining = extra_shots; g_shot_interval = interval; }
bool wdc_rdp_vk_shot_done() { return g_shot_done.load(); }

void wdc_rdp_vk_print_stats() {
    if (!g_active.load()) return;
    std::printf("[rdp-vk] barreira de VI: %llu esperas, %llu estouros de 20 ms%s\n", (unsigned long long)g_barrier_waits.load(), (unsigned long long)g_barrier_timeouts.load(), g_vi_barrier_off ? " (DESLIGADA)" : "");
    if (g_deglitch) std::printf("[rdp-vk] quadros intrusos filtrados (reexibido o anterior): %llu\n", (unsigned long long)g_deglitched.load());
    if (g_gate_on.load()) std::printf("[rdp-vk] portao de buffer: %llu lotes retidos, %llu scanouts adiantados, %llu estouros de 30 ms\n", (unsigned long long)g_gate_holds.load(), (unsigned long long)g_gate_reorders.load(), (unsigned long long)g_gate_timeouts.load());
    if (g_flick_dir) std::printf("[flicker] quadros analisados: %llu; com salto > %.1f em relacao ao anterior: %llu; QUADROS INTRUSOS (A,X,A'): %llu\n",
                                 (unsigned long long)g_flick_frames, g_flick_T, (unsigned long long)g_flick_big, (unsigned long long)g_flick_found);
    std::printf("[rdp-vk] comandos enviados a GPU: %llu; SYNC_FULL: %llu (espera media %.2f ms, maxima %.2f ms); quadros: %llu; scanouts: %llu\n",
                (unsigned long long)g_cmds.load(), (unsigned long long)g_syncs.load(),
                g_syncs.load() ? 1000.0 * g_sync_wait_total / (double)g_syncs.load() : 0.0, 1000.0 * g_sync_wait_max,
                (unsigned long long)g_frames.load(), (unsigned long long)g_scanouts.load());
}
