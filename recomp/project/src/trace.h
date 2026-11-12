// Macros que o N64Recomp emite com `trace_mode = true` (gerado por `gen_wdc_toml.py --trace`).
// Cada funcao recompilada conta quantas vezes foi chamada: o contador e registrado na primeira chamada
// (por nome) e o main.cpp relata as funcoes novas, formando um mapa de cobertura do jogo.
// Alem disso mantem uma pilha de chamadas por thread (wdc_shadow), usada por trace_hooks.cpp para
// dizer onde cada thread do jogo esta bloqueada (osRecvMesg/osSendMesg).
#pragma once
#include <stdint.h>

#define WDC_SHADOW_MAX 512

#ifdef __cplusplus
extern "C" {
#endif
uint32_t* wdc_cov_register(const char* name);
extern __declspec(thread) const char* wdc_shadow[WDC_SHADOW_MAX];
extern __declspec(thread) int wdc_shadow_depth;
extern __declspec(thread) void* wdc_ctx;   // recomp_context* da thread (para imprimir os registradores MIPS numa queda)
#ifdef __cplusplus
}
#endif

#define TRACE_ENTRY() { static uint32_t* wdc_cov_ctr; if (!wdc_cov_ctr) wdc_cov_ctr = wdc_cov_register(__func__); ++*wdc_cov_ctr; wdc_ctx = ctx; \
                        if (wdc_shadow_depth >= 0 && wdc_shadow_depth < WDC_SHADOW_MAX) wdc_shadow[wdc_shadow_depth] = __func__; wdc_shadow_depth++; }
#define TRACE_RETURN() { if (wdc_shadow_depth > 0) wdc_shadow_depth--; }
