// Funcoes de libultra que o World Driver Championship chama e que o N64ModernRuntime nao implementa.
// Cada uma foi medida no C gerado (tools/recomp/link_gap.py): sem elas o link falha.
// Este arquivo faz parte do "projeto" que usa o runtime (como o Zelda64Recomp faz com os seus), nao do runtime.

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "librecomp/helpers.hpp"
#include "ultramodern/ultramodern.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cinttypes>
#include <algorithm>
#include <unordered_map>

// OS_EVENT_SP_BREAK (11). O ucode do WDC usa o comando 0x12 ("sync", cmd_12_sync em wdc_ucode_annotated.s) como
// sincronismo CPU<-RSP: escreve SP_STATUS = 0x12810 (seta SIG3, limpa SIG1/SIG2, LEVANTA a interrupcao do SP) e gira
// ate o CPU limpar o SIG3. O __osException (thirdparty/libreultra/src/os/exceptasm.s, rotulo `rcp:`) limpa SIG3 e,
// como SIG1/SIG2 (YIELDED/TASKDONE, mascara 0x300) estao zerados, entrega OS_EVENT_SP_BREAK em vez de OS_EVENT_SP.
// O osSetEventMesg do ultramodern (events.cpp) so trata SP, DP, AI e SI: o evento 11 e IGNORADO e a thread do
// jogo que espera nessa fila (func_80023F88, fila 0x800B2B40) trava para sempre. Aqui capturamos o par (fila, mensagem)
// registrado para o evento 11 (via wdc_hooks.h, que renomeia as chamadas do C gerado) e o renderizador posta uma
// mensagem por comando 0x12 que encontrar.
// Modo LLE (padrao): o ucode grafico recompilado roda na thread de RSP do runtime (wdc_rsp_hw.cpp); --null-rsp volta ao
// renderizador nulo que so percorre as listas.
bool g_lle = true;

// Eventos do SO que o ultramodern ignora ou cuja fila/mensagem ele nao expoe. Capturamos (fila, mensagem) de cada um via o hook
// de osSetEventMesg (wdc_hooks.h) para poder entrega-los nos pontos em que o hardware os geraria.
namespace {
std::atomic<uint32_t> g_ev_mq[16], g_ev_msg[16];
}
extern "C" void osSetEventMesg_recomp(uint8_t* rdram, recomp_context* ctx);   // implementacao real (librecomp)

extern "C" void osViSetEvent_recomp(uint8_t* rdram, recomp_context* ctx);   // implementacao real (librecomp)

extern "C" void wdc_hook_osViSetEvent(uint8_t* rdram, recomp_context* ctx) {
    static const int ov = [] { const char* e = std::getenv("WDC_VI_RETRACE"); return e ? std::atoi(e) : 0; }();
    std::printf("[evt] osViSetEvent(fila=0x%08X, msg=0x%08X, retracos=%u)%s\n", (uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6, ov ? " [SOBRESCRITO por WDC_VI_RETRACE]" : "");
    if (ov > 0) ctx->r6 = (uint32_t)ov;
    osViSetEvent_recomp(rdram, ctx);
}

extern "C" void wdc_hook_osSetEventMesg(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t ev = (uint32_t)ctx->r4;
    if (ev == 4 || ev == 9 || ev == 11) {   // OS_EVENT_SP, OS_EVENT_DP, OS_EVENT_SP_BREAK
        g_ev_msg[ev] = (uint32_t)ctx->r6;
        g_ev_mq[ev] = (uint32_t)ctx->r5;
        std::printf("[evt] osSetEventMesg(%s, fila=0x%08X, msg=0x%08X)\n", ev == 4 ? "SP" : ev == 9 ? "DP" : "SP_BREAK", (uint32_t)ctx->r5, (uint32_t)ctx->r6);
    }
    osSetEventMesg_recomp(rdram, ctx);
}

static bool post_event(uint32_t ev) {
    const uint32_t mq = g_ev_mq[ev].load();
    if (mq == 0) return false;
    ultramodern::enqueue_external_message_src((PTR(OSMesgQueue))mq, (OSMesg)g_ev_msg[ev].load(), false,
                                              ev == 9 ? ultramodern::EventMessageSource::Dp : ultramodern::EventMessageSource::Sp);
    return true;
}
// OS_EVENT_SP_BREAK (11). O ucode do WDC usa o comando 0x12 ("sync", cmd_12_sync em wdc_ucode_annotated.s) como
// sincronismo CPU<-RSP: escreve SP_STATUS = 0x12810 (seta SIG3, limpa SIG1/SIG2, LEVANTA a interrupcao do SP) e gira
// ate o CPU limpar o SIG3. O __osException (thirdparty/libreultra/src/os/exceptasm.s, rotulo `rcp:`) limpa SIG3 e,
// como SIG1/SIG2 (YIELDED/TASKDONE, mascara 0x300) estao zerados, entrega OS_EVENT_SP_BREAK em vez de OS_EVENT_SP.
// O osSetEventMesg do ultramodern (events.cpp) so trata SP, DP, AI e SI: o evento 11 e IGNORADO e a thread do
// jogo que espera nessa fila (func_80023F88, fila 0x800B2B40) trava para sempre.
bool wdc_post_sp_break() { return post_event(11); }
bool wdc_post_sp() { return post_event(4); }
// OS_EVENT_DP: o ultramodern o entrega no caminho do renderizador (apos send_dl); no LLE quem o gera e o RDP ao executar
// SYNC_FULL (wdc_rsp_hw.cpp, rdp_process).
bool wdc_post_dp() { return post_event(9); }

// ---- Estado do SP_STATUS compartilhado entre a CPU do jogo e o RSP (wdc_rsp_hw.cpp) ----
// libreultra/src/io/sptask.c: osSpTaskLoad faz SP_CLR_YIELD|CLR_YIELDED|CLR_TASKDONE|SET_INTR_BREAK, osSpTaskYield faz SP_SET_YIELD
// (= SET_SIG0). No ucode do WDC o comando 02 ("ir ao fluxo B") ESPERA o SIG0 e o limpa ao observa-lo: e o "kick" com que a CPU
// diz que a lista de calculo (fluxo B) esta pronta. O runtime ignora ambos; aqui eles atualizam o SP_STATUS modelado.
void wdc_rsp_cpu_task_load();
void wdc_rsp_cpu_yield();
bool wdc_rsp_sig0_test_and_clear();
extern "C" void osSpTaskLoad_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void osSpTaskYield_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void osSpTaskStartGo_recomp(uint8_t* rdram, recomp_context* ctx);

extern "C" void wdc_hook_osSpTaskLoad(uint8_t* rdram, recomp_context* ctx) {
    wdc_rsp_cpu_task_load();
    osSpTaskLoad_recomp(rdram, ctx);
}
extern "C" void wdc_hook_osSpTaskYield(uint8_t* rdram, recomp_context* ctx) {
    wdc_rsp_cpu_yield();
    osSpTaskYield_recomp(rdram, ctx);
}
// ultramodern::submit_rsp_task manda tarefas tipo M_GFXTASK ao renderizador (send_dl) e as demais a thread de RSP, que roda o
// ucode recompilado (get_rsp_microcode) e envia OS_EVENT_SP ao TERMINAR. O WDC so tem tarefas graficas, mas o ucode e
// customizado: no modo LLE trocamos o tipo (bit 12) para que o runtime a trate como tarefa de RSP. O jogo remonta o OSTask
// inteiro, tipo incluido, a cada quadro (func_80024F08), entao nao e preciso restaurar.
extern "C" void wdc_hook_osSpTaskStartGo(uint8_t* rdram, recomp_context* ctx) {
    if (g_lle) {
        OSTask* task = TO_PTR(OSTask, ctx->r4);
        if (task->t.type == M_GFXTASK) task->t.type = M_GFXTASK | 0x1000;
    }
    osSpTaskStartGo_recomp(rdram, ctx);
}

// Espera o SIG0 e o limpa (modo --null-rsp). Devolve false se estourou `timeout_ms` (o chamador decide se continua).
bool wdc_sig0_wait_and_clear(int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited++) {
        if (wdc_rsp_sig0_test_and_clear()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return wdc_rsp_sig0_test_and_clear();
}

// osPiReadIo(u32 devAddr, u32* data): le uma palavra do cartucho por PIO.
// Chamado por func_80040B04 e func_800427B8: o jogo passa &variavel_na_pilha em a1 e le o valor depois.
// (O n64sym tinha nomeado esse endereco como osPiWriteIo; o chamador mostra que e leitura.)
// Mesmo modelo de endereco de osPiStartDma_recomp: devAddr relativo ao cartucho, combinado com rom_base.
extern "C" void osPiReadIo_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t dev_addr = _arg<0, uint32_t>(rdram, ctx) | recomp::rom_base;
    gpr dram_addr = ctx->r5;
    recomp::do_rom_pio(rdram, dram_addr, dev_addr & 0x1FFFFFFF);
    _return<s32>(ctx, 0);
}

// osPfsIsPlug(OSMesgQueue* mq, u8* pattern): bitmask dos controles com Controller Pak (CONT_CARD_ON) [ext: libultra pfsisplug.c].
// Chamado por func_80039608, func_8003AB80 e func_8003B984. Com o pak virtual (wdc_pak.cpp) a porta 0 tem pak; sem ele, nenhuma.
bool wdc_pak_enabled();
extern "C" void osPfsIsPlug_recomp(uint8_t* rdram, recomp_context* ctx) {
    const gpr pattern = ctx->r5;
    if (pattern != 0) MEM_BU(0, pattern) = wdc_pak_enabled() ? 1 : 0;
    _return<s32>(ctx, 0);
}

// rmonPrintf: na libultra de ROM e um stub vazio no hardware, mas o jogo o chama de 141 funcoes com
// mensagens de depuracao ("RACEINIT", "trackID: %d", "menu selected is %d"...). Aqui elas sao formatadas
// e impressas: o jogo "narra" o que esta fazendo, o que permite dirigir os menus sem ver a tela.
//
// Convencao o32 para funcao variadica: fmt em $a0; argumentos em $a1,$a2,$a3 e depois na pilha a partir de
// sp+0x10. Palavras sao contadas a partir do $a0 (posicao 0); um double ocupa um par alinhado em posicao PAR.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {
const auto g_dbg_t0 = std::chrono::steady_clock::now();

struct VarArgs {
    uint8_t* rdram;
    recomp_context* ctx;
    int pos = 1;
    uint32_t word() {
        const uint32_t w = (pos < 4) ? (uint32_t)(&ctx->r4)[pos] : (uint32_t)MEM_W(pos * 4, ctx->r29);
        pos++;
        return w;
    }
    uint64_t dword() {
        if (pos & 1) pos++;
        const uint64_t hi = word();
        const uint64_t lo = word();
        return (hi << 32) | lo;
    }
};

std::string read_cstr(uint8_t* rdram, uint32_t vram) {
    if (vram == 0) return "(null)";
    std::string s;
    for (int i = 0; i < 1024; i++) {
        const char c = (char)MEM_B(i, (gpr)(int32_t)vram);
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}

std::string mips_format(uint8_t* rdram, recomp_context* ctx) {
    const std::string fmt = read_cstr(rdram, (uint32_t)ctx->r4);
    VarArgs a{ rdram, ctx };
    std::string out;
    char buf[2048];
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out.push_back(fmt[i]); continue; }
        size_t j = i + 1;
        std::string spec = "%";
        while (j < fmt.size() && std::strchr("-+ #0", fmt[j])) spec.push_back(fmt[j++]);
        if (j < fmt.size() && fmt[j] == '*') { spec += std::to_string((int)a.word()); j++; }
        else while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9') spec.push_back(fmt[j++]);
        if (j < fmt.size() && fmt[j] == '.') {
            spec.push_back('.'); j++;
            if (j < fmt.size() && fmt[j] == '*') { spec += std::to_string((int)a.word()); j++; }
            else while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9') spec.push_back(fmt[j++]);
        }
        int longs = 0; bool is_short = false;
        while (j < fmt.size() && std::strchr("lhLqjzt", fmt[j])) { if (fmt[j] == 'l') longs++; if (fmt[j] == 'h') is_short = true; j++; }
        if (j >= fmt.size()) { out += spec; break; }
        const char c = fmt[j];
        i = j;
        switch (c) {
        case '%': out.push_back('%'); break;
        case 'd': case 'i':
            if (longs >= 2) std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)a.dword());
            else { int v = (int)a.word(); if (is_short) v = (short)v; std::snprintf(buf, sizeof buf, (spec + "d").c_str(), v); }
            out += buf; break;
        case 'u': case 'x': case 'X': case 'o':
            if (longs >= 2) std::snprintf(buf, sizeof buf, (spec + "ll" + c).c_str(), (unsigned long long)a.dword());
            else { unsigned v = a.word(); if (is_short) v &= 0xFFFF; std::snprintf(buf, sizeof buf, (spec + c).c_str(), v); }
            out += buf; break;
        case 'c': std::snprintf(buf, sizeof buf, (spec + "c").c_str(), (int)a.word()); out += buf; break;
        case 's': { const std::string s = read_cstr(rdram, a.word()); std::snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str()); out += buf; break; }
        case 'p': std::snprintf(buf, sizeof buf, "0x%08X", a.word()); out += buf; break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            const uint64_t bits = a.dword();
            double v; std::memcpy(&v, &bits, sizeof v);
            std::snprintf(buf, sizeof buf, (spec + c).c_str(), v);
            out += buf; break;
        }
        default: out += spec; out.push_back(c); break;
        }
    }
    return out;
}
} // namespace

extern "C" void rmonPrintf_recomp(uint8_t* rdram, recomp_context* ctx) {
    static std::mutex m;
    static long long window_start = -1;
    static int in_window = 0;
    static long long dropped = 0;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_dbg_t0).count();
    std::string text = mips_format(rdram, ctx);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    std::lock_guard<std::mutex> l(m);
    const long long sec = (long long)t;
    if (sec != window_start) {                 // limite de 150 linhas por segundo para nao afogar a saida
        if (dropped) std::printf("[dbg %7.2fs] ... %lld mensagens descartadas no segundo anterior\n", t, dropped);
        window_start = sec; in_window = 0; dropped = 0;
    }
    if (++in_window > 150) { dropped++; return; }
    std::printf("[dbg %7.2fs] %s\n", t, text.c_str());
}

// ---- Gravacao de replay a 30 Hz quando o laco de corrida roda a 60 Hz (--exp60; ver EXP60_HOOKS em gen_wdc_toml.py) ----
// O replay guarda a entrada como amostras (1 byte de botoes + 1 byte de direcao, em RLE) gravadas UMA VEZ POR ITERACAO do laco por func_80003DA4
// (modo 1); o demo da abertura, o replay e o "fantasma" sao RE-SIMULACOES a partir dessas amostras (func_80003E74, modo 2). O formato original e
// 30 Hz. Em reproducao o laco volta a 30 Hz (hook em func_8002C500), entao a leitura nao precisa de ajuste; na gravacao (iteracoes a 60 Hz) as
// iteracoes impares so mantem o resultado anterior, sem avancar o fluxo (nem o indice de quadro em +0x70). (O caminho `play=1` abaixo, que
// reaplicava a ultima amostra aos blocos de pad na leitura, ficou sem uso: mantido para experimentos.)
namespace {
struct DemoStream {
    bool hold_next = false;
    uint32_t last_v0 = 1;
    uint32_t flags[2] = {}, fval[2] = {};
    int8_t axis[2] = {};
};
std::unordered_map<uint32_t, DemoStream> g_demo_streams;
struct { uint32_t stream = 0, pads = 0; int play = 0; } g_demo_pending;
constexpr uint32_t kPadStride = 0x7C;   // 2 jogadores por fluxo; bloco de pad de 0x7C bytes
}

extern "C" int wdc_demo_pre(uint8_t* rdram, recomp_context* ctx, int play) {
    const uint32_t stream = (uint32_t)ctx->r4, pads = (uint32_t)ctx->r5;
    DemoStream& st = g_demo_streams[stream * 2 + (play ? 1 : 0)];
    if (!st.hold_next) {   // iteracao par: executa o corpo original
        st.hold_next = true;
        g_demo_pending = { stream, pads, play };
        return 0;
    }
    st.hold_next = false;   // iteracao impar: so mantem
    if (play) {
        const uint32_t mask = (uint32_t)MEM_W(0x74, (gpr)(int32_t)stream);   // jogadores que o fluxo controla
        for (int p = 0; p < 2; p++) {
            if (!((mask >> p) & 1)) continue;
            const gpr pad = (gpr)(int32_t)(pads + p * kPadStride);
            MEM_W(0, pad) = (int32_t)(((uint32_t)MEM_W(0, pad) & 0xFF00FFFFu) | st.flags[p]);
            MEM_B(0x26, pad) = st.axis[p];
            MEM_W(0x20, pad) = (int32_t)st.fval[p];
        }
    }
    ctx->r2 = (gpr)(int32_t)st.last_v0;
    return 1;
}

extern "C" void wdc_demo_post(uint8_t* rdram, recomp_context* ctx, int play) {
    if (g_demo_pending.stream == 0 || g_demo_pending.play != play) return;
    DemoStream& st = g_demo_streams[g_demo_pending.stream * 2 + (play ? 1 : 0)];
    st.last_v0 = (uint32_t)ctx->r2;
    if (play) {
        const uint32_t mask = (uint32_t)MEM_W(0x74, (gpr)(int32_t)g_demo_pending.stream);
        for (int p = 0; p < 2; p++) {
            if (!((mask >> p) & 1)) continue;
            const gpr pad = (gpr)(int32_t)(g_demo_pending.pads + p * kPadStride);
            st.flags[p] = (uint32_t)MEM_W(0, pad) & 0x00FF0000u;
            st.axis[p] = MEM_B(0x26, pad);
            st.fval[p] = (uint32_t)MEM_W(0x20, pad);
        }
    }
}

// ---- Diagnostico de travamento: quem esta bloqueado em qual fila ----
// osRecvMesg/osSendMesg/osJamMesg do C gerado passam por wdc_blk_* (wdc_hooks.h; nao nas builds trace, que ja tem o proprio rastreio). Cada thread
// do jogo grava (fila, flags) numa posicao da tabela antes da chamada e a limpa depois; main.cpp chama wdc_blk_dump() no batimento quando as
// tarefas graficas quase param (retracos continuam). So filas: o C gerado das builds normais nao grava $ra, entao nao ha chamador.
extern "C" void osRecvMesg_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void osSendMesg_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void osJamMesg_recomp(uint8_t* rdram, recomp_context* ctx);
namespace {
struct BlockSlot {
    std::atomic<uint32_t> queue{0}, flags{0}, tid{0};
    std::atomic<uint8_t> op{0};            // 0 = nao esta em chamada; 'R' recv, 'S' send, 'J' jam
    std::atomic<int64_t> since_ms{0};
    std::atomic<uint64_t> calls{0};
};
constexpr int kBlockSlots = 64;
BlockSlot g_block[kBlockSlots];
std::atomic<int> g_block_n{0};
thread_local int t_block_slot = -1;
int64_t blk_now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void blk_call(uint8_t op, void (*real)(uint8_t*, recomp_context*), uint8_t* rdram, recomp_context* ctx) {
    if (t_block_slot < 0) {
        t_block_slot = g_block_n.fetch_add(1);
        if (t_block_slot >= kBlockSlots) t_block_slot = kBlockSlots - 1;
        g_block[t_block_slot].tid = (uint32_t)std::hash<std::thread::id>()(std::this_thread::get_id());
    }
    BlockSlot& b = g_block[t_block_slot];
    b.queue = (uint32_t)ctx->r4; b.flags = (uint32_t)ctx->r6; b.since_ms = blk_now_ms(); b.op = op;
    real(rdram, ctx);
    b.op = 0;
    b.calls++;
}
}
// osViSwapBuffer: o portao de buffer do RDP precisa saber qual framebuffer foi trocado (Enhanced, 60 Hz; ver wdc_rdp_vk.cpp).
void wdc_rdp_vk_note_swap(uint32_t fb_addr);   // wdc_rdp_vk.cpp
extern "C" void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx);   // implementacao real (librecomp)
extern "C" void wdc_hook_osViSwapBuffer(uint8_t* rdram, recomp_context* ctx) {
    wdc_rdp_vk_note_swap((uint32_t)ctx->r4);
    osViSwapBuffer_recomp(rdram, ctx);
}

// Modo Enhanced (hooks condicionais do C gerado, gen_wdc_toml.py ENH_HOOKS): chaves lidas pelo C gerado (wdc_hooks.h) e preenchidas por main.cpp a partir de wdc_config.
extern "C" int g_wdc_enh_fps60 = 0;     // corrida a 60 Hz (so Enhanced)
extern "C" int g_wdc_skip_recv = 0;     // o proximo osRecvMesg e dispensado (2a espera de retraco de func_800013DC no Enhanced); armado por um hook logo antes da chamada
extern "C" void wdc_blk_osRecvMesg(uint8_t* rdram, recomp_context* ctx) {
    if (g_wdc_skip_recv) { g_wdc_skip_recv = 0; ctx->r2 = 0; return; }
    blk_call('R', osRecvMesg_recomp, rdram, ctx);
}
extern "C" void wdc_blk_osSendMesg(uint8_t* rdram, recomp_context* ctx) { blk_call('S', osSendMesg_recomp, rdram, ctx); }
extern "C" void wdc_blk_osJamMesg(uint8_t* rdram, recomp_context* ctx)  { blk_call('J', osJamMesg_recomp, rdram, ctx); }

extern "C" void wdc_blk_dump() {
    static uint64_t prev[kBlockSlots] = {};
    const int64_t now = blk_now_ms();
    const int n = std::min(g_block_n.load(), kBlockSlots);
    std::printf("[blk] threads do jogo com chamadas de fila (%d):\n", n);
    for (int i = 0; i < n; i++) {
        BlockSlot& b = g_block[i];
        const uint8_t op = b.op.load();
        const uint64_t c = b.calls.load();
        std::printf("[blk]   t%02d tid=%08X %s q=%08X flags=%u%s  chamadas=%" PRIu64 " (+%" PRIu64 ")\n", i, b.tid.load(),
                    op ? (op == 'R' ? "RECV" : op == 'S' ? "SEND" : "JAM ") : "(fora)", b.queue.load(), b.flags.load(),
                    op ? (std::string(" bloqueado ha ") + std::to_string((now - b.since_ms.load()) / 1000.0) + "s").c_str() : "",
                    c, c - prev[i]);
        prev[i] = c;
    }
    std::fflush(stdout);
}
