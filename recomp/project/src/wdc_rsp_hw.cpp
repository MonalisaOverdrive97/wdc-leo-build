// Modelo de hardware do RSP/RDP para o ucode grafico do WDC recompilado (LLE). Ver wdc_rsp_hw.hpp.
//
// Fontes de cada comportamento (nada aqui e inventado; o que vem de fora do repositorio esta marcado [ext]):
//  - bits de SP_STATUS (leitura/escrita): thirdparty/libreultra/include/2.0I/PR/rcp.h
//  - tratamento da interrupcao do SP (limpa SIG3, entrega SP ou SP_BREAK conforme SIG1|SIG2): libreultra/src/os/exceptasm.s (`rcp:`)
//  - SIG0 = osSpTaskYield, osSpTaskLoad limpa SIG0..2 e liga INTR_BREAK: libreultra/src/io/sptask.c e sptaskyield.c
//  - sequencia do ucode (SET_INTR em `12`, espera por SIG3, SIG0 em `02`): recomp/rsp/wdc_ucode_annotated.s
//  - comprimentos dos comandos do RDP e registradores DPC [ext]: documentacao publica do RDP/RSP do N64
//    (o manual Intro do repositorio nao traz o formato dos comandos do RDP).
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "recomp.h"
#include "librecomp/rsp.hpp"
#include "wdc_rsp_hw.hpp"
#include "wdc_rdp_vk.h"
#include "miniz.h"
#include <vector>

// Fornecidos por recomp/runtime_glue/wdc_runtime_stubs.cpp
bool wdc_post_sp_break();
bool wdc_post_sp();
bool wdc_post_dp();
double wdc_now_s();

namespace {
// SP_STATUS no formato de LEITURA (rcp.h): 0 halt, 1 broke, 2 dma busy, 3 dma full, 4 io busy, 5 sstep, 6 intr on break,
// 7..14 sinais 0..7.
constexpr uint32_t ST_HALT = 1u << 0, ST_BROKE = 1u << 1, ST_INTR_BREAK = 1u << 6;
constexpr uint32_t ST_SIG0 = 1u << 7, ST_SIG1 = 1u << 8, ST_SIG2 = 1u << 9, ST_SIG3 = 1u << 10;
std::atomic<uint32_t> g_sp_status{ST_HALT};

// DPC
std::atomic<uint32_t> g_dpc_start{0}, g_dpc_end{0}, g_dpc_current{0};
std::atomic<bool> g_dpc_start_valid{false};
std::atomic<uint32_t> g_dpc_status_ctl{0};   // bit 0 = XBUS_DMEM_DMA
uint32_t g_dpc_clock = 0;

// Estatisticas do consumidor de RDP
std::atomic<uint64_t> g_rdp_cmds{0}, g_rdp_sync_full{0}, g_rdp_unknown{0}, g_rdp_op[64], g_rdp_bytes{0};
std::atomic<uint64_t> g_dma_reads{0}, g_dma_writes{0}, g_sp_intr{0}, g_sp_break_posted{0}, g_tasks{0};
std::atomic<uint64_t> g_unhandled_dma_imem{0};
const int g_dma_log = [] { const char* e = std::getenv("WDC_DMALOG"); return e ? std::atoi(e) : 0; }();
std::atomic<int> g_dma_log_lines{0};

// Histograma de comandos do ucode realmente executados (despacho `jr $1` em 0x111C = fluxo A, 0x1180 = fluxo B)
uint64_t g_ops_a[256], g_ops_b[256];
// Invariantes dos triangulos do RDP (comandos 0x08..0x0F): YL/YM/YH em ponto fixo 11.2 de 14 bits com sinal; um setup correto
// satisfaz YH <= YM <= YL e fica perto da tela (240 linhas = 960 em 11.2).
std::atomic<uint64_t> g_tri_total{0}, g_tri_ordered{0}, g_tri_onscreen{0};
int32_t sext14(uint32_t v) { v &= 0x3FFF; return (v & 0x2000) ? (int32_t)v - 0x4000 : (int32_t)v; }

// Comprimento em bytes de cada comando do RDP (indice = bits 29..24 do primeiro word) [ext]. 0 = opcode sem uso.
const uint8_t k_rdp_len[64] = {
    8, 0, 0, 0, 0, 0, 0, 0,          // 00 NOP
    32, 48, 96, 112, 96, 112, 160, 176,   // 08..0F triangulos: fill, fill+Z, tex, tex+Z, shade, shade+Z, shade+tex, shade+tex+Z
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 16, 16, 8, 8,        // 24 TEX_RECT 25 TEX_RECT_FLIP 26 SYNC_LOAD 27 SYNC_PIPE
    8, 8, 8, 8, 8, 8, 8, 8,          // 28 SYNC_TILE 29 SYNC_FULL 2A KEY_GB 2B KEY_R 2C CONVERT 2D SCISSOR 2E PRIM_DEPTH 2F OTHER_MODES
    8, 0, 8, 8, 8, 8, 8, 8,          // 30 LOAD_TLUT 32 SET_TILE_SIZE 33 LOAD_BLOCK 34 LOAD_TILE 35 SET_TILE 36 FILL_RECT 37 FILL_COLOR
    8, 8, 8, 8, 8, 8, 8, 8,          // 38 FOG 39 BLEND 3A PRIM 3B ENV 3C COMBINE 3D TEX_IMAGE 3E Z_IMAGE 3F COLOR_IMAGE
};

uint32_t rd_word(uint8_t* rdram, uint32_t phys) {
    return MEM_W(0, (int64_t)(int32_t)(0x80000000u | (phys & 0x00FFFFFFu)));
}

// ---- Diagnostico (WDC_TRIDUMP=N): no N-esimo SYNC_FULL (0-based) rasteriza em software o MAPA DE COBERTURA dos triangulos RDP do quadro
// (cor por triangulo; preto = nenhum triangulo cobre o pixel) e pede o dump do framebuffer do RDRAM desse mesmo quadro. Serve para
// separar "geometria ausente no fluxo RDP" de "geometria presente mas pintada de preto pela GPU".
const int g_tridump = [] { const char* e = std::getenv("WDC_TRIDUMP"); return e ? std::atoi(e) : -1; }();
struct ColorImage { uint32_t addr = 0, width = 320, bits = 16; } g_cimg;
std::mutex g_cimg_mu;
std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_cimg_all;   // TODOS os enderecos de SET_COLOR_IMAGE vistos (a GPU escreve neles de forma assincrona) -> (maior largura, bits)
std::atomic<uint32_t> g_zimg{0};
struct TriRec { uint32_t op; uint32_t w[8]; };
std::vector<TriRec> g_frame_tris;

void write_coverage_png(const char* path, const std::vector<TriRec>& tris, uint32_t width) {
    int32_t maxy = 0;
    for (const auto& t : tris) { const int32_t yl = sext14(t.w[0]); if (yl > maxy) maxy = yl; }
    const uint32_t H = (maxy >> 2) > 245 ? 480 : 240;
    std::vector<uint8_t> img((size_t)width * H * 4, 0);
    uint32_t idx = 0;
    for (const auto& t : tris) {
        idx++;
        const int32_t yl = sext14(t.w[0]), ym = sext14(t.w[1] >> 16), yh = sext14(t.w[1]);
        const double XL = (int32_t)t.w[2] / 65536.0, DxL = (int32_t)t.w[3] / 65536.0, XH = (int32_t)t.w[4] / 65536.0, DxH = (int32_t)t.w[5] / 65536.0,
                     XM = (int32_t)t.w[6] / 65536.0, DxM = (int32_t)t.w[7] / 65536.0;
        const uint32_t h = idx * 2654435761u;
        const uint8_t cr = 64 + (h >> 24) % 192, cg = 64 + (h >> 16 & 255) % 192, cb = 64 + (h >> 8 & 255) % 192;
        for (int32_t sy = yh; sy <= yl; sy++) {
            const int32_t row = sy >> 2;
            if (row < 0 || row >= (int32_t)H) continue;
            const double yy = (sy - yh) / 4.0;
            const double xa = XH + DxH * yy;
            const double xb = sy < ym ? XM + DxM * yy : XL + DxL * ((sy - ym) / 4.0);
            int32_t x0 = (int32_t)(xa < xb ? xa : xb), x1 = (int32_t)(xa < xb ? xb : xa);
            for (int32_t x = x0; x <= x1; x++) {
                if (x < 0 || x >= (int32_t)width) continue;
                uint8_t* px = &img[((size_t)row * width + x) * 4];
                px[0] = cr; px[1] = cg; px[2] = cb; px[3] = 255;
            }
        }
    }
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory(img.data(), (int)width, (int)H, 4, &len);
    if (png) {
        if (FILE* f = std::fopen(path, "wb")) { std::fwrite(png, 1, len, f); std::fclose(f); }
        mz_free(png);
    }
    std::printf("[tridump] quadro (SYNC_FULL %d): %zu triangulos, imagem de cobertura %s (%ux%u); color image 0x%06X largura %u, %u bpp\n", g_tridump, tris.size(), path,
                width, H, g_cimg.addr, g_cimg.width, g_cimg.bits);
}

// Consome os comandos de [current, end) como o RDP faria e entrega OS_EVENT_DP a cada SYNC_FULL.
void rdp_process(uint8_t* rdram) {
    uint32_t cur = g_dpc_current.load();
    const uint32_t end = g_dpc_end.load();
    const bool vk = wdc_rdp_vk_active();
    static thread_local std::vector<uint32_t> batch;   // [n_palavras, palavras...] por comando, enviado a thread de RDP (ParaLLEl-RDP)
    batch.clear();
    while (cur + 8 <= end) {
        const uint32_t w0 = rd_word(rdram, cur);
        const uint32_t op = (w0 >> 24) & 0x3F;
        uint32_t len = k_rdp_len[op];
        if (len == 0) { g_rdp_unknown++; len = 8; }
        if (cur + len > end) break;   // comando incompleto: o RDP espera o proximo DPC_END
        g_rdp_cmds++;
        g_rdp_op[op]++;
        if (op >= 0x08 && op <= 0x0F) {
            const uint32_t w1 = rd_word(rdram, cur + 4);
            const int32_t yl = sext14(w0), ym = sext14(w1 >> 16), yh = sext14(w1);
            g_tri_total++;
            if (yh <= ym && ym <= yl) g_tri_ordered++;
            if (yh >= -0x400 && yl <= 0x7FF) g_tri_onscreen++;
        }
        g_rdp_bytes += len;
        if (op == 0x3F) {   // SET_COLOR_IMAGE: formato(23:21) tamanho(20:19) largura-1(9:0); w1 = endereco
            g_cimg.bits = 4u << ((w0 >> 19) & 3);
            g_cimg.width = (w0 & 0x3FF) + 1;
            g_cimg.addr = rd_word(rdram, cur + 4) & 0xFFFFFF;
            { std::lock_guard<std::mutex> l(g_cimg_mu); auto& e = g_cimg_all[g_cimg.addr]; if (g_cimg.width * g_cimg.bits >= e.first * e.second) e = {g_cimg.width, g_cimg.bits}; }
        }
        if (op == 0x3E) g_zimg = rd_word(rdram, cur + 4) & 0xFFFFFF;   // SET_Z_IMAGE
        if (g_tridump >= 0 && (int)g_rdp_sync_full.load() == g_tridump && op >= 0x08 && op <= 0x0F) {
            TriRec r{op, {}};
            for (int k = 0; k < 8; k++) r.w[k] = rd_word(rdram, cur + 4 * k);
            g_frame_tris.push_back(r);
        }
        if (vk) {
            batch.push_back(len / 4);
            for (uint32_t k = 0; k < len; k += 4) batch.push_back(rd_word(rdram, cur + k));
        }
        if (op == 0x29) {             // SYNC_FULL => interrupcao DP
            if (g_tridump >= 0 && (int)g_rdp_sync_full.load() == g_tridump) {
                char path[260];
                std::snprintf(path, sizeof(path), "%s/tri_%d_cobertura.png", std::getenv("WDC_TRIDUMP_DIR") ? std::getenv("WDC_TRIDUMP_DIR") : ".", g_tridump);
                write_coverage_png(path, g_frame_tris, g_cimg.width);
                std::snprintf(path, sizeof(path), "%s/tri_%d_framebuffer.png", std::getenv("WDC_TRIDUMP_DIR") ? std::getenv("WDC_TRIDUMP_DIR") : ".", g_tridump);
                wdc_rdp_vk_dump_fb(path, g_cimg.addr, g_cimg.width, 240, g_cimg.bits);   // entra na fila DEPOIS dos comandos deste quadro
                g_frame_tris.clear();
            }
            g_rdp_sync_full++;
            if (!vk) wdc_post_dp();   // com o ParaLLEl-RDP o evento sai da thread de RDP, depois de a GPU terminar
        }
        cur += len;
    }
    if (vk && !batch.empty()) wdc_rdp_vk_commands(batch.data(), batch.size());
    g_dpc_current.store(cur);
}

// Efeito da interrupcao do SP sobre o jogo: o que o __osException faria (exceptasm.s, `rcp:`).
void raise_sp_interrupt() {
    g_sp_intr++;
    const uint32_t status = g_sp_status.load();
    g_sp_status.fetch_and(~ST_SIG3);                 // li t1,(SP_CLR_INTR | SP_CLR_SIG3) ; sw t1, SP_STATUS
    if (status & (ST_SIG1 | ST_SIG2)) {              // andi t4, t4, 0x300 (YIELDED | TASKDONE)
        wdc_post_sp();                               // OS_EVENT_SP
    } else {
        g_sp_break_posted++;
        wdc_post_sp_break();                         // OS_EVENT_SP_BREAK
    }
}

// Espera ativa com cessao de CPU: o ucode gira em SP_STATUS/DPC esperando a CPU do jogo (outra thread).
struct SpinGuard {
    uint32_t last = 0xFFFFFFFFu;
    uint32_t same = 0;
    void observe(uint32_t v) {
        if (v == last) {
            ++same;
            if (same > 20000) std::this_thread::sleep_for(std::chrono::microseconds(200));
            else if (same > 200) std::this_thread::yield();
        } else { last = v; same = 0; }
    }
};
thread_local SpinGuard t_spin;
}  // namespace

// Usados pelo gravador de segmentos da build trace (trace_seg.cpp): enderecos que a GPU escreve na RDRAM
extern "C" uint32_t wdc_rdp_color_image() { return g_cimg.addr; }
extern "C" uint32_t wdc_rdp_z_image() { return g_zimg.load(); }
// Color images distintas vistas ate agora: out[3*i] = endereco, out[3*i+1] = largura (pixels), out[3*i+2] = bits por pixel, para i < min(total, max).
// Devolve o total. Usado pelo gravador de segmentos (fb_images.txt).
extern "C" size_t wdc_rdp_color_images(uint32_t* out, size_t max) {
    std::lock_guard<std::mutex> l(g_cimg_mu);
    size_t n = 0;
    for (const auto& kv : g_cimg_all) {
        if (n < max) { out[3 * n] = kv.first; out[3 * n + 1] = kv.second.first; out[3 * n + 2] = kv.second.second; }
        n++;
    }
    return n;
}
extern "C" uint64_t wdc_gfx_tasks() { return g_tasks.load(); }   // tarefas graficas executadas pelo ucode (LLE); base do --task-clock
#ifdef WDC_TRACE_BUILD
extern "C" void wdc_seg_mark_ext(uint32_t phys, uint32_t len);
#endif

// ---------------------------------------------------------------------------------------------
// DMA
// ---------------------------------------------------------------------------------------------
void wdc_rsp_dma(uint8_t* rdram, bool to_rdram, uint32_t mem_addr, uint32_t dram_addr, uint32_t len) {
    const uint32_t row = ((len & 0xFFF) | 7) + 1;
    const uint32_t count = ((len >> 12) & 0xFF) + 1;
    const uint32_t skip = (len >> 20) & 0xFFF;
    uint32_t m = mem_addr & 0xFF8;
    uint32_t d = dram_addr & 0xFFFFF8;
    if (mem_addr & 0x1000) { g_unhandled_dma_imem++; return; }   // o ucode do WDC nao usa overlays: IMEM nao e tocada
    (to_rdram ? g_dma_writes : g_dma_reads)++;
    if (g_dma_log && g_dma_log_lines < 4000 && (g_dma_log == 2 || to_rdram)) {   // WDC_DMALOG=1 (so escritas) ou 2 (tudo)
        g_dma_log_lines++;
        std::printf("[dma %7.2fs] tarefa #%llu %s dmem=0x%03X dram=0x%06X linha=0x%X x%u skip=0x%X\n", wdc_now_s(), (unsigned long long)g_tasks.load(),
                    to_rdram ? "W" : "R", mem_addr & 0xFFF, dram_addr & 0xFFFFFF, row, count, skip);
    }
    for (uint32_t r = 0; r < count; r++) {
#ifdef WDC_TRACE_BUILD
        if (to_rdram) wdc_seg_mark_ext(d & 0xFFFFFF, row);
#endif
        for (uint32_t i = 0; i < row; i++) {
            const int64_t ga = (int64_t)(int32_t)(0x80000000u | ((d + i) & 0x00FFFFFFu));
            if (to_rdram) MEM_B(0, ga) = RSP_MEM_B(i, m);
            else RSP_MEM_B(i, m) = MEM_B(0, ga);
        }
        m = (m + row) & 0xFF8;
        d += row + skip;
    }
}

// ---------------------------------------------------------------------------------------------
// COP0
// ---------------------------------------------------------------------------------------------
uint32_t wdc_rsp_mfc0(uint8_t*, int reg) {
    switch (reg) {
    case 4: {   // SP_STATUS
        const uint32_t v = g_sp_status.load();
        t_spin.observe(v);
        return v;
    }
    case 5: return 0;   // SP_DMA_FULL: DMA instantanea
    case 6: return 0;   // SP_DMA_BUSY
    case 7: return 0;   // SP_SEMAPHORE: sempre adquirido
    case 8: return g_dpc_start.load();
    case 9: return g_dpc_end.load();
    case 10: {          // DPC_CURRENT: o RDP "ja consumiu" tudo ate DPC_END
        const uint32_t v = g_dpc_current.load();
        t_spin.observe(v);
        return v;
    }
    case 11: {          // DPC_STATUS: bit 0 xbus, bit 7 cbuf ready, bit 10 start valid
        uint32_t v = (g_dpc_status_ctl.load() & 1) | (1u << 7);
        if (g_dpc_start_valid.load()) v |= 1u << 10;
        t_spin.observe(v);
        return v;
    }
    case 12: return ++g_dpc_clock;
    default: return 0;
    }
}

void wdc_rsp_mtc0(uint8_t* rdram, int reg, uint32_t v) {
    switch (reg) {
    case 4: {   // SP_STATUS (escrita: pares clear/set, rcp.h)
        t_spin = SpinGuard{};
        uint32_t clr = 0, set = 0;
        if (v & 0x00001) clr |= ST_HALT;
        if (v & 0x00002) set |= ST_HALT;
        if (v & 0x00004) clr |= ST_BROKE;
        if (v & 0x00080) clr |= ST_INTR_BREAK;
        if (v & 0x00100) set |= ST_INTR_BREAK;
        if (v & 0x00200) clr |= ST_SIG0;  if (v & 0x00400) set |= ST_SIG0;
        if (v & 0x00800) clr |= ST_SIG1;  if (v & 0x01000) set |= ST_SIG1;
        if (v & 0x02000) clr |= ST_SIG2;  if (v & 0x04000) set |= ST_SIG2;
        if (v & 0x08000) clr |= ST_SIG3;  if (v & 0x10000) set |= ST_SIG3;
        if (v & 0x20000) clr |= 1u << 11; if (v & 0x40000) set |= 1u << 11;
        if (v & 0x80000) clr |= 1u << 12; if (v & 0x100000) set |= 1u << 12;
        if (v & 0x200000) clr |= 1u << 13; if (v & 0x400000) set |= 1u << 13;
        if (v & 0x800000) clr |= 1u << 14; if (v & 0x1000000) set |= 1u << 14;
        g_sp_status.fetch_and(~clr);
        g_sp_status.fetch_or(set);
        if (v & 0x00010) raise_sp_interrupt();   // SP_SET_INTR
        break;
    }
    case 7: break;      // SP_SEMAPHORE
    case 8:             // DPC_START
        g_dpc_start.store(v & 0x00FFFFF8u);
        g_dpc_start_valid.store(true);
        break;
    case 9:             // DPC_END: se ha START pendente, o RDP recomeca dali; depois consome ate o END
        g_dpc_end.store(v & 0x00FFFFF8u);
        if (g_dpc_start_valid.exchange(false)) g_dpc_current.store(g_dpc_start.load());
        if (g_dpc_status_ctl.load() & 1) { /* XBUS (lista na DMEM): o ucode do WDC nao usa */ }
        rdp_process(rdram);
        break;
    case 11: {          // DPC_STATUS (escrita): bit 0 = CLR_XBUS, bit 1 = SET_XBUS
        if (v & 1) g_dpc_status_ctl.fetch_and(~1u);
        if (v & 2) g_dpc_status_ctl.fetch_or(1u);
        break;
    }
    default: break;
    }
}

// ---------------------------------------------------------------------------------------------
// Interface com a CPU do jogo (hooks de libultra em wdc_runtime_stubs.cpp) e com o runtime
// ---------------------------------------------------------------------------------------------
// osSpTaskLoad: SP_CLR_YIELD | SP_CLR_YIELDED | SP_CLR_TASKDONE | SP_SET_INTR_BREAK
void wdc_rsp_cpu_task_load() {
    g_sp_status.fetch_and(~(ST_SIG0 | ST_SIG1 | ST_SIG2));
    g_sp_status.fetch_or(ST_INTR_BREAK);
}
// osSpTaskYield: SP_SET_YIELD (= SET_SIG0)
void wdc_rsp_cpu_yield() { g_sp_status.fetch_or(ST_SIG0); }
// Le o SIG0 (usado pelo modo --null-rsp) sem o limpar
bool wdc_rsp_sig0_test_and_clear() {
    uint32_t cur = g_sp_status.load();
    while (cur & ST_SIG0) {
        if (g_sp_status.compare_exchange_weak(cur, cur & ~ST_SIG0)) return true;
    }
    return false;
}

// Ponto de entrada devolvido por get_rsp_microcode: reproduz o que o boot da libultra (IMEM 0x1000) e o osSpTaskStartGo fazem
// antes de o ucode comecar, e entao executa o ucode recompilado.
RspExitReason wdc_ucode(uint8_t* rdram, uint32_t ucode_addr);   // gerado pelo RSPRecomp

// Motor alternativo (WDC_RSP_ENGINE=ref): em vez do C gerado pelo RSPRecomp, o ucode (IMEM = texto original da ROM) roda no
// interpretador de referencia independente (codigo do MAME, tools/rspref), usando o MESMO modelo de hardware (DMA, SP_STATUS, DPC).
// Serve para separar defeitos do recompilador/VU dos do modelo de hardware.
#ifdef WDC_RSP_REF
#include "rsp_ref.h"
static RspExitReason wdc_ucode_ref(uint8_t* rdram) {
    static RspRef* R = nullptr;
    static uint8_t* cur_rdram = nullptr;
    static uint32_t dma_mem = 0, dma_dram = 0;
    cur_rdram = rdram;
    if (!R) {
        R = new RspRef();
        FILE* f = std::fopen(WDC_UCODE_TEXT_BIN, "rb");
        if (!f || std::fread(R->imem + 0x80, 1, 0xF60, f) != 0xF60) { std::fprintf(stderr, "[rsp-ref] nao consegui ler %s\n", WDC_UCODE_TEXT_BIN); std::abort(); }
        std::fclose(f);
        R->ext_dmem = dmem;
        R->m_sp_reg_r_func = [](int reg, uint32_t) { return wdc_rsp_mfc0(cur_rdram, reg); };
        R->m_dp_reg_r_func = [](int reg, uint32_t) { return wdc_rsp_mfc0(cur_rdram, reg + 8); };
        R->m_sp_reg_w_func = [](int reg, uint32_t d, uint32_t) {
            switch (reg) {
            case 0: dma_mem = d; break;
            case 1: dma_dram = d; break;
            case 2: wdc_rsp_dma(cur_rdram, false, dma_mem, dma_dram, d); break;
            case 3: wdc_rsp_dma(cur_rdram, true, dma_mem, dma_dram, d); break;
            default: wdc_rsp_mtc0(cur_rdram, reg, d); break;
            }
        };
        R->m_dp_reg_w_func = [](int reg, uint32_t d, uint32_t) { wdc_rsp_mtc0(cur_rdram, reg + 8, d); };
        R->m_sp_set_status_func = [](int, uint32_t, uint32_t) { R->m_sr |= RSP_STATUS_HALT | RSP_STATUS_BROKE; };   // BREAK
        std::printf("[rsp-ref] motor de referencia (MAME) ativo\n");
    }
    R->m_sr = 0; R->m_pc = 0x080; R->m_nextpc = 0xffff;
    // WDC_REF_TRACE=<tarefa_inicial>,<quantas>: lista os comandos do ucode (fluxo A em 0x111C, B em 0x1180) e despeja matrizes/vertices dos 0A/0C
    static long tr0 = -1, trn = 0;
    static bool tr_init = false;
    if (!tr_init) { tr_init = true; if (const char* e = std::getenv("WDC_REF_TRACE")) std::sscanf(e, "%ld,%ld", &tr0, &trn); }
    const long tno = (long)g_tasks.load();
    const bool tracing = tr0 >= 0 && tno >= tr0 && tno < tr0 + trn;
    auto dmem_b = [](uint32_t a) { return (uint8_t)RSP_MEM_BU(0, a & 0xFFF); };
    auto dmem_h = [&](uint32_t a) { return (uint16_t)((dmem_b(a) << 8) | dmem_b(a + 1)); };
    auto dump_mat = [&](const char* nome, uint32_t a) {
        std::printf("      %s @%03X (16.16, 4 linhas de 4):\n", nome, a & 0xFFF);
        for (int row = 0; row < 4; row++) {
            std::printf("        ");
            for (int c = 0; c < 4; c++) {
                const int i = row * 4 + c;
                const int16_t ip = (int16_t)dmem_h(a + 2 * i);
                const uint16_t fp = dmem_h(a + 0x20 + 2 * i);
                std::printf(" %10.4f", (double)ip + fp / 65536.0);
            }
            std::printf("\n");
        }
    };
    if (!tracing) {
        R->m_icount = 0x7fffffff;
        try { R->execute_run(); } catch (const std::exception& e) { std::fprintf(stderr, "[rsp-ref] %s (pc=%03X)\n", e.what(), R->m_ppc); std::abort(); }
    } else {
        std::printf("[trace] tarefa #%ld\n", tno);
        try {
            while (!(R->m_sr & RSP_STATUS_BROKE)) {
                const uint32_t pc12 = R->m_pc & 0xFFF;
                if (R->m_nextpc == 0xffff && (pc12 == 0x11C || pc12 == 0x180)) {
                    const uint32_t w0 = R->m_r[28], w1 = R->m_r[27], op = w0 >> 24;
                    std::printf("  [%c] cmd %02X w0=%08X w1=%08X\n", pc12 == 0x11C ? 'A' : 'B', op, w0, w1);
                    if (op == 0x0C) {
                        dump_mat("matriz", w0);
                        const uint32_t n = ((w1 >> 24) / 2 + 1) * 2, src = (w1 >> 12) & 0xFFF;
                        std::printf("      vertices (x,y,z s16) a partir de %03X, %u:", src, n);
                        for (uint32_t v = 0; v < n && v < 8; v++) std::printf(" (%d,%d,%d)", (int16_t)dmem_h(src + 6 * v), (int16_t)dmem_h(src + 6 * v + 2), (int16_t)dmem_h(src + 6 * v + 4));
                        std::printf("\n");
                    } else if (op == 0x0A) {
                        dump_mat("fonte0", w0);
                        dump_mat("fonte1", w1 >> 16);
                    } else if (op == 0x08) {
                        dump_mat("matriz", w1);
                    }
                }
                R->m_icount = 1;
                R->execute_run();
            }
        } catch (const std::exception& e) { std::fprintf(stderr, "[rsp-ref] %s (pc=%03X)\n", e.what(), R->m_ppc); std::abort(); }
    }
    return (R->m_sr & RSP_STATUS_BROKE) ? RspExitReason::Broke : RspExitReason::Invalid;
}
static const bool g_engine_ref = [] { const char* e = std::getenv("WDC_RSP_ENGINE"); return e && std::strcmp(e, "ref") == 0; }();
#else
static const bool g_engine_ref = false;
#endif

RspExitReason wdc_ucode_entry(uint8_t* rdram, uint32_t ucode_addr) {
    g_tasks++;
    // run_task gravou o OSTask em DMEM[0xFC0] com o tipo alterado pelo hook de osSpTaskStartGo: restaura o tipo original.
    RSP_MEM_W_STORE(0xFC0, 0, 1);
    // O boot copia exatamente `ucode_data_size` bytes (OSTask+0x1C) de `ucode_data` (OSTask+0x18) para DMEM 0; o run_task do
    // librecomp copiou 0xF80, o que sobrescreve DMEM[0xF10..0xF7F] com memoria do jogo.
    const uint32_t data_size = RSP_MEM_W_LOAD(0xFC0 + 0x1C, 0);
    const uint32_t data_ptr = RSP_MEM_W_LOAD(0xFC0 + 0x18, 0);
    memset(dmem + 0xF10, 0, 0xFC0 - 0xF10);
    wdc_rsp_dma(rdram, false, 0, data_ptr, data_size - 1);
    // osSpTaskStartGo: SP_SET_INTR_BREAK | CLR_SSTEP | CLR_BROKE | CLR_HALT
    g_sp_status.fetch_and(~(ST_HALT | ST_BROKE));
    g_sp_status.fetch_or(ST_INTR_BREAK);
#ifdef WDC_RSP_REF
    const RspExitReason r = g_engine_ref ? wdc_ucode_ref(rdram) : wdc_ucode(rdram, ucode_addr);
#else
    const RspExitReason r = wdc_ucode(rdram, ucode_addr);
#endif
    g_sp_status.fetch_or(ST_HALT | ST_BROKE);
    return r;
}

// WDC_RSP_SLOW_NS=<n>: espera ativa de n ns a cada comando do ucode. O RSP emulado e muito mais rapido que o real; serve para testar se um
// defeito visual e uma corrida CPU x RSP (sensivel a temporizacao).
static const uint64_t g_slow_ns = [] { const char* e = std::getenv("WDC_RSP_SLOW_NS"); return e ? (uint64_t)std::atoll(e) : 0ull; }();
static inline void slow_down() {
    if (!g_slow_ns) return;
    const auto t0 = std::chrono::steady_clock::now();
    while ((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count() < g_slow_ns) {}
}

void wdc_rsp_label_hit(uint32_t addr, uint32_t w0) {
    slow_down();
    if (addr == 0x111C) g_ops_a[w0 >> 24]++;
    else if (addr == 0x1180) g_ops_b[w0 >> 24]++;
}

void wdc_rsp_print_stats() {
    auto dump = [](const char* nome, const uint64_t* h) {
        std::printf("[rsp] comandos executados pelo ucode, %s:", nome);
        for (int i = 0; i < 256; i++) if (h[i]) std::printf(" %02X x%" PRIu64, i, h[i]);
        std::printf("\n");
    };
    dump("fluxo A", g_ops_a);
    dump("fluxo B", g_ops_b);
    std::printf("[rdp] triangulos: %" PRIu64 "; com YH<=YM<=YL: %" PRIu64 "; perto da tela (-256..511 px): %" PRIu64 "\n",
                g_tri_total.load(), g_tri_ordered.load(), g_tri_onscreen.load());
    std::printf("[rsp] tarefas LLE: %" PRIu64 "; DMA: %" PRIu64 " leituras, %" PRIu64 " escritas (IMEM ignorada: %" PRIu64 "); interrupcoes SP: %" PRIu64
                " (SP_BREAK entregues: %" PRIu64 ")\n",
                g_tasks.load(), g_dma_reads.load(), g_dma_writes.load(), g_unhandled_dma_imem.load(), g_sp_intr.load(), g_sp_break_posted.load());
    std::printf("[rdp] comandos consumidos: %" PRIu64 " (%" PRIu64 " bytes), SYNC_FULL: %" PRIu64 ", opcodes sem comprimento conhecido: %" PRIu64 "\n",
                g_rdp_cmds.load(), g_rdp_bytes.load(), g_rdp_sync_full.load(), g_rdp_unknown.load());
    std::printf("[rdp] por opcode:");
    for (int i = 0; i < 64; i++) if (g_rdp_op[i]) std::printf(" %02X x%" PRIu64, i, g_rdp_op[i].load());
    std::printf("\n");
}
