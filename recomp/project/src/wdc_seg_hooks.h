// GERADO por tools/recomp/gen_seg_hooks.py -- nao edite.
// Incluido no fim de trace_hooks.h: redireciona as chamadas de funcoes do runtime para wrappers de segmento (trace_seg.cpp).
#pragma once
#include "recomp.h"
#ifdef __cplusplus
extern "C" {
#endif
void wdc_seg___ll_div(uint8_t* rdram, recomp_context* ctx);
void wdc_seg___ll_mul(uint8_t* rdram, recomp_context* ctx);
void wdc_seg___osDisableInt(uint8_t* rdram, recomp_context* ctx);
void wdc_seg___osRestoreInt(uint8_t* rdram, recomp_context* ctx);
void wdc_seg___osSetFpcCsr(uint8_t* rdram, recomp_context* ctx);
void wdc_seg___osSpSetPc(uint8_t* rdram, recomp_context* ctx);
void wdc_seg___ull_div(uint8_t* rdram, recomp_context* ctx);
void wdc_seg___ull_rem(uint8_t* rdram, recomp_context* ctx);
void wdc_seg___ull_to_d(uint8_t* rdram, recomp_context* ctx);
void wdc_seg___ull_to_f(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osAiGetLength(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osAiGetStatus(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osAiSetFrequency(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osAiSetNextBuffer(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osCartRomInit(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osContGetQuery(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osContGetReadData(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osContInit(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osContStartQuery(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osContStartReadData(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osCreateMesgQueue(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osCreatePiManager(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osCreateThread(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osCreateViManager(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osDestroyThread(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osEPiRawStartDma(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osGetCount(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osGetThreadPri(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osGetTime(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osInitialize(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osInvalDCache(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osInvalICache(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osJamMesg(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osMotorInit(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osMotorStart(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osMotorStop(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osPfsAllocateFile(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osPfsDeleteFile(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osPfsFileState(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osPfsFindFile(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osPfsFreeBlocks(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osPfsInitPak(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osPfsIsPlug(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osPfsNumFiles(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osPfsReadWriteFile(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osPfsRepairId(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osPiReadIo(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osPiStartDma(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osRecvMesg(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osSendMesg(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osSetEventMesg(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osSetIntMask(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osSetThreadPri(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osSetTimer(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osSpTaskLoad(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osSpTaskStartGo(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osSpTaskYield(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osStartThread(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osStopThread(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osUnmapTLBAll(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osViBlack(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osViGetCurrentFramebuffer(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osViSetEvent(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osViSetMode(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osViSetSpecialFeatures(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osViSwapBuffer(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osVirtualToPhysical(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osWritebackDCache(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_osWritebackDCacheAll(uint8_t* rdram, recomp_context* ctx);
void wdc_seg_rmonPrintf(uint8_t* rdram, recomp_context* ctx);
#ifdef __cplusplus
}
#endif
#undef __ll_div_recomp
#define __ll_div_recomp wdc_seg___ll_div
#undef __ll_mul_recomp
#define __ll_mul_recomp wdc_seg___ll_mul
#undef __osDisableInt_recomp
#define __osDisableInt_recomp wdc_seg___osDisableInt
#undef __osRestoreInt_recomp
#define __osRestoreInt_recomp wdc_seg___osRestoreInt
#undef __osSetFpcCsr_recomp
#define __osSetFpcCsr_recomp wdc_seg___osSetFpcCsr
#undef __osSpSetPc_recomp
#define __osSpSetPc_recomp wdc_seg___osSpSetPc
#undef __ull_div_recomp
#define __ull_div_recomp wdc_seg___ull_div
#undef __ull_rem_recomp
#define __ull_rem_recomp wdc_seg___ull_rem
#undef __ull_to_d_recomp
#define __ull_to_d_recomp wdc_seg___ull_to_d
#undef __ull_to_f_recomp
#define __ull_to_f_recomp wdc_seg___ull_to_f
#undef osAiGetLength_recomp
#define osAiGetLength_recomp wdc_seg_osAiGetLength
#undef osAiGetStatus_recomp
#define osAiGetStatus_recomp wdc_seg_osAiGetStatus
#undef osAiSetFrequency_recomp
#define osAiSetFrequency_recomp wdc_seg_osAiSetFrequency
#undef osAiSetNextBuffer_recomp
#define osAiSetNextBuffer_recomp wdc_seg_osAiSetNextBuffer
#undef osCartRomInit_recomp
#define osCartRomInit_recomp wdc_seg_osCartRomInit
#undef osContGetQuery_recomp
#define osContGetQuery_recomp wdc_seg_osContGetQuery
#undef osContGetReadData_recomp
#define osContGetReadData_recomp wdc_seg_osContGetReadData
#undef osContInit_recomp
#define osContInit_recomp wdc_seg_osContInit
#undef osContStartQuery_recomp
#define osContStartQuery_recomp wdc_seg_osContStartQuery
#undef osContStartReadData_recomp
#define osContStartReadData_recomp wdc_seg_osContStartReadData
#undef osCreateMesgQueue_recomp
#define osCreateMesgQueue_recomp wdc_seg_osCreateMesgQueue
#undef osCreatePiManager_recomp
#define osCreatePiManager_recomp wdc_seg_osCreatePiManager
#undef osCreateThread_recomp
#define osCreateThread_recomp wdc_seg_osCreateThread
#undef osCreateViManager_recomp
#define osCreateViManager_recomp wdc_seg_osCreateViManager
#undef osDestroyThread_recomp
#define osDestroyThread_recomp wdc_seg_osDestroyThread
#undef osEPiRawStartDma_recomp
#define osEPiRawStartDma_recomp wdc_seg_osEPiRawStartDma
#undef osGetCount_recomp
#define osGetCount_recomp wdc_seg_osGetCount
#undef osGetThreadPri_recomp
#define osGetThreadPri_recomp wdc_seg_osGetThreadPri
#undef osGetTime_recomp
#define osGetTime_recomp wdc_seg_osGetTime
#undef osInitialize_recomp
#define osInitialize_recomp wdc_seg_osInitialize
#undef osInvalDCache_recomp
#define osInvalDCache_recomp wdc_seg_osInvalDCache
#undef osInvalICache_recomp
#define osInvalICache_recomp wdc_seg_osInvalICache
#undef osJamMesg_recomp
#define osJamMesg_recomp wdc_seg_osJamMesg
#undef osMotorInit_recomp
#define osMotorInit_recomp wdc_seg_osMotorInit
#undef osMotorStart_recomp
#define osMotorStart_recomp wdc_seg_osMotorStart
#undef osMotorStop_recomp
#define osMotorStop_recomp wdc_seg_osMotorStop
#undef osPfsAllocateFile_recomp
#define osPfsAllocateFile_recomp wdc_seg_osPfsAllocateFile
#undef osPfsDeleteFile_recomp
#define osPfsDeleteFile_recomp wdc_seg_osPfsDeleteFile
#undef osPfsFileState_recomp
#define osPfsFileState_recomp wdc_seg_osPfsFileState
#undef osPfsFindFile_recomp
#define osPfsFindFile_recomp wdc_seg_osPfsFindFile
#undef osPfsFreeBlocks_recomp
#define osPfsFreeBlocks_recomp wdc_seg_osPfsFreeBlocks
#undef osPfsInitPak_recomp
#define osPfsInitPak_recomp wdc_seg_osPfsInitPak
#undef osPfsIsPlug_recomp
#define osPfsIsPlug_recomp wdc_seg_osPfsIsPlug
#undef osPfsNumFiles_recomp
#define osPfsNumFiles_recomp wdc_seg_osPfsNumFiles
#undef osPfsReadWriteFile_recomp
#define osPfsReadWriteFile_recomp wdc_seg_osPfsReadWriteFile
#undef osPfsRepairId_recomp
#define osPfsRepairId_recomp wdc_seg_osPfsRepairId
#undef osPiReadIo_recomp
#define osPiReadIo_recomp wdc_seg_osPiReadIo
#undef osPiStartDma_recomp
#define osPiStartDma_recomp wdc_seg_osPiStartDma
#undef osRecvMesg_recomp
#define osRecvMesg_recomp wdc_seg_osRecvMesg
#undef osSendMesg_recomp
#define osSendMesg_recomp wdc_seg_osSendMesg
#undef osSetEventMesg_recomp
#define osSetEventMesg_recomp wdc_seg_osSetEventMesg
#undef osSetIntMask_recomp
#define osSetIntMask_recomp wdc_seg_osSetIntMask
#undef osSetThreadPri_recomp
#define osSetThreadPri_recomp wdc_seg_osSetThreadPri
#undef osSetTimer_recomp
#define osSetTimer_recomp wdc_seg_osSetTimer
#undef osSpTaskLoad_recomp
#define osSpTaskLoad_recomp wdc_seg_osSpTaskLoad
#undef osSpTaskStartGo_recomp
#define osSpTaskStartGo_recomp wdc_seg_osSpTaskStartGo
#undef osSpTaskYield_recomp
#define osSpTaskYield_recomp wdc_seg_osSpTaskYield
#undef osStartThread_recomp
#define osStartThread_recomp wdc_seg_osStartThread
#undef osStopThread_recomp
#define osStopThread_recomp wdc_seg_osStopThread
#undef osUnmapTLBAll_recomp
#define osUnmapTLBAll_recomp wdc_seg_osUnmapTLBAll
#undef osViBlack_recomp
#define osViBlack_recomp wdc_seg_osViBlack
#undef osViGetCurrentFramebuffer_recomp
#define osViGetCurrentFramebuffer_recomp wdc_seg_osViGetCurrentFramebuffer
#undef osViSetEvent_recomp
#define osViSetEvent_recomp wdc_seg_osViSetEvent
#undef osViSetMode_recomp
#define osViSetMode_recomp wdc_seg_osViSetMode
#undef osViSetSpecialFeatures_recomp
#define osViSetSpecialFeatures_recomp wdc_seg_osViSetSpecialFeatures
#undef osViSwapBuffer_recomp
#define osViSwapBuffer_recomp wdc_seg_osViSwapBuffer
#undef osVirtualToPhysical_recomp
#define osVirtualToPhysical_recomp wdc_seg_osVirtualToPhysical
#undef osWritebackDCache_recomp
#define osWritebackDCache_recomp wdc_seg_osWritebackDCache
#undef osWritebackDCacheAll_recomp
#define osWritebackDCacheAll_recomp wdc_seg_osWritebackDCacheAll
#undef rmonPrintf_recomp
#define rmonPrintf_recomp wdc_seg_rmonPrintf
