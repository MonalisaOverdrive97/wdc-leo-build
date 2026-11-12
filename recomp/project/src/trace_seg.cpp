// Build trace: gravador de SEGMENTOS para o diferencial de CPU (tools/recomp/difftest_seg.py).
//
// Um segmento e o trecho de uma thread entre duas chamadas de funcoes do runtime (wrappers wdc_seg_* gerados por gen_seg_hooks.py).
// Em cada fronteira grava um registro: kind END (na entrada da funcao: estado que o MIPS real teria ao chegar la) ou START (na volta:
// estado de onde o codigo continua; PC de retomada = $ra do jal, que o pos-processador do C gera). A memoria vai como DELTA de paginas de 4 KB
// em relacao ao registro anterior (qualquer thread), entao o arquivo reconstitui a RDRAM em cada ponto.
//
// Uso: WDC_SEG=<pasta> [WDC_SEG_FROM=<s, padrao 8>] [WDC_SEG_N=<max registros, padrao 40000>]   -> <pasta>/seg.bin e <pasta>/seg_funcs.txt
//      ou varias janelas na mesma execucao: WDC_SEG_WIN=<de_s>:<registros>,<de_s>:<registros>,...  (fora das janelas nada e gravado; o delta de
//      memoria da 1a gravacao de cada janela cobre o que mudou no intervalo, entao o arquivo continua reconstituindo a RDRAM)
// Formato do seg.bin (little-endian do host): ver `struct Hdr` e write_record().
#include <atomic>
#include <cfenv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

#include "recomp.h"

extern double wdc_now_s();
extern uint8_t* wdc_get_rdram();
struct WdcSegFn { const char* name; unsigned vram; };
extern "C" const WdcSegFn wdc_seg_table[];
extern "C" const int wdc_seg_table_n;
extern "C" uint32_t wdc_rdp_color_image();   // wdc_rsp_hw.cpp
extern "C" uint32_t wdc_rdp_z_image();
extern "C" size_t wdc_rdp_color_images(uint32_t* out, size_t max);

namespace {
constexpr size_t kRam = 0x400000, kPage = 4096, kPages = kRam / kPage;
enum Kind : uint32_t { K_BASE = 0, K_START = 1, K_END = 2 };

#pragma pack(push, 1)
struct Hdr {
    uint32_t magic;      // 'SEG1'
    uint32_t kind;
    uint32_t tid;        // indice da thread (ordem de aparecimento)
    uint32_t fn;         // id da funcao do runtime (indice em wdc_seg_table)
    uint64_t seq;
    uint32_t ndirty;     // paginas de RDRAM alteradas desde o registro anterior
    uint32_t retpc;      // $ra do jal (PC de retomada)
    double t;
    uint32_t cimg, zimg; // enderecos RDP de color image / z image (a GPU escreve neles)
    uint32_t rounding;   // modo de arredondamento do FPU (0 nearest, 1 zero, 2 up, 3 down)
    uint32_t next;       // numero de paginas "externas" (DMA do RSP) que seguem as paginas sujas
};
#pragma pack(pop)

struct Rec {
    std::mutex mu;
    bool enabled = false, finished = false;
    struct Win { double from; uint64_t n; };
    std::vector<Win> wins;                 // janelas de gravacao, em ordem
    size_t wi = 0;                         // janela atual (ou a proxima, se !active)
    bool active = false;                   // dentro de uma janela
    uint64_t win_rec0 = 0;                 // nrec ao abrir a janela atual (depois do BASE)
    uint32_t epoch = 1;                    // muda a cada abertura/fechamento: invalida os segmentos abertos (t_open)
    std::string dir;
    FILE* f = nullptr;
    uint8_t* shadow = nullptr;
    uint64_t seq = 0, nrec = 0;
    std::map<DWORD, uint32_t> tids;
};
Rec R;
std::atomic<uint8_t> g_ext[kPages];
struct PiDma { uint32_t phys, len; std::chrono::steady_clock::time_point until; };
std::vector<PiDma> g_pi;   // com R.mu travado
__declspec(thread) bool t_open = false;
__declspec(thread) uint32_t t_epoch = 0;

void init_once() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char* d = std::getenv("WDC_SEG")) {
        R.enabled = true;
        R.dir = d;
        if (const char* w = std::getenv("WDC_SEG_WIN")) {   // "de:n,de:n,..."
            std::string spec = w;
            size_t i = 0;
            while (i < spec.size()) {
                size_t e = spec.find(',', i);
                if (e == std::string::npos) e = spec.size();
                const std::string item = spec.substr(i, e - i);
                i = e + 1;
                const size_t c = item.find(':');
                if (c == std::string::npos) continue;
                R.wins.push_back({std::atof(item.substr(0, c).c_str()), (uint64_t)std::atoll(item.substr(c + 1).c_str())});
            }
        } else {
            double from = 8.0;
            uint64_t n = 40000;
            if (const char* e = std::getenv("WDC_SEG_FROM")) from = std::atof(e);
            if (const char* e = std::getenv("WDC_SEG_N")) n = (uint64_t)std::atoll(e);
            R.wins.push_back({from, n});
        }
    }
}

uint32_t rounding_mode() {
    switch (std::fegetround()) {
    case FE_TOWARDZERO: return 1;
    case FE_UPWARD: return 2;
    case FE_DOWNWARD: return 3;
    default: return 0;
    }
}

// chamada com R.mu travado
void write_record(Kind kind, uint32_t fn, const recomp_context* ctx, uint32_t retpc) {
    uint8_t* ram = wdc_get_rdram();
    std::vector<uint32_t> dirty;
    for (size_t p = 0; p < kPages; p++)
        if (std::memcmp(ram + p * kPage, R.shadow + p * kPage, kPage) != 0) dirty.push_back((uint32_t)p);
    // DMA do PI (ROM -> RDRAM) em andamento: o runtime a executa numa thread propria, de forma assincrona, e a pagina de destino muda sem nenhuma thread do jogo
    // ter escrito nela. Cada registro remarca o destino enquanto a DMA pode estar em voo (ver note_pi_dma).
    {
        const auto now = std::chrono::steady_clock::now();
        for (size_t i = 0; i < g_pi.size();) {
            if (now > g_pi[i].until) { g_pi.erase(g_pi.begin() + (long)i); continue; }
            for (uint32_t a = g_pi[i].phys & ~(uint32_t)(kPage - 1); a < g_pi[i].phys + g_pi[i].len && a < kRam; a += kPage) g_ext[a / kPage].store(1);
            i++;
        }
    }
    // A marca de pagina externa fica "grudada" por 16 registros depois de drenada: a thread que escreve (DMA do RSP/PI) marca ANTES de escrever, e o registro
    // que drena a marca pode ser tirado antes de a escrita chegar; sem isso a mudanca apareceria no delta do registro seguinte sem marca nenhuma.
    static uint8_t ext_hold[kPages];
    std::vector<uint32_t> ext;
    for (size_t p = 0; p < kPages; p++) {
        if (g_ext[p].exchange(0)) ext_hold[p] = 16;
        if (ext_hold[p]) { ext.push_back((uint32_t)p); ext_hold[p]--; }
    }

    DWORD tid = GetCurrentThreadId();
    auto it = R.tids.find(tid);
    if (it == R.tids.end()) it = R.tids.emplace(tid, (uint32_t)R.tids.size()).first;

    Hdr h{};
    h.magic = 0x31474553u;   // "SEG1"
    h.kind = kind;
    h.tid = it->second;
    h.fn = fn;
    h.seq = R.seq++;
    h.ndirty = (uint32_t)dirty.size();
    h.retpc = retpc;
    h.t = wdc_now_s();
    h.cimg = wdc_rdp_color_image();
    h.zimg = wdc_rdp_z_image();
    h.rounding = rounding_mode();
    h.next = (uint32_t)ext.size();
    std::fwrite(&h, sizeof h, 1, R.f);
    uint64_t regs[32 + 32 + 2] = {};
    if (ctx) {
        std::memcpy(regs, &ctx->r0, 32 * 8);
        std::memcpy(regs + 32, &ctx->f0, 32 * 8);
        regs[64] = ctx->hi;
        regs[65] = ctx->lo;
    }
    std::fwrite(regs, sizeof regs, 1, R.f);
    for (uint32_t p : dirty) {
        std::fwrite(&p, 4, 1, R.f);
        std::fwrite(ram + (size_t)p * kPage, kPage, 1, R.f);
        std::memcpy(R.shadow + (size_t)p * kPage, ram + (size_t)p * kPage, kPage);
    }
    if (!ext.empty()) std::fwrite(ext.data(), 4, ext.size(), R.f);
    R.nrec++;
    static const bool flush_each = std::getenv("WDC_SEG_FLUSH") != nullptr;   // WDC_SEG_FLUSH=1: descarrega cada registro (o ultimo registro sobrevive a uma queda; mais lento)
    if (flush_each) std::fflush(R.f);
}

void finish_locked();
void write_fb_images();

// R.mu travado: fecha a janela cheia, abre a proxima quando chega a hora, encerra quando acabam as janelas.
void window_update_locked() {
    if (R.finished) return;
    if (R.active && R.nrec - R.win_rec0 >= R.wins[R.wi].n) {
        R.active = false;
        R.epoch++;
        write_fb_images();
        if (R.f) std::fflush(R.f);   // se o jogo cair depois, o que ja foi gravado esta no arquivo
        std::printf("[seg %.2fs] janela %zu fechada (%llu registros no total)\n", wdc_now_s(), R.wi, (unsigned long long)R.nrec);
        R.wi++;
    }
    if (R.active) return;
    if (R.wi >= R.wins.size()) { finish_locked(); return; }
    if (wdc_now_s() < R.wins[R.wi].from) return;
    if (!R.f) {
        char path[512];
        std::snprintf(path, sizeof path, "%s/seg.bin", R.dir.c_str());
        R.f = std::fopen(path, "wb");
        std::snprintf(path, sizeof path, "%s/seg_funcs.txt", R.dir.c_str());
        if (FILE* t = std::fopen(path, "w")) {
            for (int i = 0; i < wdc_seg_table_n; i++) std::fprintf(t, "%d %08X %s\n", i, wdc_seg_table[i].vram, wdc_seg_table[i].name);
            std::fclose(t);
        }
        if (!R.f) { std::printf("[seg] nao consegui abrir %s\n", path); R.finished = true; return; }
        R.shadow = (uint8_t*)std::calloc(1, kRam);
        write_record(K_BASE, 0, nullptr, 0);   // imagem completa
    }
    for (size_t p = 0; p < kPages; p++) g_ext[p].store(0);   // paginas de DMA do intervalo entre janelas nao pertencem a nenhum segmento
    R.active = true;
    R.epoch++;
    R.win_rec0 = R.nrec;
    std::printf("[seg %.2fs] janela %zu aberta em %s (%llu registros)\n", wdc_now_s(), R.wi, R.dir.c_str(), (unsigned long long)R.wins[R.wi].n);
}

// <pasta>/fb_images.txt: TODOS os enderecos de color image que o RDP usou ate agora (um por linha, hex). O difftest ignora a RDRAM desses buffers:
// a GPU os escreve de forma assincrona e o cabecalho dos registros so guarda o ultimo SET_COLOR_IMAGE visto.
void write_fb_images() {
    uint32_t a[3 * 256];
    const size_t n = wdc_rdp_color_images(a, 256);
    char path[512];
    std::snprintf(path, sizeof path, "%s/fb_images.txt", R.dir.c_str());
    if (FILE* t = std::fopen(path, "w")) {   // uma linha por buffer: endereco(hex) largura bits
        for (size_t i = 0; i < n && i < 256; i++) std::fprintf(t, "%06X %u %u\n", a[3 * i], a[3 * i + 1], a[3 * i + 2]);
        std::fclose(t);
    }
}

void finish_locked() {
    if (R.finished) return;
    write_fb_images();
    R.finished = true;
    if (R.f) { std::fclose(R.f); R.f = nullptr; }
    std::printf("[seg %.2fs] fim: %llu registros, %zu threads\n", wdc_now_s(), (unsigned long long)R.nrec, R.tids.size());
}
}  // namespace

// DMA do RSP para a RDRAM (wdc_rsp_dma): a pagina muda sem a CPU ter escrito
extern "C" void wdc_seg_mark_ext(uint32_t phys, uint32_t len) {
    if (!len) return;
    for (uint32_t a = phys & ~(uint32_t)(kPage - 1); a < phys + len && a < kRam; a += kPage) g_ext[a / kPage].store(1);
}

// Anota o destino de uma DMA ROM -> RDRAM pedida pelo jogo (osPiStartDma / osEPiRawStartDma, direcao OS_READ = 0). Argumentos pela ABI O32: a0..a3 e o
// resto na pilha a partir de sp+0x10. Chamada com R.mu travado.
void note_pi_dma(int id, const recomp_context* ctx, uint8_t* rdram) {
    const char* nm = wdc_seg_table[id].name;
    auto stack_word = [&](uint32_t off) { return *(const uint32_t*)(rdram + (((uint32_t)ctx->r29 + off) & 0x1FFFFFFF)); };
    uint32_t dir, dram, size;
    if (!std::strcmp(nm, "osPiStartDma")) { dir = (uint32_t)ctx->r6; dram = stack_word(0x10); size = stack_word(0x14); }
    else if (!std::strcmp(nm, "osEPiRawStartDma")) { dir = (uint32_t)ctx->r5; dram = (uint32_t)ctx->r7; size = stack_word(0x10); }
    else return;
    if (dir != 0 || size == 0 || size > kRam) return;
    g_pi.push_back({dram & 0x1FFFFFFF, size, std::chrono::steady_clock::now() + std::chrono::milliseconds(300)});
}

extern "C" void wdc_seg_call(uint8_t* rdram, recomp_context* ctx, int id, void (*real)(uint8_t*, recomp_context*)) {
    init_once();
    if (!R.enabled || R.finished) { real(rdram, ctx); return; }
    const uint32_t retpc = (uint32_t)ctx->r31;
    {
        std::lock_guard<std::mutex> l(R.mu);
        note_pi_dma(id, ctx, rdram);
        window_update_locked();
        if (R.active && t_open && t_epoch == R.epoch) write_record(K_END, (uint32_t)id, ctx, retpc);
        t_open = false;
    }
    real(rdram, ctx);
    {
        std::lock_guard<std::mutex> l(R.mu);
        window_update_locked();
        if (R.active) {
            write_record(K_START, (uint32_t)id, ctx, retpc);
            t_open = true;
            t_epoch = R.epoch;
        }
    }
}
