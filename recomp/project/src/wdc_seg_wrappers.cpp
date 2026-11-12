// GERADO por tools/recomp/gen_seg_hooks.py -- nao edite. NAO inclui wdc_seg_hooks.h: chama as funcoes reais.
#include "recomp.h"
#include <cstddef>
extern "C" {
void wdc_seg_call(uint8_t* rdram, recomp_context* ctx, int id, void (*real)(uint8_t*, recomp_context*));
void __ll_div_recomp(uint8_t* rdram, recomp_context* ctx);
void __ll_mul_recomp(uint8_t* rdram, recomp_context* ctx);
void __osDisableInt_recomp(uint8_t* rdram, recomp_context* ctx);
void __osRestoreInt_recomp(uint8_t* rdram, recomp_context* ctx);
void __osSetFpcCsr_recomp(uint8_t* rdram, recomp_context* ctx);
void __osSpSetPc_recomp(uint8_t* rdram, recomp_context* ctx);
void __ull_div_recomp(uint8_t* rdram, recomp_context* ctx);
void __ull_rem_recomp(uint8_t* rdram, recomp_context* ctx);
void __ull_to_d_recomp(uint8_t* rdram, recomp_context* ctx);
void __ull_to_f_recomp(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osAiGetLength(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osAiGetStatus(uint8_t* rdram, recomp_context* ctx);
void osAiSetFrequency_recomp(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osAiSetNextBuffer(uint8_t* rdram, recomp_context* ctx);
void osCartRomInit_recomp(uint8_t* rdram, recomp_context* ctx);
void osContGetQuery_recomp(uint8_t* rdram, recomp_context* ctx);
void osContGetReadData_recomp(uint8_t* rdram, recomp_context* ctx);
void osContInit_recomp(uint8_t* rdram, recomp_context* ctx);
void osContStartQuery_recomp(uint8_t* rdram, recomp_context* ctx);
void osContStartReadData_recomp(uint8_t* rdram, recomp_context* ctx);
void osCreateMesgQueue_recomp(uint8_t* rdram, recomp_context* ctx);
void osCreatePiManager_recomp(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osCreateThread(uint8_t* rdram, recomp_context* ctx);
void osCreateViManager_recomp(uint8_t* rdram, recomp_context* ctx);
void osDestroyThread_recomp(uint8_t* rdram, recomp_context* ctx);
void osEPiRawStartDma_recomp(uint8_t* rdram, recomp_context* ctx);
void osGetCount_recomp(uint8_t* rdram, recomp_context* ctx);
void osGetThreadPri_recomp(uint8_t* rdram, recomp_context* ctx);
void osGetTime_recomp(uint8_t* rdram, recomp_context* ctx);
void osInitialize_recomp(uint8_t* rdram, recomp_context* ctx);
void osInvalDCache_recomp(uint8_t* rdram, recomp_context* ctx);
void osInvalICache_recomp(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osJamMesg(uint8_t* rdram, recomp_context* ctx);
void osMotorInit_recomp(uint8_t* rdram, recomp_context* ctx);
void osMotorStart_recomp(uint8_t* rdram, recomp_context* ctx);
void osMotorStop_recomp(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsAllocateFile(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsDeleteFile(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsFileState(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsFindFile(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsFreeBlocks(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsInitPak(uint8_t* rdram, recomp_context* ctx);
void osPfsIsPlug_recomp(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsNumFiles(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsReadWriteFile(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osPfsRepairId(uint8_t* rdram, recomp_context* ctx);
void osPiReadIo_recomp(uint8_t* rdram, recomp_context* ctx);
void osPiStartDma_recomp(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osRecvMesg(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osSendMesg(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osSetEventMesg(uint8_t* rdram, recomp_context* ctx);
void osSetIntMask_recomp(uint8_t* rdram, recomp_context* ctx);
void osSetThreadPri_recomp(uint8_t* rdram, recomp_context* ctx);
void osSetTimer_recomp(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osSpTaskLoad(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osSpTaskStartGo(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osSpTaskYield(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osStartThread(uint8_t* rdram, recomp_context* ctx);
void osStopThread_recomp(uint8_t* rdram, recomp_context* ctx);
void osUnmapTLBAll_recomp(uint8_t* rdram, recomp_context* ctx);
void osViBlack_recomp(uint8_t* rdram, recomp_context* ctx);
void osViGetCurrentFramebuffer_recomp(uint8_t* rdram, recomp_context* ctx);
void osViSetEvent_recomp(uint8_t* rdram, recomp_context* ctx);
void osViSetMode_recomp(uint8_t* rdram, recomp_context* ctx);
void osViSetSpecialFeatures_recomp(uint8_t* rdram, recomp_context* ctx);
void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx);
void osVirtualToPhysical_recomp(uint8_t* rdram, recomp_context* ctx);
void osWritebackDCache_recomp(uint8_t* rdram, recomp_context* ctx);
void osWritebackDCacheAll_recomp(uint8_t* rdram, recomp_context* ctx);
void rmonPrintf_recomp(uint8_t* rdram, recomp_context* ctx);
}
extern "C" void wdc_seg___ll_div(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 0, __ll_div_recomp); }
extern "C" void wdc_seg___ll_mul(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 1, __ll_mul_recomp); }
extern "C" void wdc_seg___osDisableInt(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 2, __osDisableInt_recomp); }
extern "C" void wdc_seg___osRestoreInt(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 3, __osRestoreInt_recomp); }
extern "C" void wdc_seg___osSetFpcCsr(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 4, __osSetFpcCsr_recomp); }
extern "C" void wdc_seg___osSpSetPc(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 5, __osSpSetPc_recomp); }
extern "C" void wdc_seg___ull_div(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 6, __ull_div_recomp); }
extern "C" void wdc_seg___ull_rem(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 7, __ull_rem_recomp); }
extern "C" void wdc_seg___ull_to_d(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 8, __ull_to_d_recomp); }
extern "C" void wdc_seg___ull_to_f(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 9, __ull_to_f_recomp); }
extern "C" void wdc_seg_osAiGetLength(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 10, wdc_hook_osAiGetLength); }
extern "C" void wdc_seg_osAiGetStatus(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 11, wdc_hook_osAiGetStatus); }
extern "C" void wdc_seg_osAiSetFrequency(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 12, osAiSetFrequency_recomp); }
extern "C" void wdc_seg_osAiSetNextBuffer(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 13, wdc_hook_osAiSetNextBuffer); }
extern "C" void wdc_seg_osCartRomInit(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 14, osCartRomInit_recomp); }
extern "C" void wdc_seg_osContGetQuery(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 15, osContGetQuery_recomp); }
extern "C" void wdc_seg_osContGetReadData(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 16, osContGetReadData_recomp); }
extern "C" void wdc_seg_osContInit(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 17, osContInit_recomp); }
extern "C" void wdc_seg_osContStartQuery(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 18, osContStartQuery_recomp); }
extern "C" void wdc_seg_osContStartReadData(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 19, osContStartReadData_recomp); }
extern "C" void wdc_seg_osCreateMesgQueue(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 20, osCreateMesgQueue_recomp); }
extern "C" void wdc_seg_osCreatePiManager(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 21, osCreatePiManager_recomp); }
extern "C" void wdc_seg_osCreateThread(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 22, wdc_hook_osCreateThread); }
extern "C" void wdc_seg_osCreateViManager(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 23, osCreateViManager_recomp); }
extern "C" void wdc_seg_osDestroyThread(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 24, osDestroyThread_recomp); }
extern "C" void wdc_seg_osEPiRawStartDma(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 25, osEPiRawStartDma_recomp); }
extern "C" void wdc_seg_osGetCount(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 26, osGetCount_recomp); }
extern "C" void wdc_seg_osGetThreadPri(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 27, osGetThreadPri_recomp); }
extern "C" void wdc_seg_osGetTime(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 28, osGetTime_recomp); }
extern "C" void wdc_seg_osInitialize(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 29, osInitialize_recomp); }
extern "C" void wdc_seg_osInvalDCache(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 30, osInvalDCache_recomp); }
extern "C" void wdc_seg_osInvalICache(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 31, osInvalICache_recomp); }
extern "C" void wdc_seg_osJamMesg(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 32, wdc_hook_osJamMesg); }
extern "C" void wdc_seg_osMotorInit(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 33, osMotorInit_recomp); }
extern "C" void wdc_seg_osMotorStart(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 34, osMotorStart_recomp); }
extern "C" void wdc_seg_osMotorStop(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 35, osMotorStop_recomp); }
extern "C" void wdc_seg_osPfsAllocateFile(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 36, wdc_hook_osPfsAllocateFile); }
extern "C" void wdc_seg_osPfsDeleteFile(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 37, wdc_hook_osPfsDeleteFile); }
extern "C" void wdc_seg_osPfsFileState(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 38, wdc_hook_osPfsFileState); }
extern "C" void wdc_seg_osPfsFindFile(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 39, wdc_hook_osPfsFindFile); }
extern "C" void wdc_seg_osPfsFreeBlocks(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 40, wdc_hook_osPfsFreeBlocks); }
extern "C" void wdc_seg_osPfsInitPak(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 41, wdc_hook_osPfsInitPak); }
extern "C" void wdc_seg_osPfsIsPlug(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 42, osPfsIsPlug_recomp); }
extern "C" void wdc_seg_osPfsNumFiles(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 43, wdc_hook_osPfsNumFiles); }
extern "C" void wdc_seg_osPfsReadWriteFile(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 44, wdc_hook_osPfsReadWriteFile); }
extern "C" void wdc_seg_osPfsRepairId(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 45, wdc_hook_osPfsRepairId); }
extern "C" void wdc_seg_osPiReadIo(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 46, osPiReadIo_recomp); }
extern "C" void wdc_seg_osPiStartDma(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 47, osPiStartDma_recomp); }
extern "C" void wdc_seg_osRecvMesg(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 48, wdc_hook_osRecvMesg); }
extern "C" void wdc_seg_osSendMesg(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 49, wdc_hook_osSendMesg); }
extern "C" void wdc_seg_osSetEventMesg(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 50, wdc_hook_osSetEventMesg); }
extern "C" void wdc_seg_osSetIntMask(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 51, osSetIntMask_recomp); }
extern "C" void wdc_seg_osSetThreadPri(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 52, osSetThreadPri_recomp); }
extern "C" void wdc_seg_osSetTimer(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 53, osSetTimer_recomp); }
extern "C" void wdc_seg_osSpTaskLoad(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 54, wdc_hook_osSpTaskLoad); }
extern "C" void wdc_seg_osSpTaskStartGo(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 55, wdc_hook_osSpTaskStartGo); }
extern "C" void wdc_seg_osSpTaskYield(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 56, wdc_hook_osSpTaskYield); }
extern "C" void wdc_seg_osStartThread(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 57, wdc_hook_osStartThread); }
extern "C" void wdc_seg_osStopThread(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 58, osStopThread_recomp); }
extern "C" void wdc_seg_osUnmapTLBAll(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 59, osUnmapTLBAll_recomp); }
extern "C" void wdc_seg_osViBlack(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 60, osViBlack_recomp); }
extern "C" void wdc_seg_osViGetCurrentFramebuffer(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 61, osViGetCurrentFramebuffer_recomp); }
extern "C" void wdc_seg_osViSetEvent(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 62, osViSetEvent_recomp); }
extern "C" void wdc_seg_osViSetMode(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 63, osViSetMode_recomp); }
extern "C" void wdc_seg_osViSetSpecialFeatures(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 64, osViSetSpecialFeatures_recomp); }
extern "C" void wdc_seg_osViSwapBuffer(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 65, osViSwapBuffer_recomp); }
extern "C" void wdc_seg_osVirtualToPhysical(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 66, osVirtualToPhysical_recomp); }
extern "C" void wdc_seg_osWritebackDCache(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 67, osWritebackDCache_recomp); }
extern "C" void wdc_seg_osWritebackDCacheAll(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 68, osWritebackDCacheAll_recomp); }
extern "C" void wdc_seg_rmonPrintf(uint8_t* rdram, recomp_context* ctx) { wdc_seg_call(rdram, ctx, 69, rmonPrintf_recomp); }
struct WdcSegFn { const char* name; unsigned vram; };
extern "C" const WdcSegFn wdc_seg_table[] = {
    {"__ll_div", 0x80071A4Cu},
    {"__ll_mul", 0x80071AA8u},
    {"__osDisableInt", 0x80074EB0u},
    {"__osRestoreInt", 0x80074ED0u},
    {"__osSetFpcCsr", 0x80076FF0u},
    {"__osSpSetPc", 0x800778A0u},
    {"__ull_div", 0x800719A8u},
    {"__ull_rem", 0x8007196Cu},
    {"__ull_to_d", 0x80071DA4u},
    {"__ull_to_f", 0x80071DD8u},
    {"osAiGetLength", 0x80074EA0u},
    {"osAiGetStatus", 0x80074E90u},
    {"osAiSetFrequency", 0x80074D30u},
    {"osAiSetNextBuffer", 0x80074C80u},
    {"osCartRomInit", 0x80076570u},
    {"osContGetQuery", 0x80073F54u},
    {"osContGetReadData", 0x8006F04Cu},
    {"osContInit", 0x8006F970u},
    {"osContStartQuery", 0x80073ED0u},
    {"osContStartReadData", 0x8006EFC0u},
    {"osCreateMesgQueue", 0x8006EEB0u},
    {"osCreatePiManager", 0x800701D0u},
    {"osCreateThread", 0x8006F6D0u},
    {"osCreateViManager", 0x8006FE70u},
    {"osDestroyThread", 0x80073F80u},
    {"osEPiRawStartDma", 0x80076910u},
    {"osGetCount", 0x80071930u},
    {"osGetThreadPri", 0x800761E0u},
    {"osGetTime", 0x80075AF0u},
    {"osInitialize", 0x80070440u},
    {"osInvalDCache", 0x8006FCB0u},
    {"osInvalICache", 0x800741D0u},
    {"osJamMesg", 0x80075C60u},
    {"osMotorInit", 0x80073BECu},
    {"osMotorStart", 0x800738D0u},
    {"osMotorStop", 0x80073730u},
    {"osPfsAllocateFile", 0x80072800u},
    {"osPfsDeleteFile", 0x800742D0u},
    {"osPfsFileState", 0x800720B0u},
    {"osPfsFindFile", 0x800723A0u},
    {"osPfsFreeBlocks", 0x80072560u},
    {"osPfsInitPak", 0x80071E10u},
    {"osPfsIsPlug", 0x800748F0u},
    {"osPfsNumFiles", 0x800726B0u},
    {"osPfsReadWriteFile", 0x800730ACu},
    {"osPfsRepairId", 0x800734D0u},
    {"osPiReadIo", 0x80074C40u},
    {"osPiStartDma", 0x8006FD60u},
    {"osRecvMesg", 0x8006ED70u},
    {"osSendMesg", 0x8006F210u},
    {"osSetEventMesg", 0x8006EEE0u},
    {"osSetIntMask", 0x8007B150u},
    {"osSetThreadPri", 0x80070360u},
    {"osSetTimer", 0x80075B80u},
    {"osSpTaskLoad", 0x8007175Cu},
    {"osSpTaskStartGo", 0x800718ECu},
    {"osSpTaskYield", 0x80071620u},
    {"osStartThread", 0x8006F820u},
    {"osStopThread", 0x80074090u},
    {"osUnmapTLBAll", 0x800706D0u},
    {"osViBlack", 0x8006EF50u},
    {"osViGetCurrentFramebuffer", 0x8006F5E0u},
    {"osViSetEvent", 0x8006F660u},
    {"osViSetMode", 0x8006F360u},
    {"osViSetSpecialFeatures", 0x8006F3D0u},
    {"osViSwapBuffer", 0x8006F590u},
    {"osVirtualToPhysical", 0x80077820u},
    {"osWritebackDCache", 0x80074150u},
    {"osWritebackDCacheAll", 0x8006F1E0u},
    {"rmonPrintf", 0x8006F63Cu},
};
extern "C" const int wdc_seg_table_n = 70;
