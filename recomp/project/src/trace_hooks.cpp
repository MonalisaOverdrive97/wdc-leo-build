// Build trace: wrappers das trocas de mensagens do libultra + relatorio de threads bloqueadas.
// Nao inclui trace_hooks.h (que renomeia os simbolos): chama os osXxxMesg_recomp reais do librecomp.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <atomic>
#include <string>
#include <tuple>
#include <vector>
#include <windows.h>
#include <dbghelp.h>

#include "recomp.h"
#include "trace.h"

extern "C" {
void osRecvMesg_recomp(uint8_t* rdram, recomp_context* ctx);
void osSendMesg_recomp(uint8_t* rdram, recomp_context* ctx);
void osJamMesg_recomp(uint8_t* rdram, recomp_context* ctx);
void osCreateThread_recomp(uint8_t* rdram, recomp_context* ctx);
void osStartThread_recomp(uint8_t* rdram, recomp_context* ctx);
}

__declspec(thread) const char* wdc_shadow[WDC_SHADOW_MAX];
__declspec(thread) int wdc_shadow_depth = 0;
__declspec(thread) void* wdc_ctx = nullptr;

extern double wdc_now_s();
extern uint8_t* wdc_get_rdram();            // main.cpp
extern void wdc_write_cov(const char* tag); // main.cpp: grava cov_<tag>.txt na pasta de configuracao
extern void wdc_print_gfx_summary();        // main.cpp
extern bool wdc_is_stopping();              // main.cpp

namespace {
struct Blocked { const char* op; uint32_t queue, msg, flags; std::string stack; double since; };
std::mutex g_mutex;
std::map<DWORD, Blocked> g_blocked;                                  // thread -> chamada em curso
std::set<std::tuple<std::string, uint32_t, std::string>> g_seen;     // (op, fila, funcao) ja relatados
std::map<std::pair<std::string, uint32_t>, uint64_t> g_counts;       // (op, fila) -> chamadas

std::string stack_string(int max_frames) {
    std::string s;
    const int depth = wdc_shadow_depth < WDC_SHADOW_MAX ? wdc_shadow_depth : WDC_SHADOW_MAX;
    for (int i = depth - 1, n = 0; i >= 0 && n < max_frames; i--, n++) {
        if (!s.empty()) s += " < ";
        s += wdc_shadow[i];
    }
    return s;
}

// WDC_MSGLOG=<hex>[,<hex>...]: registra TODA chamada (com tempo e pilha) nas filas indicadas, antes e depois.
std::set<uint32_t> msglog_queues() {
    static std::set<uint32_t> s = [] {
        std::set<uint32_t> r;
        if (const char* e = std::getenv("WDC_MSGLOG")) {
            const char* p = e;
            while (*p) {
                char* end = nullptr;
                const unsigned long v = std::strtoul(p, &end, 16);
                if (end == p) break;
                r.insert((uint32_t)v);
                p = (*end == ',') ? end + 1 : end;
            }
        }
        return r;
    }();
    return s;
}

void hooked(const char* op, void (*real)(uint8_t*, recomp_context*), uint8_t* rdram, recomp_context* ctx) {
    const uint32_t queue = (uint32_t)ctx->r4, msg = (uint32_t)ctx->r5, flags = (uint32_t)ctx->r6;
    const std::string top = (wdc_shadow_depth > 0 && wdc_shadow_depth <= WDC_SHADOW_MAX) ? wdc_shadow[wdc_shadow_depth - 1] : "?";
    const bool logall = msglog_queues().count(queue) != 0;
    if (logall) std::printf("[mq %8.4fs] >> %s q=%08X msg=%08X flags=%u tid=%lu  %s\n", wdc_now_s(), op, queue, msg, flags, GetCurrentThreadId(), stack_string(5).c_str());
    {
        std::lock_guard<std::mutex> l(g_mutex);
        g_counts[{ op, queue }]++;
        if (g_seen.insert({ op, queue, top }).second) {
            std::printf("[msg %7.2fs] %s q=%08X msg=%08X flags=%u  %s\n", wdc_now_s(), op, queue, msg, flags, stack_string(6).c_str());
        }
        g_blocked[GetCurrentThreadId()] = { op, queue, msg, flags, stack_string(10), wdc_now_s() };
    }
    real(rdram, ctx);
    {
        std::lock_guard<std::mutex> l(g_mutex);
        g_blocked.erase(GetCurrentThreadId());
    }
    if (logall) std::printf("[mq %8.4fs] << %s q=%08X ret=%d tid=%lu\n", wdc_now_s(), op, queue, (int)ctx->r2, GetCurrentThreadId());
}
}  // namespace

extern "C" void wdc_hook_osRecvMesg(uint8_t* rdram, recomp_context* ctx) { hooked("RECV", osRecvMesg_recomp, rdram, ctx); }
extern "C" void wdc_hook_osSendMesg(uint8_t* rdram, recomp_context* ctx) { hooked("SEND", osSendMesg_recomp, rdram, ctx); }
extern "C" void wdc_hook_osJamMesg(uint8_t* rdram, recomp_context* ctx) { hooked("JAM ", osJamMesg_recomp, rdram, ctx); }

// Criacao/inicio de threads do jogo (ordem de escalonamento).
extern "C" void wdc_hook_osCreateThread(uint8_t* rdram, recomp_context* ctx) {
    std::printf("[thr %8.4fs] osCreateThread(t=%08X id=%u entry=%08X) tid=%lu  %s\n", wdc_now_s(), (uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6,
                GetCurrentThreadId(), stack_string(4).c_str());
    osCreateThread_recomp(rdram, ctx);
}
extern "C" void wdc_hook_osStartThread(uint8_t* rdram, recomp_context* ctx) {
    std::printf("[thr %8.4fs] osStartThread(t=%08X) tid=%lu  %s\n", wdc_now_s(), (uint32_t)ctx->r4, GetCurrentThreadId(), stack_string(4).c_str());
    osStartThread_recomp(rdram, ctx);
    std::printf("[thr %8.4fs] osStartThread(t=%08X) retornou tid=%lu\n", wdc_now_s(), (uint32_t)ctx->r4, GetCurrentThreadId());
}


// ---- Sondas de diagnostico do laco de poligonos da CPU (func_8002703C, caminho de triangulos; ver TRACE_PROBES em gen_wdc_toml.py) ----
extern "C" int wdc_exp(int n) {
    static const std::set<int> on = [] {
        std::set<int> r;
        if (const char* e = std::getenv("WDC_EXP")) {
            for (const char* p = e; *p;) {
                char* end = nullptr;
                const long v = std::strtol(p, &end, 10);
                if (end == p) break;
                r.insert((int)v);
                p = (*end == ',') ? end + 1 : end;
            }
        }
        return r;
    }();
    return on.count(n) ? 1 : 0;
}
namespace {
enum Counter { C_TEST, C_FLAG, C_DEPTH_TEST, C_DEPTH, C_CLIP_NZ, C_CLIP_Z, C_FACE, C_EMIT, C_QPASS, C_COUNT };
std::atomic<uint64_t> g_pc[C_COUNT];
std::atomic<int> g_probe_lines{0};
double probe_from() {
    static const double v = [] { const char* e = std::getenv("WDC_PROBE_FROM"); return e ? std::atof(e) : 1e9; }();
    return v;
}
void print_vertex(uint8_t* rdram, uint32_t p) {
    const int64_t a = (int64_t)(int32_t)p;
    std::printf(" (%d,%d,z=%d,fl=%02X/%02X)", (int)(int16_t)MEM_H(0, a), (int)(int16_t)MEM_H(2, a), (int)(int16_t)MEM_H(0xE, a),
                (unsigned)(uint8_t)MEM_BU(0xC, a), (unsigned)(uint8_t)MEM_BU(0xD, a));
}
}  // namespace
// WDC_SNAP=<pasta>: teste diferencial contra um emulador MIPS (tools/recomp/difftest_mips.py). Guarda o estado (32 GPRs de 64 bits + 4 MB de RDRAM em ordem
// de palavra do host) antes de cada chamada do recortador; se ele "falhar" (devolve o ponteiro de saida recebido) grava antes/depois em disco.
namespace {
constexpr size_t kSnapRam = 0x400000;
std::vector<uint8_t> g_snap_before;
uint64_t g_snap_regs_before[32];
int g_snap_count = 0;
void snap_before(uint8_t* rdram, recomp_context* ctx) {
    if (!std::getenv("WDC_SNAP") || g_snap_count >= 4 || wdc_now_s() < probe_from()) return;
    g_snap_before.assign(rdram, rdram + kSnapRam);
    std::memcpy(g_snap_regs_before, &ctx->r0, sizeof(g_snap_regs_before));
}
void snap_write(const char* path, const uint64_t* regs, const uint8_t* ram) {
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(regs, 8, 32, f);
        std::fwrite(ram, 1, kSnapRam, f);
        std::fclose(f);
    }
}
void snap_after(uint8_t* rdram, recomp_context* ctx) {
    const char* dir = std::getenv("WDC_SNAP");
    if (!dir || g_snap_before.empty() || g_snap_count >= 4) return;
    if ((uint32_t)ctx->r2 != (uint32_t)g_snap_regs_before[16]) { g_snap_before.clear(); return; }   // recortador produziu saida: nao e o caso procurado
    char a[300], b[300];
    std::snprintf(a, sizeof(a), "%s/snap%d_before.bin", dir, g_snap_count);
    std::snprintf(b, sizeof(b), "%s/snap%d_after.bin", dir, g_snap_count);
    snap_write(a, g_snap_regs_before, g_snap_before.data());
    snap_write(b, &ctx->r0, rdram);
    std::printf("[snap] caso %d gravado em %s (v0=%08X s0_antes=%08X)\n", g_snap_count, dir, (uint32_t)ctx->r2, (uint32_t)g_snap_regs_before[16]);
    g_snap_count++;
    g_snap_before.clear();
}
}  // namespace

namespace {
// Passagem de poligonos inteira (func_8002784C): entrada/saida, arquivos passN_before.bin / passN_after.bin (formato de snapN).
std::vector<uint8_t> g_pass_before;
uint64_t g_pass_regs[32];
int g_pass_count = 0;
uint64_t g_pass_c0[C_COUNT];
void pass_before(uint8_t* rdram, recomp_context* ctx) {
    if (!std::getenv("WDC_SNAP") || g_pass_count >= 2 || wdc_now_s() < probe_from()) return;
    for (int i = 0; i < C_COUNT; i++) g_pass_c0[i] = g_pc[i].load();
    g_pass_before.assign(rdram, rdram + kSnapRam);
    std::memcpy(g_pass_regs, &ctx->r0, sizeof(g_pass_regs));
}
void pass_after(uint8_t* rdram, recomp_context* ctx) {
    const char* dir = std::getenv("WDC_SNAP");
    if (!dir || g_pass_before.empty() || g_pass_count >= 2) return;
    const char* mn = std::getenv("WDC_SNAP_MIN");
    const uint64_t accepted = (g_pc[C_DEPTH_TEST] - g_pass_c0[C_DEPTH_TEST]) + (g_pc[C_QPASS] - g_pass_c0[C_QPASS]);
    if (accepted < (uint64_t)(mn ? std::atoi(mn) : 0)) { g_pass_before.clear(); return; }   // so passagens com poligonos que passaram no teste de flags
    char a[300], b[300];
    std::snprintf(a, sizeof(a), "%s/pass%d_before.bin", dir, g_pass_count);
    std::snprintf(b, sizeof(b), "%s/pass%d_after.bin", dir, g_pass_count);
    snap_write(a, g_pass_regs, g_pass_before.data());
    snap_write(b, &ctx->r0, rdram);
    std::printf("[snap] passagem %d gravada em %s; nesta passagem: triangulos testados %llu, flags %llu, prof-teste %llu, recortador !=0 %llu / ==0 %llu, face %llu, emitidos %llu\n",
                g_pass_count, dir, (unsigned long long)(g_pc[C_TEST] - g_pass_c0[C_TEST]), (unsigned long long)(g_pc[C_FLAG] - g_pass_c0[C_FLAG]),
                (unsigned long long)(g_pc[C_DEPTH_TEST] - g_pass_c0[C_DEPTH_TEST]), (unsigned long long)(g_pc[C_CLIP_NZ] - g_pass_c0[C_CLIP_NZ]),
                (unsigned long long)(g_pc[C_CLIP_Z] - g_pass_c0[C_CLIP_Z]), (unsigned long long)(g_pc[C_FACE] - g_pass_c0[C_FACE]), (unsigned long long)(g_pc[C_EMIT] - g_pass_c0[C_EMIT]));
    g_pass_count++;
    g_pass_before.clear();
}
}  // namespace

extern "C" void wdc_probe_poly(uint8_t* rdram, recomp_context* ctx, int site) {
    const bool log = wdc_now_s() >= probe_from() && g_probe_lines.load() < 400;
    auto show = [&](const char* tag) {
        g_probe_lines++;
        std::printf("[poly %7.2fs] %s s1=%08X v:", wdc_now_s(), tag, (uint32_t)ctx->r17);
        print_vertex(rdram, (uint32_t)ctx->r20);
        print_vertex(rdram, (uint32_t)ctx->r21);
        print_vertex(rdram, (uint32_t)ctx->r19);
        std::printf("  [s2=%d fp=%d]\n", (int)(int32_t)ctx->r18, (int)(int32_t)ctx->r30);
    };
    switch (site) {
    case 2: g_pc[C_TEST]++;       if ((ctx->r25 & 0xFF) != 0) { g_pc[C_FLAG]++; if (log && std::getenv("WDC_PROBE_ALL")) show("DESCARTE-FLAGS"); } break;
    case 3: g_pc[C_DEPTH_TEST]++; if (log && (ctx->r1 != 0 || ((ctx->r3 | ctx->r4 | ctx->r6) & 0xFF) != 0)) show(ctx->r1 != 0 ? "DESCARTE-PROF" : "VAI-AO-RECORTADOR"); if (ctx->r1 != 0) g_pc[C_DEPTH]++; break;
    case 4: snap_after(rdram, ctx); (ctx->r2 != 0 ? g_pc[C_CLIP_NZ] : g_pc[C_CLIP_Z])++; if (log) { g_probe_lines++; std::printf("[poly %7.2fs]    recortador devolveu 0x%08X (s0 antes=%08X)\n", wdc_now_s(), (uint32_t)ctx->r2, (uint32_t)ctx->r16); } break;
    case 6: if (ctx->r1 == 0) { g_pc[C_FACE]++; } break;
    case 7: g_pc[C_EMIT]++; break;
    case 8: snap_before(rdram, ctx); break;
    case 11:   // quadrilateros: s1 = poligono, r24 = AND das flags (+0xC) dos 4 vertices (v1,a0,a2,a3)
        if ((ctx->r24 & 0xFF) == 0) g_pc[C_QPASS]++;
        if (log) { g_probe_lines++; std::printf("[quad %7.2fs] s1=%08X and=%02X fl=%02X/%02X/%02X/%02X\n", wdc_now_s(), (uint32_t)ctx->r17, (unsigned)(ctx->r24 & 0xFF),
                                                   (unsigned)(ctx->r3 & 0xFF), (unsigned)(ctx->r4 & 0xFF), (unsigned)(ctx->r6 & 0xFF), (unsigned)(ctx->r7 & 0xFF)); }
        break;
    case 9: pass_before(rdram, ctx); break;
    case 10: pass_after(rdram, ctx); break;
    default: break;
    }
}
void wdc_probe_report() {
    std::printf("[probe] triangulos testados: %llu; descartados por flags do RSP: %llu; teste de profundidade: %llu (descartados %llu); "
                "recortador: %llu devolveram !=0, %llu devolveram 0; descarte de face: %llu; emitidos sem recorte: %llu\n",
                (unsigned long long)g_pc[C_TEST].load(), (unsigned long long)g_pc[C_FLAG].load(), (unsigned long long)g_pc[C_DEPTH_TEST].load(),
                (unsigned long long)g_pc[C_DEPTH].load(), (unsigned long long)g_pc[C_CLIP_NZ].load(), (unsigned long long)g_pc[C_CLIP_Z].load(),
                (unsigned long long)g_pc[C_FACE].load(), (unsigned long long)g_pc[C_EMIT].load());
}
// Violacao de acesso no codigo recompilado: imprime o que o jogo estava fazendo (pilha de funcoes da thread que caiu).
// Sem isto o processo morre com 0xC0000005 e nenhuma informacao.
namespace {
LONG WINAPI wdc_crash_filter(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (wdc_is_stopping()) return EXCEPTION_CONTINUE_SEARCH;   // encerramento pedido pelo watchdog: threads sendo derrubadas, nao e queda
    std::printf("\n[CRASH] excecao 0x%08lX em RIP=%p", er->ExceptionCode, er->ExceptionAddress);
    {   // RIP -> funcao/arquivo:linha (requer o PDB ao lado do exe; /Z7 + /DEBUG na build trace)
        HANDLE proc = GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        if (SymInitialize(proc, nullptr, TRUE)) {
            char buf[sizeof(SYMBOL_INFO) + 256] = {};
            SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 255;
            DWORD64 disp = 0;
            DWORD ldisp = 0;
            IMAGEHLP_LINE64 ln = {};
            ln.SizeOfStruct = sizeof(ln);
            const DWORD64 pc = (DWORD64)er->ExceptionAddress;
            if (SymFromAddr(proc, pc, &disp, si)) std::printf("\n[CRASH] funcao host: %s+0x%llX", si->Name, (unsigned long long)disp);
            if (SymGetLineFromAddr64(proc, pc, &ldisp, &ln)) std::printf("\n[CRASH] linha: %s:%lu", ln.FileName, ln.LineNumber);
        }
    }
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        std::printf(" (%s endereco host %p)", er->ExceptionInformation[0] == 0 ? "leitura de" : er->ExceptionInformation[0] == 1 ? "escrita em" : "execucao de",
                    (void*)er->ExceptionInformation[1]);
        // Endereco guest: o C gerado acessa rdram[(endereco_guest - 0xFFFFFFFF80000000) ^ 3] (palavras guardadas invertidas por byte).
        const uint8_t* rd = wdc_get_rdram();
        if (rd) {
            const int64_t off = (int64_t)(er->ExceptionInformation[1] - (uintptr_t)rd);
            std::printf("\n[CRASH] rdram=%p, deslocamento host-rdram = 0x%llX (guest KSEG0 ~ 0x%08llX; 4 MB = 0x400000)", (const void*)rd,
                        (unsigned long long)off, (unsigned long long)(0x80000000ull + (uint64_t)off));
        }
    }
    std::printf("\n[CRASH] pilha de funcoes do jogo (topo primeiro): %s\n", stack_string(16).c_str());
    std::printf("[CRASH] profundidade da pilha sombra: %d\n", wdc_shadow_depth);
    if (const recomp_context* c = (const recomp_context*)wdc_ctx) {   // registradores MIPS da funcao em execucao (valores no ultimo ponto salvo)
        const uint64_t* r = &c->r0;
        static const char* nm[32] = { "zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
                                      "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra" };
        std::printf("[CRASH] GPRs MIPS:");
        for (int i = 1; i < 32; i++) std::printf("%s%s=%08X", (i % 8 == 1) ? "\n[CRASH]   " : " ", nm[i], (uint32_t)r[i]);
        std::printf("\n");
    }
    // WDC_CRASHDUMP=<hex addr>:<hex bytes>[,...]: despeja memoria guest (palavras big-endian) para inspecionar estruturas do jogo.
    if (const char* e = std::getenv("WDC_CRASHDUMP")) {
        const uint8_t* rd2 = wdc_get_rdram();
        for (const char* q = e; rd2 && *q;) {
            char* end = nullptr;
            const unsigned long a = std::strtoul(q, &end, 16);
            if (end == q || *end != ':') break;
            const unsigned long n = std::strtoul(end + 1, &end, 16);
            std::printf("[CRASH] dump 0x%08lX (+0x%lX):", a, n);
            for (unsigned long o = 0; o < n; o += 4) {
                if (o % 16 == 0) std::printf("\n[CRASH]   %08lX:", a + o);
                std::printf(" %08X", *(const uint32_t*)(rd2 + ((a + o) & 0x1FFFFFFF)));
            }
            std::printf("\n");
            q = (*end == ',') ? end + 1 : end;
        }
    }
    wdc_write_cov("crash");   // contadores de cobertura no momento da queda (cov_crash.txt)
    wdc_print_gfx_summary();  // o que a RSP "recebeu" ate a queda
    std::fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}
struct CrashInstaller { CrashInstaller() { SetUnhandledExceptionFilter(wdc_crash_filter); } } g_crash_installer;
}  // namespace

// Chamadas que ainda estao dentro de osRecvMesg/osSendMesg no momento do relatorio = threads bloqueadas.
void wdc_probe_report();
extern "C" void wdc_dump_blocked(void) {
    wdc_probe_report();
    std::lock_guard<std::mutex> l(g_mutex);
    std::printf("[msg] chamadas em curso (candidatas a bloqueio): %zu\n", g_blocked.size());
    const double now = wdc_now_s();
    for (const auto& [tid, b] : g_blocked) {
        std::printf("[msg]   thread %lu: %s q=%08X msg=%08X flags=%u ha %.2fs  %s\n", (unsigned long)tid, b.op, b.queue, b.msg, b.flags, now - b.since, b.stack.c_str());
    }
    std::printf("[msg] contagem por (operacao, fila):\n");
    for (const auto& [k, c] : g_counts) std::printf("[msg]   %s q=%08X : %llu\n", k.first.c_str(), k.second, (unsigned long long)c);
}
