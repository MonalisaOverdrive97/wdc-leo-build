// Projeto minimo que linka o C gerado do World Driver Championship com o N64ModernRuntime.
// Etapa 1: renderizador NULO (so registra as tarefas graficas), audio e entrada simulados.
// Objetivo: provar que a recompilacao da CPU + libultra linka e roda, sem depender de grafico.
//
// Uso:  wdc_recomp.exe [--rom <z64>] [--config <pasta>] [--seconds N] [--autopress]

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <deque>
#include <xmmintrin.h>
#include <io.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <fstream>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "wdc_rdp_vk.h"
#include "wdc_audio.h"
#include "wdc_pak.h"
#include "wdc_config.h"
#include "wdc_setup.h"
#include "wdc_rom.h"
#include "wdc_version.h"
#include "wdc_input.h"
#include "wdc_window.h"

// Tabelas geradas pelo N64Recomp (secoes, funcoes). Define section_table, num_sections, overlay_sections_by_index.
#include "recomp_overlays.inl"

// lookup.cpp (gerado)
gpr get_entrypoint_address();

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// Hash XXH3-64 do wdc_bigendian.z64 (calculado com o xxhash.h do proprio runtime).
static constexpr uint64_t kWdcRomHash = 0x1BF1E67DCFA21FB8ull;

static std::atomic<uint64_t> g_vi_count{0};
static std::atomic<uint64_t> g_task_count{0};
static std::atomic<uint64_t> g_frame_count{0};
static std::atomic<uint64_t> g_updates_done{0};   // update_screen() concluidos (Vi job ja na fila do RDP); ver wdc_vi_barrier em wdc_rdp_vk.cpp
static bool g_autopress = false;
static std::string g_dump_every_dir; static double g_dump_every_t = -1.0, g_dump_every_dt = 0.0; static int g_dump_every_left = 0, g_dump_every_idx = 0;   // --dump-ram-every pasta inicio intervalo quantidade
static std::string g_dump_ram_path; static double g_dump_ram_t = -1.0;   // --dump-ram arquivo segundos: grava a RDRAM (4 MB, palavras em ordem do host) no instante dado
static bool g_exit_after_script = false;   // --exit-after-script: encerra 3 s (relogio ativo) depois do fim do ultimo evento nao periodico do roteiro
static double g_script_end = 0.0;
static bool g_task_clock = false;   // --task-clock: roteiro, janelas de gravacao e capturas contam TAREFAS graficas (30/s) em vez de relogio de parede (reprodutivel sob carga)
static const Clock::time_point g_t0 = Clock::now();

static double now_s() { return std::chrono::duration<double>(Clock::now() - g_t0).count(); }
extern "C" uint64_t wdc_gfx_tasks();   // wdc_rsp_hw.cpp
double wdc_now_s() { return g_task_clock ? (double)std::max<uint64_t>(g_task_count.load(), wdc_gfx_tasks()) / 30.0 : now_s(); }   // usado por trace_hooks.cpp, trace_seg.cpp, wdc_rdp_vk.cpp e pelo roteiro de entrada
static uint8_t* g_rdram = nullptr;
static fs::path g_config_dir;
extern "C" int g_wdc_enh_fps60;   // wdc_runtime_stubs.cpp: chave do modo Enhanced (60 fps)
uint8_t* wdc_get_rdram() { return g_rdram; }
#ifdef WDC_TRACE_BUILD
extern "C" void wdc_dump_blocked(void);
#else
extern "C" void wdc_blk_dump(void);   // wdc_runtime_stubs.cpp: tabela de quem esta em osRecvMesg/osSendMesg (diagnostico de travamento)
#endif

// ---------------------------------------------------------------------------------------------
// Mapa de cobertura (so no build com trace_mode): cada funcao registra um contador na 1a chamada.
// ---------------------------------------------------------------------------------------------
static std::mutex g_cov_mutex;
static std::unordered_map<std::string, uint32_t*> g_cov_map;
static std::vector<std::pair<std::string, uint32_t*>> g_cov_order;   // ordem da 1a chamada
static std::deque<uint32_t> g_cov_storage;                           // enderecos estaveis
static std::vector<double> g_cov_first_t;                            // relogio ativo (wdc_now_s) na 1a chamada, paralelo a g_cov_order

extern "C" uint32_t* wdc_cov_register(const char* name) {
    std::lock_guard<std::mutex> l(g_cov_mutex);
    auto it = g_cov_map.find(name);
    if (it != g_cov_map.end()) return it->second;
    g_cov_storage.push_back(0);
    uint32_t* p = &g_cov_storage.back();
    g_cov_map.emplace(name, p);
    g_cov_order.emplace_back(name, p);
    g_cov_first_t.push_back(wdc_now_s());
    return p;
}

bool wdc_post_sp_break();   // wdc_runtime_stubs.cpp
void wdc_rsp_print_stats(); // wdc_rsp_hw.cpp
extern bool g_lle;          // wdc_runtime_stubs.cpp (true = LLE: ucode recompilado; --null-rsp = false)
bool wdc_sig0_wait_and_clear(int timeout_ms);   // wdc_runtime_stubs.cpp
static uint64_t g_dump_from = 0;        // --dump-from N: imprime a lista completa das tarefas N..N+7
static std::atomic<bool> g_stop{false}; // o watchdog ja pediu o encerramento
static bool g_rdp_vk = true;            // --no-rdp-vk desliga o ParaLLEl-RDP (volta ao consumidor de RDP nulo)
static int g_rdp_gpu = -1;              // --rdp-gpu N: indice da GPU Vulkan (padrao: primeira discreta)

// Grava os contadores de cobertura em <config>/cov_<tag>.txt (usado no fim normal, no snapshot e na queda).
void wdc_write_cov(const char* tag) {
    std::lock_guard<std::mutex> l(g_cov_mutex);
    if (g_cov_order.empty() || g_config_dir.empty()) return;
    std::ofstream f(g_config_dir / (std::string("cov_") + tag + ".txt"));
    for (const auto& [n, c] : g_cov_order) f << *c << " " << n << "\n";
}

// ---------------------------------------------------------------------------------------------
// Renderizador nulo: nao desenha nada, so registra o que o jogo manda para a RSP.
// ---------------------------------------------------------------------------------------------
class NullRenderer final : public ultramodern::renderer::RendererContext {
public:
    explicit NullRenderer(uint8_t* rdram) : rdram_(rdram) {
        setup_result = ultramodern::renderer::SetupResult::Success;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;
    }
    bool valid() override { return true; }
    bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override { return true; }
    void enable_instant_present() override {}
    void send_dummy_workload(uint32_t) override {}
    void shutdown() override {}
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1.0f; }
    void update_screen() override {
        g_frame_count++;
        const ultramodern::renderer::ViRegs* v = ultramodern::renderer::get_vi_regs();   // registradores de VI deste quadro
        const uint32_t regs[14] = { v->VI_STATUS_REG, v->VI_ORIGIN_REG, v->VI_WIDTH_REG, v->VI_INTR_REG, v->VI_V_CURRENT_LINE_REG, v->VI_TIMING_REG,
                                    v->VI_V_SYNC_REG, v->VI_H_SYNC_REG, v->VI_LEAP_REG, v->VI_H_START_REG, v->VI_V_START_REG, v->VI_V_BURST_REG,
                                    v->VI_X_SCALE_REG, v->VI_Y_SCALE_REG };
        wdc_rdp_vk_vi(regs);
        g_updates_done++;
    }

    void send_dl(const OSTask* task) override {
        uint8_t* rdram = rdram_;                       // usado por TO_PTR
        const OSTask_s& t = task->t;
        const uint64_t n = ++g_task_count;
        const bool verbose = n <= 3;
        if (verbose || n % 300 == 0) {
            std::printf("[gfx %7.2fs] tarefa #%" PRIu64 " tipo=%u flags=0x%X boot=0x%08X/0x%X ucode=0x%08X/0x%X data=0x%08X/0x%X "
                        "out=0x%08X/0x%X dl=0x%08X/0x%X yield=0x%08X\n",
                        now_s(), n, t.type, t.flags, (uint32_t)t.ucode_boot, t.ucode_boot_size, (uint32_t)t.ucode, t.ucode_size,
                        (uint32_t)t.ucode_data, t.ucode_data_size, (uint32_t)t.output_buff, (uint32_t)t.output_buff_size,
                        (uint32_t)t.data_ptr, t.data_size, (uint32_t)t.yield_data_ptr);
        }
        if (t.type != M_GFXTASK || t.data_ptr == 0 || t.data_size == 0 || t.data_size > 0x100000) {
            return;
        }
        // Fluxo A: data_size/8 comandos. Fluxo B: a partir de yield_data_ptr, ate o opcode 1A (volta ao A).
        const uint32_t count = t.data_size / 8;
        if (g_dump_from && n >= g_dump_from && n < g_dump_from + 8) {   // --dump-from N: lista completa de 8 tarefas
            std::printf("[dump] tarefa #%" PRIu64 " t=%.2fs flags=0x%X data=0x%08X/0x%X yield=0x%08X/0x%X out=0x%08X/0x%X\n", n, now_s(), t.flags,
                        (uint32_t)t.data_ptr, t.data_size, (uint32_t)t.yield_data_ptr, t.yield_data_size, (uint32_t)t.output_buff, (uint32_t)t.output_buff_size);
            auto dump_list = [&](const char* nome, int32_t base, uint32_t max, bool until_1a) {
                std::printf("[dump]   %s @0x%08X:\n", nome, (uint32_t)base);
                for (uint32_t i = 0; i < max; i++) {
                    const uint32_t w0 = *TO_PTR(uint32_t, base + (int32_t)(8 * i)), w1 = *TO_PTR(uint32_t, base + (int32_t)(8 * i + 4));
                    std::printf("[dump]     %3u: %08X %08X\n", i, w0, w1);
                    if (until_1a && (w0 >> 24) == 0x1A) break;
                }
            };
            dump_list("A", t.data_ptr, std::min(count, 120u), false);
            if (t.yield_data_ptr) dump_list("B", t.yield_data_ptr, 200, true);
        }
        std::map<uint32_t, uint64_t> this_task;
        run_ucode(t, n, count, this_task);

        // Detector de cena: comandos que so o 3D usa (matrizes, vertices, triangulos, luz, normais...).
        static const uint32_t k3d[] = { 0x08, 0x0A, 0x0C, 0x10, 0x14, 0x16, 0x18, 0x1C, 0x22, 0x24 };
        uint64_t n3d = 0;
        for (uint32_t op : k3d) { auto it = this_task.find(op); if (it != this_task.end()) n3d += it->second; }
        const bool is3d = n3d > 0;
        if (is3d) { total_3d_cmds_ += n3d; tasks_3d_++; }
        if (is3d != scene_3d_) {
            scene_3d_ = is3d;
            std::printf("[cena %7.2fs] tarefa #%" PRIu64 ": %s (%" PRIu64 " comandos 3D nesta tarefa)\n", now_s(), n, is3d ? "3D" : "2D", n3d);
            if (is3d && scene_dumps_ < 3) {
                scene_dumps_++;
                std::printf("    fluxo A (%u comandos) opcodes:", count);
                for (auto& [op, c] : this_task) std::printf(" %02X x%" PRIu64, op, c);
                std::printf("\n    primeiros 16 comandos:");
                for (uint32_t i = 0; i < std::min(count, 16u); i++) {
                    std::printf(" [%08X %08X]", *TO_PTR(uint32_t, t.data_ptr + (int32_t)(8 * i)), *TO_PTR(uint32_t, t.data_ptr + (int32_t)(8 * i + 4)));
                }
                std::printf("\n");
            }
        }
        if (verbose) {
            std::printf("    fluxo A: %u comandos; primeiros:", count);
            for (uint32_t i = 0; i < std::min(count, 6u); i++) {
                std::printf(" [%08X %08X]", *TO_PTR(uint32_t, t.data_ptr + (int32_t)(8 * i)), *TO_PTR(uint32_t, t.data_ptr + (int32_t)(8 * i + 4)));
            }
            std::printf("\n    fluxo B (yield_data_ptr): primeiros:");
            for (uint32_t i = 0; i < 6; i++) {
                std::printf(" [%08X %08X]", *TO_PTR(uint32_t, t.yield_data_ptr + (int32_t)(8 * i)), *TO_PTR(uint32_t, t.yield_data_ptr + (int32_t)(8 * i + 4)));
            }
            std::printf("\n");
        }
    }

    void print_summary() const {
        auto dump = [](const char* nome, const std::map<uint32_t, uint64_t>& m) {
            std::printf("[gfx] %s:", nome);
            for (auto& [op, c] : m) std::printf(" %02X x%" PRIu64, op, c);
            std::printf("\n");
        };
        std::printf("[gfx] resumo: %" PRIu64 " tarefas, %" PRIu64 " atualizacoes de tela\n", g_task_count.load(), g_frame_count.load());
        dump("opcodes fluxo A     ", a_ops_);
        dump("opcodes fluxo B     ", b_ops_);
        dump("opcodes listas 0E   ", nested_ops_);
        std::printf("[gfx] anomalias: principal fora de {00,02,04,...,26} = %" PRIu64 "; aninhada fora de [C9..FF] = %" PRIu64 "\n", anomalies_main_, anomalies_nested_);
        std::printf("[gfx] tarefas com comandos 3D: %" PRIu64 " (%" PRIu64 " comandos 3D no total)\n", tasks_3d_, total_3d_cmds_);
        std::printf("[gfx] eventos SP_BREAK (comando 0x12): %" PRIu64 " entregues, %" PRIu64 " perdidos (jogo sem registro)\n", sp_breaks_posted_, sp_breaks_lost_);
        std::printf("[gfx] fluxo A com data_size != comandos ate o 02: %" PRIu64 "; fluxos sem terminador: %" PRIu64 "; esperas de SIG0 > 1 s: %" PRIu64 "\n",
                    a_len_mismatch_, runaway_, sig0_timeouts_);
    }

private:
    // Modelo do FLUXO DE CONTROLE do ucode (recomp/rsp/wdc_ucode_annotated.s). O ucode nao usa data_size: executa o fluxo A
    // a partir de data_ptr ate o comando 02; o 02 ESPERA o SIG0 (o "kick" da CPU por osSpTaskYield: a lista de calculo,
    // fluxo B, esta pronta), limpa-o e passa ao fluxo B (yield_data_ptr), que roda ate o 1A (fim da tarefa: SET_SIG2 +
    // break). Os comandos 12 (sync) levantam a interrupcao do SP => OS_EVENT_SP_BREAK para o jogo.
    void run_ucode(const OSTask_s& t, uint64_t n, uint32_t count, std::map<uint32_t, uint64_t>& per_task) {
        uint8_t* rdram = rdram_;
        int32_t pos = t.data_ptr;
        bool in_b = false;
        uint32_t executed = 0, executed_a = 0;
        const bool dump_b = g_dump_from && n >= g_dump_from && n < g_dump_from + 8;
        while (true) {
            if (++executed > 0x20000) {
                runaway_++;
                std::printf("[gfx] tarefa #%" PRIu64 ": fluxo %c sem terminador apos %u comandos\n", n, in_b ? 'B' : 'A', executed);
                return;
            }
            const uint32_t w0 = *TO_PTR(uint32_t, pos);
            const uint32_t w1 = *TO_PTR(uint32_t, pos + 4);
            pos += 8;
            const uint32_t op = w0 >> 24;
            (in_b ? b_ops_ : a_ops_)[op]++;
            per_task[op]++;
            if (!in_b) executed_a++;
            const bool ok = op < 0x28 && (op >= 0x20 ? (op == 0x22 || op == 0x24 || op == 0x26) : (op % 2 == 0));
            if (!ok && op != 0x00) anomalies_main_++;
            switch (op) {
            case 0x12:   // sync CPU<-RSP
                if (wdc_post_sp_break()) sp_breaks_posted_++; else sp_breaks_lost_++;
                break;
            case 0x0E: { // lista RDP aninhada
                int32_t p = (int32_t)(0x80000000u | (w1 & 0xFFFFFF));
                for (int k = 0; k < 0x4000; k++) {
                    const uint32_t nop = *TO_PTR(uint32_t, p + (int32_t)(8 * k)) >> 24;
                    nested_ops_[nop]++;
                    if (nop < 0xC9) anomalies_nested_++;
                    if (nop == 0xDF) break;
                }
                break;
            }
            case 0x02:   // ir ao fluxo B (espera o kick)
                if (in_b) { anomalies_main_++; break; }
                if (executed_a != count) {
                    a_len_mismatch_++;
                    if (a_len_mismatch_ <= 5) std::printf("[gfx] tarefa #%" PRIu64 ": fluxo A terminou em 02 apos %u comandos, data_size/8 = %u\n", n, executed_a, count);
                }
                for (int waited = 1; !wdc_sig0_wait_and_clear(1000); waited++) {
                    if (g_stop) return;
                    if (waited == 3 || waited % 10 == 0) std::printf("[gfx %7.2fs] tarefa #%" PRIu64 ": aguardando SIG0 (osSpTaskYield) ha %d s\n", now_s(), n, waited);
                    sig0_timeouts_++;
                }
                in_b = true;
                pos = t.yield_data_ptr;
                if (dump_b) {
                    std::printf("[dump]   B @kick 0x%08X:\n", (uint32_t)pos);
                    for (uint32_t i = 0; i < 400; i++) {
                        const uint32_t b0 = *TO_PTR(uint32_t, pos + (int32_t)(8 * i)), b1 = *TO_PTR(uint32_t, pos + (int32_t)(8 * i + 4));
                        std::printf("[dump]     %3u: %08X %08X\n", i, b0, b1);
                        if ((b0 >> 24) == 0x1A) break;
                    }
                }
                break;
            case 0x1A:   // fim do fluxo B => tarefa concluida (no A seria anomalia)
                if (!in_b) anomalies_main_++;
                return;
            default:
                break;
            }
        }
    }

    uint8_t* rdram_;
    std::map<uint32_t, uint64_t> a_ops_, b_ops_, nested_ops_;
    uint64_t anomalies_main_ = 0, anomalies_nested_ = 0;
    uint64_t sp_breaks_posted_ = 0, sp_breaks_lost_ = 0, a_len_mismatch_ = 0, runaway_ = 0, sig0_timeouts_ = 0;
    bool scene_3d_ = false;
    int scene_dumps_ = 0;
    uint64_t total_3d_cmds_ = 0, tasks_3d_ = 0;
public:
    uint64_t tasks_3d() const { return tasks_3d_; }
};

static NullRenderer* g_renderer = nullptr;
bool wdc_is_stopping() { return g_stop.load(); }
void wdc_print_gfx_summary() { if (g_renderer) g_renderer->print_summary(); if (g_lle) wdc_rsp_print_stats(); }   // chamado pelo tratador de queda (trace_hooks.cpp)

static std::unique_ptr<ultramodern::renderer::RendererContext> create_null_renderer(uint8_t* rdram, ultramodern::renderer::WindowHandle, bool) {
    auto r = std::make_unique<NullRenderer>(rdram);
    g_renderer = r.get();
    return r;
}

// Audio: wdc_audio.{h,cpp} (WASAPI + captura WAV; modelo nulo sem dispositivo).

// ---------------------------------------------------------------------------------------------
// Entrada: um controle na porta 0. Com --autopress, pulsa Start e A para sair das telas iniciais.
// ---------------------------------------------------------------------------------------------
// Roteiro de entrada: "inicio:botoes:duracao[:periodo];..." (segundos). Botoes: A B Z S(tart) U D L R (d-pad),
// l r (gatilhos L/R), e C-botoes: w(up) s(down) a(left) d(right). Com periodo > 0 o evento se repete.
// Campos opcionais 5 e 6: analogico x e y em [-1,1] (y positivo = para cima), ex. "40:A:5:0:-0.6:0" = A + volante a 60% para a esquerda por 5 s.
// Campo opcional 7: porta do controle (0..3; padrao 0), para o modo de 2 jogadores (--players 2).
struct ScriptEvent { double start, dur, period; uint16_t buttons; float ax, ay; int port; };
static int g_players = 1;             // --players N: controles N64 conectados (1..4); so a porta 0 tem Controller Pak
static std::vector<ScriptEvent> g_script;
// --poke "T:ENDERECO:VALOR[;...]" (hex, palavra de 32 bits): grava na RDRAM do jogo no 1o ciclo de entrada em que o relogio ativo >= T (aplicado na leitura do controle
// 0, ou seja, preso aos quadros do jogo). Serve para ATALHOS DE TESTE que o roteiro nao alcanca (ex.: forcar o fim de uma prova).
struct Poke { double t; uint32_t addr, val; bool done; };
static std::vector<Poke> g_pokes;
static void parse_pokes(const std::string& spec) {
    size_t i = 0;
    while (i < spec.size()) {
        size_t e = spec.find(';', i);
        if (e == std::string::npos) e = spec.size();
        unsigned a = 0, v = 0; double t = 0;
        if (std::sscanf(spec.substr(i, e - i).c_str(), "%lf:%x:%x", &t, &a, &v) == 3) g_pokes.push_back({t, a, v, false});
        i = e + 1;
    }
}
static std::atomic<uint16_t> g_last_buttons{0};
static bool g_play = false;          // --play: janela + teclado/controle reais

static uint16_t parse_buttons(const std::string& s) {
    uint16_t b = 0;
    for (char c : s) {
        switch (c) {
        case 'A': b |= 0x8000; break;  case 'B': b |= 0x4000; break;  case 'Z': b |= 0x2000; break;
        case 'S': b |= 0x1000; break;  case 'U': b |= 0x0800; break;  case 'D': b |= 0x0400; break;
        case 'L': b |= 0x0200; break;  case 'R': b |= 0x0100; break;  case 'l': b |= 0x0020; break;
        case 'r': b |= 0x0010; break;  case 'w': b |= 0x0008; break;  case 's': b |= 0x0004; break;
        case 'a': b |= 0x0002; break;  case 'd': b |= 0x0001; break;
        default: break;
        }
    }
    return b;
}
static void parse_script(const std::string& spec) {
    size_t i = 0;
    while (i < spec.size()) {
        size_t e = spec.find(';', i);
        if (e == std::string::npos) e = spec.size();
        std::string item = spec.substr(i, e - i);
        i = e + 1;
        std::vector<std::string> f;
        size_t p = 0;
        while (true) {
            size_t q = item.find(':', p);
            f.push_back(item.substr(p, q == std::string::npos ? std::string::npos : q - p));
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (f.size() < 3) continue;
        g_script.push_back({ std::atof(f[0].c_str()), std::atof(f[2].c_str()), f.size() > 3 ? std::atof(f[3].c_str()) : 0.0, parse_buttons(f[1]),
                             f.size() > 4 ? (float)std::atof(f[4].c_str()) : 0.0f, f.size() > 5 ? (float)std::atof(f[5].c_str()) : 0.0f,
                             f.size() > 6 ? std::atoi(f[6].c_str()) : 0 });
        if (g_script.back().period <= 0.0) g_script_end = std::max(g_script_end, g_script.back().start + g_script.back().dur);
    }
}

static void input_poll() {}
static bool input_get(int num, uint16_t* buttons, float* x, float* y) {
    if (num < 0 || num >= g_players) return false;
    const double t = wdc_now_s();
    if (num == 0 && g_rdram) {
        for (auto& pk : g_pokes) {
            if (!pk.done && t >= pk.t) {
                pk.done = true;
                *(uint32_t*)(g_rdram + (pk.addr & 0x1FFFFFFF)) = pk.val;   // palavra do guest guardada em ordem do host
                std::printf("[poke %7.2fs] 0x%08X <- 0x%08X\n", t, pk.addr, pk.val);
            }
        }
    }
    uint16_t b = 0;
    float ax = 0.0f, ay = 0.0f;
    for (const auto& ev : g_script) {
        if (ev.port != num || t < ev.start) continue;
        const bool on = ev.period > 0 ? std::fmod(t - ev.start, ev.period) < ev.dur : t < ev.start + ev.dur;
        if (on) { b |= ev.buttons; ax += ev.ax; ay += ev.ay; }
    }
    ax = std::max(-1.0f, std::min(1.0f, ax));
    ay = std::max(-1.0f, std::min(1.0f, ay));
    if (g_autopress && num == 0) {
        const double ph = std::fmod(t, 3.0);
        if (ph < 0.15) b |= 0x1000;            // Start
        else if (ph > 1.5 && ph < 1.65) b |= 0x8000;   // A
    }
    if (g_play && num > 0) {   // jogadores 2..4: so o controle XInput de mesmo indice (teclado e do jogador 1)
        const WdcInput r = wdc_input_read_pad((unsigned)num);
        b |= r.buttons;
        if (r.x != 0.0f || r.y != 0.0f) { ax = r.x; ay = r.y; }
        static const bool log_inp = std::getenv("WDC_INPUTLOG") != nullptr;   // mesmo log do jogador 1, com o numero do jogador: [input2 ...]
        static uint16_t lb[4]; static float lxp[4], lyp[4];
        if (log_inp && (r.buttons != lb[num] || std::fabs(r.x - lxp[num]) > 0.25f || std::fabs(r.y - lyp[num]) > 0.25f)) {
            std::printf("[input%d %7.2fs] botoes=%04X analogico=(%.2f, %.2f)\n", num + 1, wdc_now_s(), r.buttons, r.x, r.y);
            lb[num] = r.buttons; lxp[num] = r.x; lyp[num] = r.y;
        }
    }
    if (g_play && num == 0) {   // teclado (janela) e controle XInput
        const WdcInput r = wdc_input_read();
        b |= r.buttons;
        if (r.x != 0.0f || r.y != 0.0f) { ax = r.x; ay = r.y; }
        static const bool log_in = std::getenv("WDC_INPUTLOG") != nullptr;   // WDC_INPUTLOG=1: loga cada mudanca do estado do controle
        static uint16_t last_b = 0; static float last_x = 0, last_y = 0;
        if (log_in && (r.buttons != last_b || std::fabs(ax - last_x) > 0.25f || std::fabs(ay - last_y) > 0.25f)) {
            std::printf("[input %7.2fs] botoes=%04X analogico=(%.2f, %.2f)%s\n", wdc_now_s(), r.buttons, ax, ay, r.pad_connected ? " [controle]" : "");
            last_b = r.buttons; last_x = ax; last_y = ay;
        }
    }
    if (num == 0) g_last_buttons = b;
    *buttons = b; *x = ax; *y = ay;
    return true;
}
static void input_rumble(int, bool) {}
static ultramodern::input::connected_device_info_t input_info(int num) {
    if (num == 0) return { ultramodern::input::Device::Controller, wdc_pak_enabled() ? (ultramodern::input::Pak)2 /* Controller Pak (o enum do runtime so declara o Rumble Pak) */ : ultramodern::input::Pak::None };
    if (num > 0 && num < g_players) return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

// ---------------------------------------------------------------------------------------------
// Demais callbacks
// ---------------------------------------------------------------------------------------------
RspExitReason wdc_ucode_entry(uint8_t* rdram, uint32_t ucode_addr);   // wdc_rsp_hw.cpp (ucode recompilado pelo RSPRecomp)
static RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    // Modo LLE: o hook de osSpTaskStartGo troca o tipo da tarefa grafica (1 -> 0x1001) para que o runtime a mande para cá.
    if (g_lle && (task->t.type & 0xFFF) == M_GFXTASK) return wdc_ucode_entry;
    std::fprintf(stderr, "[rsp] tarefa nao grafica inesperada: tipo %u\n", task->t.type);
    return nullptr;
}
static void message_box(const char* msg) { std::fprintf(stderr, "[wdc] %s\n", msg); }
static void vi_callback() { g_vi_count++; }
uint64_t wdc_vi_ticks() { return g_vi_count.load(); }                 // ticks de VI vistos pela thread de VI (incrementado no FIM de cada tick)
uint64_t wdc_vi_updates_done() { return g_updates_done.load(); }      // quantos desses ticks ja viraram um Vi job na fila do RDP
static void* create_gfx() { return nullptr; }
static ultramodern::renderer::WindowHandle create_window(void*) {
#ifdef _WIN32
    return ultramodern::renderer::WindowHandle{ nullptr, 0 };   // thread_id != -1 (valor padrao), logo "ja existe"
#else
    return nullptr;
#endif
}
static void update_gfx(void*) {}

// O IPL3 real deixa osMemSize em 4 MB (WDC nao usa Expansion Pak); o runtime grava 8 MB em init().
static std::vector<uint32_t> g_watch;   // --watch: enderecos KSEG0 de palavras a observar
static int g_cov_snap_at = -1;          // --covsnap: segundo em que gravar cov_snap.txt

// A libultra liga o bit FS (flush-to-zero) do FCSR em todas as threads: no VR4300 um resultado denormal vira 0. O C gerado roda em SSE, que por padrao
// NAO descarta denormais (0x00000001 em vez de 0.0f). FTZ|DAZ no MXCSR da thread reproduz o hardware; o MXCSR e POR THREAD e as threads novas
// comecam com o padrao (0x1F80), entao cada thread do jogo liga o seu: a de entrada em on_init e as de osCreateThread pelo callback de nome.
static void wdc_fp_enable_flush() { _mm_setcsr(_mm_getcsr() | 0x8040); }
static std::string wdc_game_thread_name(const OSThread* t) {
    wdc_fp_enable_flush();   // chamado pelo ultramodern na propria thread nova, antes de ela executar codigo do jogo
    return "wdc thread " + std::to_string(t->id);
}

#ifdef _WIN32
// Diagnostico de quedas nativas (acesso invalido, estouro de pilha, corrupcao de heap, fastfail): imprime codigo, thread e a pilha como modulo+RVA
// em stderr ANTES de o Windows encerrar o processo. Silencioso se nada acontece; o RVA se resolve com o PDB (build trace) via llvm-symbolizer.
static LONG CALLBACK wdc_crash_log(PEXCEPTION_POINTERS ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != 0xC0000005 && code != 0xC0000409 && code != 0xC00000FD && code != 0xC0000374 && code != 0xC0000094) return EXCEPTION_CONTINUE_SEARCH;
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;
    std::fprintf(stderr, "\n[CRASH-NATIVO] codigo=0x%08lX endereco=%p thread=%lu (%s)\n", code, ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId(),
                 code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2 ? (ep->ExceptionRecord->ExceptionInformation[0] ? "escrita" : "leitura") : "-");
    void* frames[32];
    const USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m)) GetModuleFileNameA(m, name, MAX_PATH);
        const char* base = std::strrchr(name, '\\');
        std::fprintf(stderr, "[CRASH-NATIVO]   #%u %s+0x%llX\n", (unsigned)i, base ? base + 1 : name, (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)m));
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static void on_init(uint8_t* rdram, recomp_context*) {
    wdc_fp_enable_flush();
    g_rdram = rdram;
    constexpr int32_t osMemSize = 0x80000318;
    MEM_W(osMemSize, 0) = 4 * 1024 * 1024;
    std::printf("[init %7.2fs] osMemSize = 4 MB\n", now_s());
    // ParaLLEl-RDP: importa o RDRAM do jogo (8 MB, base alinhada a 64 KiB pela VirtualAlloc do runtime) como buffer da GPU.
    if (g_lle && g_rdp_vk) {
        if (wdc_rdp_vk_init(rdram, 8 * 1024 * 1024, g_rdp_gpu)) std::printf("[init %7.2fs] ParaLLEl-RDP (Vulkan) ativo\n", now_s());
        else std::printf("[init %7.2fs] ParaLLEl-RDP indisponivel: RDP nulo\n", now_s());
    }
}

static void register_wdc_overlays() {
    recomp::overlays::overlay_section_table_data_t sections{
        .code_sections = section_table,
        .num_code_sections = num_sections,
        .total_num_sections = num_sections,
    };
    recomp::overlays::overlays_by_index_t overlays{
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };
    recomp::overlays::register_overlays(sections, overlays);
}

// Pasta do executavel (GetModuleFileName: argv[0] pode ser so o nome quando iniciado pelo PATH).
static fs::path wdc_exe_dir() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return n ? fs::path(buf).parent_path() : fs::current_path();
}

// Config, ROM convertida e saves ficam em <pasta do exe>/wdc_config (instalacao portatil: apagar a pasta apaga tudo). Se a pasta nao for gravavel
// (ex.: o jogo foi colocado em "Arquivos de Programas"), usa %LOCALAPPDATA%/WorldDriverChampionship.
static fs::path wdc_pick_config_dir(const fs::path& exe_dir) {
    auto writable = [](const fs::path& d) {
        std::error_code ec;
        fs::create_directories(d, ec);
        const fs::path t = d / ".write_test";
        std::ofstream f(t);
        const bool ok = (bool)f;
        f.close();
        fs::remove(t, ec);
        return ok;
    };
    const fs::path local = exe_dir / "wdc_config";
    if (writable(local)) return local;
    if (const char* la = std::getenv("LOCALAPPDATA")) {
        const fs::path alt = fs::path(la) / "WorldDriverChampionship";
        if (writable(alt)) return alt;
    }
    return local;
}

// Acha a ROM e a valida (librecomp: aceita .z64/.v64/.n64, converte e guarda <config>/wdc.z64). Ordem: --rom > copia ja guardada > ao lado do exe (wdc.z64, ou o
// .z64/.v64/.n64 da pasta ou de rom/) > caminho de desenvolvimento. Sem ROM valida, no modo interativo abre o seletor de arquivo (repete ate acertar ou cancelar).
static bool wdc_locate_rom(const fs::path& cli_rom, bool rom_given, bool interactive, const fs::path& config_dir, const fs::path& exe_dir) {
    std::vector<fs::path> cands;
    if (rom_given) {
        cands.push_back(cli_rom);
    } else {
        cands.push_back(config_dir / "wdc.z64");
        cands.push_back(exe_dir / "wdc.z64");
        for (const fs::path& dir : {exe_dir, exe_dir / "rom"}) {
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                std::string x = e.path().extension().string();
                for (auto& c : x) c = (char)std::tolower((unsigned char)c);
                if (e.is_regular_file(ec) && (x == ".z64" || x == ".v64" || x == ".n64")) cands.push_back(e.path());
            }
        }
#ifdef WDC_DEFAULT_ROM
        if (std::strlen(WDC_DEFAULT_ROM) > 0) cands.push_back(WDC_DEFAULT_ROM);
#endif
    }
    for (const fs::path& c : cands) {
        std::error_code ec;
        if (!fs::exists(c, ec)) continue;
        if (recomp::select_rom(c, u8"wdc") == recomp::RomValidationError::Good) { std::printf("[main] ROM: %s\n", c.string().c_str()); return true; }
        std::fprintf(stderr, "ROM invalida: %s\n", c.string().c_str());
    }
    if (!interactive || rom_given) return false;
    if (!wdc_rom_explain()) return false;
    for (;;) {
        const fs::path p = wdc_rom_pick_dialog();
        if (p.empty()) return false;
        const auto rv = recomp::select_rom(p, u8"wdc");
        if (rv == recomp::RomValidationError::Good) return true;
        wdc_rom_show_error((int)rv, p);
    }
}

int main(int argc, char** argv) {
#ifdef _WIN32
    AddVectoredExceptionHandler(1, wdc_crash_log);
#endif
    bool console_attached = true;
#ifdef WDC_RELEASE_BUILD
    // Build de distribuicao = aplicativo de janela (sem console). Iniciado de um terminal, a saida vai para ele.
    {
        const HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
        const bool redirected = ho != nullptr && ho != INVALID_HANDLE_VALUE;   // o chamador ja redirecionou a saida (> arquivo, pipe): respeita
        console_attached = redirected;
        if (!redirected && AttachConsole(ATTACH_PARENT_PROCESS)) {
            console_attached = true;
            std::freopen("CONOUT$", "w", stdout);
            std::freopen("CONOUT$", "w", stderr);
        }
    }
#endif
    (void)console_attached;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--version")) { std::printf("%s\n", WDC_BUILD_LABEL); return 0; }
    fs::path rom_path;
    bool rom_given = false;
    const fs::path exe_dir = wdc_exe_dir();
    fs::path config_dir = wdc_pick_config_dir(exe_dir);
    int seconds = 30;
    bool seconds_given = false;
    std::string shot_path, wav_path, pak_path;
    bool run_setup = argc == 1;                // sem argumentos (clique duplo no executavel): setup; com --setup: setup tambem
    const char* setup_shot = nullptr;
    bool no_pak = false;
    bool audio_out = true;
    double shot_at = 20.0, shot_interval = 1.0;
    int shot_extra = 0;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--rom") && i + 1 < argc) { rom_path = argv[++i]; rom_given = true; }
        else if (!std::strcmp(argv[i], "--config") && i + 1 < argc) config_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) { seconds = std::atoi(argv[++i]); seconds_given = true; }
        else if (!std::strcmp(argv[i], "--pak") && i + 1 < argc) pak_path = argv[++i];          // Controller Pak virtual na porta 0 (imagem de 32 KB; criada se nao existir)
        else if (!std::strcmp(argv[i], "--no-pak")) no_pak = true;                              // --play liga o pak por padrao; isto desliga
        else if (!std::strcmp(argv[i], "--play")) g_play = true;      // modo interativo: janela, teclado e controle; roda ate fechar (Esc)
        else if (!std::strcmp(argv[i], "--autopress")) g_autopress = true;
        else if (!std::strcmp(argv[i], "--task-clock")) g_task_clock = true;
        else if (!std::strcmp(argv[i], "--players") && i + 1 < argc) g_players = std::max(1, std::min(4, std::atoi(argv[++i])));
        else if (!std::strcmp(argv[i], "--exit-after-script")) g_exit_after_script = true;
        else if (!std::strcmp(argv[i], "--no-rdp-vk")) g_rdp_vk = false;
        else if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) wdc_cfg_set_override("mode", argv[++i]);                   // legacy | enhanced
        else if (!std::strcmp(argv[i], "--fps60") && i + 1 < argc) wdc_cfg_set_override("enhanced.fps60", argv[++i]);        // on | off (so Enhanced)
        else if (!std::strcmp(argv[i], "--scale") && i + 1 < argc) wdc_cfg_set_override("enhanced.internal_scale", argv[++i]); // 1 | 2 | 4 | 8 (so Enhanced)
        else if (!std::strcmp(argv[i], "--vsync") && i + 1 < argc) wdc_cfg_set_override("display.vsync", argv[++i]);          // on | off
        else if (!std::strcmp(argv[i], "--filter") && i + 1 < argc) wdc_cfg_set_override("display.filter", argv[++i]);        // nearest | bilinear | sharp
        else if (!std::strcmp(argv[i], "--present") && i + 1 < argc) wdc_cfg_set_override("display.backend", argv[++i]);      // d3d11 | gdi
        else if (!std::strcmp(argv[i], "--setup")) run_setup = true;                                                       // mostra o setup (modo e controles) antes de jogar
        else if (!std::strcmp(argv[i], "--setup-shot") && i + 1 < argc) { run_setup = true; setup_shot = argv[++i]; }   // depuracao: captura do setup
        else if (!std::strcmp(argv[i], "--controls") && i + 1 < argc) wdc_cfg_set_override("controls.scheme", argv[++i]);     // classic | modern
        else if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];          // captura o audio do jogo (estereo s16, taxa do jogo)
        else if (!std::strcmp(argv[i], "--no-audio-out")) audio_out = false;                    // sem dispositivo: modelo nulo (ainda captura com --wav)
        else if (!std::strcmp(argv[i], "--rdp-gpu") && i + 1 < argc) g_rdp_gpu = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) { shot_path = argv[++i]; }          // --shot arquivo.png [--shot-at segundos]
        else if (!std::strcmp(argv[i], "--shot-at") && i + 1 < argc) { shot_at = std::atof(argv[++i]); }
        else if (!std::strcmp(argv[i], "--shot-count") && i + 1 < argc) { shot_extra = std::atoi(argv[++i]) - 1; }      // total de capturas
        else if (!std::strcmp(argv[i], "--shot-interval") && i + 1 < argc) { shot_interval = std::atof(argv[++i]); }
        else if (!std::strcmp(argv[i], "--null-rsp")) g_lle = false;   // renderizador nulo (sem executar o ucode)
        else if (!std::strcmp(argv[i], "--script") && i + 1 < argc) parse_script(argv[++i]);
        else if (!std::strcmp(argv[i], "--poke") && i + 1 < argc) parse_pokes(argv[++i]);
        else if (!std::strcmp(argv[i], "--dump-ram-every") && i + 4 < argc) { g_dump_every_dir = argv[++i]; g_dump_every_t = std::atof(argv[++i]); g_dump_every_dt = std::atof(argv[++i]); g_dump_every_left = std::atoi(argv[++i]); }   // varios dumps: <pasta>/ram_NN.bin
        else if (!std::strcmp(argv[i], "--dump-ram") && i + 2 < argc) { g_dump_ram_path = argv[++i]; g_dump_ram_t = std::atof(argv[++i]); }
        else if (!std::strcmp(argv[i], "--covsnap") && i + 1 < argc) g_cov_snap_at = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--dump-from") && i + 1 < argc) g_dump_from = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--watch") && i + 1 < argc) {
            std::string s = argv[++i];
            for (size_t p = 0; p < s.size();) {
                size_t q = s.find(',', p);
                if (q == std::string::npos) q = s.size();
                g_watch.push_back((uint32_t)std::strtoul(s.substr(p, q - p).c_str(), nullptr, 16));
                p = q + 1;
            }
        }
    }
    fs::create_directories(config_dir);
    g_config_dir = config_dir;
#ifdef WDC_RELEASE_BUILD
    if (!console_attached) {   // clique duplo (sem terminal): registra a execucao em <config>/wdc_log.txt (a ultima)
        const std::string lp = (config_dir / "wdc_log.txt").string();
        if (std::freopen(lp.c_str(), "w", stdout)) _dup2(_fileno(stdout), _fileno(stderr));
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
#endif
    wdc_cfg_load(config_dir);                                   // modo Legacy/Enhanced e opcoes (CLI > wdc_config.toml > padrao do modo)
    g_wdc_enh_fps60 = wdc_cfg().fps60 ? 1 : 0;                  // os hooks condicionais do C gerado (gen_wdc_toml.py, ENH_HOOKS) leem isto
    wdc_input_configure(wdc_cfg());                             // esquema de botoes do XInput (classico ou moderno) e botoes personalizados
    if (run_setup) {   // setup: escolhas gravadas em wdc_config.toml; Sair/fechar encerra
        if (!wdc_setup_run(config_dir, setup_shot)) return 0;
        if (setup_shot) return 0;
        wdc_cfg_load(config_dir);                               // recarrega o que o usuario escolheu
        g_wdc_enh_fps60 = wdc_cfg().fps60 ? 1 : 0;
        wdc_input_configure(wdc_cfg());
        g_play = true;
    }
    if (g_play) {
        if (!seconds_given) seconds = 0x3FFFFFFF;
        if (!g_rdp_vk) { std::printf("[main] --play precisa do ParaLLEl-RDP (sem --no-rdp-vk)\n"); return 1; }
        // a janela do jogo so abre depois de a ROM estar validada (mais abaixo): o seletor de ROM nao fica com uma janela vazia atras
    }
    if (!shot_path.empty()) { wdc_rdp_vk_request_shot(shot_path.c_str(), shot_at); wdc_rdp_vk_shot_sequence(shot_extra, shot_interval); }
    // Cada modo tem o seu Controller Pak: recordes e replays (re-simulacoes de amostras a 30 Hz) nao sao portaveis entre Legacy e Enhanced.
    if (g_play && pak_path.empty() && !no_pak) pak_path = (config_dir / (wdc_cfg().enhanced ? "wdc_pak0_enhanced.mpk" : "wdc_pak0.mpk")).string();
    if (!pak_path.empty() && !no_pak) wdc_pak_init(pak_path.c_str());
    recomp::register_config_path(config_dir);
    register_wdc_overlays();

    recomp::GameEntry entry{};
    entry.rom_hash = kWdcRomHash;
    entry.internal_name = "WORLD DRIVER CHAMP";
    entry.display_name = "World Driver Championship";
    entry.game_id = u8"wdc";
    entry.mod_game_id = "";                       // sem mods: pula o carregador de mods
    entry.save_type = recomp::SaveType::None;
    entry.is_enabled = true;
    entry.entrypoint_address = get_entrypoint_address();
    entry.entrypoint = recomp_entrypoint;
    entry.on_init_callback = on_init;
    recomp::register_game(entry);

    if (!wdc_locate_rom(rom_path, rom_given, run_setup || g_play, config_dir, exe_dir)) {
        std::fprintf(stderr, "Sem ROM valida do World Driver Championship (USA): use --rom <arquivo>\n");
        return 2;
    }
    if (g_play) {
        const std::string win_title = std::string("World Driver Championship - " WDC_BUILD_NAME " - ") + wdc_cfg_mode_name();
        if (!wdc_window_create(win_title.c_str(), 960, 720)) { std::printf("[main] nao consegui criar a janela\n"); return 1; }
        wdc_input_init();
        wdc_rdp_vk_set_present(wdc_window_present);
        std::printf("[input] teclado: setas=analogico WASD=direcional X=A C=B Z=Z Enter=Start Q=L E=R IJKL=C Esc=sair F11=tela cheia\n");
    }
    std::printf("[main] ROM validada e copiada para %s\n", (config_dir / "wdc.z64").string().c_str());

    recomp::Configuration cfg{};
    cfg.rsp_callbacks.get_rsp_microcode = get_rsp_microcode;
    cfg.renderer_callbacks.create_render_context = create_null_renderer;
    cfg.audio_callbacks = { wdc_audio_queue_samples, wdc_audio_frames_remaining, wdc_audio_set_frequency };
    cfg.input_callbacks = { input_poll, input_get, input_rumble, input_info };
    cfg.gfx_callbacks = { create_gfx, create_window, update_gfx };
    cfg.events_callbacks = { vi_callback, nullptr };
    cfg.error_handling_callbacks = { message_box };
    cfg.threads_callbacks = { wdc_game_thread_name };

    recomp::start_game(u8"wdc", "");

    // Relator de cobertura: a cada 0,5 s imprime as funcoes que executaram pela primeira vez.
    std::thread cov_reporter([] {
        size_t reported = 0;
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::vector<std::pair<double, std::string>> fresh;   // (relogio ativo da 1a chamada, nome): com --task-clock, o mesmo relogio das rotas e de WDC_SEG_WIN
            {
                std::lock_guard<std::mutex> l(g_cov_mutex);
                while (reported < g_cov_order.size() && fresh.size() < 400) { fresh.emplace_back(g_cov_first_t[reported], g_cov_order[reported].first); reported++; }
            }
            for (const auto& n : fresh) std::printf("[cov %7.2fs] +%s\n", n.first, n.second.c_str());
        }
    });
    cov_reporter.detach();

    // Observador de memoria (diagnostico): registra toda mudanca nas palavras de --watch (leitura sem sincronismo).
    if (!g_watch.empty()) {
        std::thread watcher([] {
            std::vector<uint32_t> last(g_watch.size(), 0xFFFFFFFFu);
            std::vector<bool> seen(g_watch.size(), false);
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (!g_rdram) continue;
                for (size_t i = 0; i < g_watch.size(); i++) {
                    const uint32_t v = *reinterpret_cast<volatile uint32_t*>(g_rdram + (g_watch[i] & 0x1FFFFFFF));
                    if (!seen[i] || v != last[i]) {
                        std::printf("[w %7.2fs] %08X: %08X -> %08X  (botoes=%04X)\n", now_s(), g_watch[i], seen[i] ? last[i] : 0u, v, g_last_buttons.load());
                        last[i] = v; seen[i] = true;
                    }
                }
            }
        });
        watcher.detach();
    }

    // Watchdog + batimento: mostra se o jogo esta vivo e encerra depois de N segundos.
    std::thread watchdog([seconds, config_dir] {
        bool window_closed = false, script_done = false;
        for (int s = 1; s <= seconds && !window_closed && !script_done; s++) {
            for (int k = 0; k < 10 && !window_closed && !script_done; k++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (g_play && wdc_window_closed()) window_closed = true;
                if (g_exit_after_script && g_script_end > 0.0 && wdc_now_s() > g_script_end + 3.0) script_done = true;
                if (g_dump_every_left > 0 && g_rdram && wdc_now_s() >= g_dump_every_t) {
                    char nm[600];
                    std::snprintf(nm, sizeof nm, "%s/ram_%02d.bin", g_dump_every_dir.c_str(), g_dump_every_idx++);
                    std::ofstream f(nm, std::ios::binary);
                    f.write((const char*)g_rdram, 4 * 1024 * 1024);
                    std::printf("[dump-ram %.2fs] RDRAM gravada em %s\n", wdc_now_s(), nm);
                    g_dump_every_t += g_dump_every_dt;
                    g_dump_every_left--;
                }
                if (g_dump_ram_t >= 0.0 && g_rdram && wdc_now_s() >= g_dump_ram_t) {
                    std::ofstream f(g_dump_ram_path, std::ios::binary);
                    f.write((const char*)g_rdram, 4 * 1024 * 1024);
                    std::printf("[dump-ram %.2fs] RDRAM gravada em %s\n", wdc_now_s(), g_dump_ram_path.c_str());
                    g_dump_ram_t = -1.0;
                }
            }
            if (window_closed || script_done) break;
            if (s == g_cov_snap_at) {   // --covsnap N: contadores no meio da execucao (a diferenca ate o fim mostra o que ainda roda)
                std::lock_guard<std::mutex> l(g_cov_mutex);
                std::ofstream f(config_dir / "cov_snap.txt");
                for (const auto& [n, c] : g_cov_order) f << *c << " " << n << "\n";
            }
            if (s % 2 == 0) {
                const uint64_t hb_tasks = std::max<uint64_t>(g_task_count.load(), wdc_gfx_tasks());
                std::printf("[hb %5.1fs] vi=%" PRIu64 " tarefas=%" PRIu64 " telas=%" PRIu64 "\n", now_s(), g_vi_count.load(), hb_tasks, g_frame_count.load());
#ifndef WDC_TRACE_BUILD
                static uint64_t hb_prev_tasks = 0, hb_prev_vi = 0;
                if (s > 6 && hb_tasks - hb_prev_tasks < 20 && g_vi_count.load() - hb_prev_vi > 60) {   // retracos seguem, tarefas graficas quase pararam
                    std::printf("[hb] ATENCAO: so %" PRIu64 " tarefas em 2 s com os retracos em andamento (jogo parado?)\n", hb_tasks - hb_prev_tasks);
                    wdc_blk_dump();
                }
                hb_prev_tasks = hb_tasks; hb_prev_vi = g_vi_count.load();
#endif
            }
        }
        g_stop = true;
        if (window_closed) std::printf("[main] janela fechada: encerrando\n");
        else if (script_done) std::printf("[main] roteiro concluido (%.1fs do relogio ativo): encerrando\n", wdc_now_s());
        else std::printf("[main] tempo esgotado (%ds): encerrando\n", seconds);
        if (g_renderer) g_renderer->print_summary();
        if (g_lle) wdc_rsp_print_stats();
        wdc_rdp_vk_print_stats();
        wdc_audio_shutdown();
        wdc_pak_flush();
        if (g_play) wdc_window_destroy();
#ifdef WDC_TRACE_BUILD
        wdc_dump_blocked();
#endif
        {
            std::lock_guard<std::mutex> l(g_cov_mutex);
            if (!g_cov_order.empty()) {
                std::ofstream f(config_dir / "cov_final.txt");
                for (const auto& [n, c] : g_cov_order) f << *c << " " << n << "\n";
                std::printf("[cov] %zu funcoes executadas de %zu (contagens em %s)\n", g_cov_order.size(),
                            (size_t)section_table[0].num_funcs, (config_dir / "cov_final.txt").string().c_str());
            }
        }
        // Saida imediata, SEM o encerramento ordenado do ultramodern::quit(): tudo que importa ja foi gravado acima (estatisticas, WAV, pak, cobertura).
        // O encerramento do runtime libera a RDRAM (VirtualFree em recomp::start) com as threads do jogo ainda vivas; a thread ociosa, acordada
        // por uma mensagem externa, le a RDRAM liberada e cai (acesso invalido em ultramodern::wait_for_external_message; capturado com o
        // registro de quedas nativas: ~metade das execucoes curtas disparadas do bash). Sair antes disso elimina a corrida.
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(0);
    });
    watchdog.detach();

    wdc_audio_init(wav_path.empty() ? nullptr : wav_path.c_str(), audio_out);
    recomp::start(cfg);
    // Saida SEM destrutores estaticos: as threads destacadas (RDP, audio, watchdog) ainda existem e a saida normal destroi seus objetos
    // globais com elas em uso (queda intermitente na saida, codigo 139, vista em 1 de ~8 execucoes de 75 s).
    wdc_pak_flush();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);
}
