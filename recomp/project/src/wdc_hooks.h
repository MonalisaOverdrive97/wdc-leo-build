// Incluido a forca (/FI) em todos os arquivos do C gerado (builds normal e trace): redireciona chamadas de libultra
// que o runtime nao trata por completo para wrappers do projeto (recomp/runtime_glue/wdc_runtime_stubs.cpp).
#pragma once
#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif
// osSetEventMesg: captura o evento OS_EVENT_SP_BREAK (11), que o ultramodern ignora, e repassa ao real.
void wdc_hook_osSetEventMesg(uint8_t* rdram, recomp_context* ctx);
// osSpTaskLoad limpa SIG0 (SP_CLR_YIELD) e osSpTaskYield o seta (SP_SET_YIELD): o ucode do WDC usa SIG0 como "pode
// executar o fluxo B" (ver wdc_runtime_stubs.cpp). O runtime ignora ambos; aqui modelamos o flag.
void wdc_hook_osSpTaskLoad(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osSpTaskYield(uint8_t* rdram, recomp_context* ctx);
// osSpTaskStartGo: no modo LLE troca o tipo da tarefa grafica para que o runtime a execute como tarefa de RSP (ucode recompilado).
void wdc_hook_osSpTaskStartGo(uint8_t* rdram, recomp_context* ctx);
// AI emulada como o hardware (wdc_audio.cpp): DMA em curso + pendente, RDRAM lida ao tocar. O runtime copia as amostras ao enfileirar e
// responde "nunca cheio", o que faz o WDC reenfileirar o anel de 16 KB a cada quadro (audio ~6x mais rapido e repetido).
void wdc_hook_osAiSetNextBuffer(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osAiGetStatus(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osAiGetLength(uint8_t* rdram, recomp_context* ctx);
// osViSetEvent: registra os argumentos (fila, mensagem, contagem de retracos) e deixa WDC_VI_RETRACE=N sobrescrever a contagem (experimento de ritmo de quadro).
void wdc_hook_osViSetEvent(uint8_t* rdram, recomp_context* ctx);
// Modo Enhanced: chaves (preenchidas por main.cpp a partir de wdc_config) que os hooks condicionais do C gerado consultam. Legacy = tudo 0 = codigo original.
extern int g_wdc_enh_fps60;
extern int g_wdc_skip_recv;
// Demo/replay a 60 Hz (Enhanced): amostras de entrada a 30 Hz (pre: devolve 1 nas iteracoes que so mantem a amostra; post: guarda o resultado).
int wdc_demo_pre(uint8_t* rdram, recomp_context* ctx, int play);
void wdc_demo_post(uint8_t* rdram, recomp_context* ctx, int play);
// Diagnostico de travamento (wdc_runtime_stubs.cpp): tabela de quem esta em osRecvMesg/osSendMesg/osJamMesg; so na build normal (a trace tem o seu rastreio).
// osViSwapBuffer: avisa o portao de buffer do RDP (Enhanced, 60 Hz); so na build normal.
void wdc_hook_osViSwapBuffer(uint8_t* rdram, recomp_context* ctx);
void wdc_blk_osRecvMesg(uint8_t* rdram, recomp_context* ctx);
void wdc_blk_osSendMesg(uint8_t* rdram, recomp_context* ctx);
void wdc_blk_osJamMesg(uint8_t* rdram, recomp_context* ctx);
void wdc_blk_dump(void);
// Controller Pak virtual (wdc_pak.cpp): o runtime responde PFS_ERR_NOPACK a tudo.
void wdc_hook_osPfsInitPak(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsRepairId(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsFileState(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsFindFile(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsAllocateFile(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsDeleteFile(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsReadWriteFile(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsFreeBlocks(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsNumFiles(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsChecker(uint8_t* rdram, recomp_context* ctx);
#ifdef __cplusplus
}
#endif

#ifndef WDC_TRACE_BUILD
#define osViSwapBuffer_recomp wdc_hook_osViSwapBuffer
#define osRecvMesg_recomp wdc_blk_osRecvMesg
#define osSendMesg_recomp wdc_blk_osSendMesg
#define osJamMesg_recomp  wdc_blk_osJamMesg
#endif
#define osViSetEvent_recomp wdc_hook_osViSetEvent
#define osAiSetNextBuffer_recomp wdc_hook_osAiSetNextBuffer
#define osAiGetStatus_recomp     wdc_hook_osAiGetStatus
#define osAiGetLength_recomp     wdc_hook_osAiGetLength
#define osSetEventMesg_recomp wdc_hook_osSetEventMesg
#define osSpTaskLoad_recomp   wdc_hook_osSpTaskLoad
#define osSpTaskYield_recomp  wdc_hook_osSpTaskYield
#define osSpTaskStartGo_recomp wdc_hook_osSpTaskStartGo
#define osPfsInitPak_recomp wdc_hook_osPfsInitPak
#define osPfsRepairId_recomp wdc_hook_osPfsRepairId
#define osPfsFileState_recomp wdc_hook_osPfsFileState
#define osPfsFindFile_recomp wdc_hook_osPfsFindFile
#define osPfsAllocateFile_recomp wdc_hook_osPfsAllocateFile
#define osPfsDeleteFile_recomp wdc_hook_osPfsDeleteFile
#define osPfsReadWriteFile_recomp wdc_hook_osPfsReadWriteFile
#define osPfsFreeBlocks_recomp wdc_hook_osPfsFreeBlocks
#define osPfsNumFiles_recomp wdc_hook_osPfsNumFiles
#define osPfsChecker_recomp wdc_hook_osPfsChecker
