// Incluido a forca (/FI) so nos arquivos do C gerado da build trace: redireciona as trocas de mensagens do libultra
// para wrappers que registram a fila, o chamador (pilha de chamadas) e quem esta bloqueado.
#pragma once
#include "recomp.h"
#include "wdc_hooks.h"

#ifdef __cplusplus
extern "C" {
#endif
void wdc_hook_osRecvMesg(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osSendMesg(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osJamMesg(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osCreateThread(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osStartThread(uint8_t* rdram, recomp_context* ctx);
void wdc_dump_blocked(void);
// Sondas de diagnostico (hooks gerados por gen_wdc_toml.py --trace; ver TRACE_PROBES)
int wdc_exp(int n);                                              // env WDC_EXP="1,2": liga o experimento n
void wdc_probe_poly(uint8_t* rdram, recomp_context* ctx, int site);
#ifdef __cplusplus
}
#endif

#define osRecvMesg_recomp wdc_hook_osRecvMesg
#define osSendMesg_recomp wdc_hook_osSendMesg
#define osJamMesg_recomp  wdc_hook_osJamMesg
#define osCreateThread_recomp wdc_hook_osCreateThread
#define osStartThread_recomp  wdc_hook_osStartThread

// Fronteiras de segmento do diferencial de CPU (gerado por tools/recomp/gen_seg_hooks.py): TODAS as chamadas de funcoes do runtime.
#include "wdc_seg_hooks.h"
