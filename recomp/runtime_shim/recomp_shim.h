// Gerado por tools/recomp/gen_runtime_shim.py -- nao editar a mao.
// Stand-in das declaracoes do runtime; so serve para validar sintaxe do C gerado.
#include "recomp.h"

#ifndef MEM_WU
#define MEM_WU(offset, reg) \
    (*(uint32_t*)(rdram + ((((reg) + (offset))) - 0xFFFFFFFF80000000)))
#endif

#ifdef __cplusplus
extern "C" {
#endif

void osPfsIsPlug_recomp(uint8_t* rdram, recomp_context* ctx);
void osPfsNumFiles_recomp(uint8_t* rdram, recomp_context* ctx);
void osPfsRepairId_recomp(uint8_t* rdram, recomp_context* ctx);
void osUnmapTLBAll_recomp(uint8_t* rdram, recomp_context* ctx);
void rmonPrintf_recomp(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
}
#endif
