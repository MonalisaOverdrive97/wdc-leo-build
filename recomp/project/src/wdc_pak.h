// Controller Pak virtual do projeto: imagem de 32 KB em arquivo + as funcoes osPfs* da libultra (wdc_pfs.{h,cpp}).
// O ultramodern/librecomp respondem "sem pak" (PFS_ERR_NOPACK) a todas elas e so conhecem o Rumble Pak; as chamadas do C gerado sao
// redirecionadas para os hooks abaixo (wdc_hooks.h).
#pragma once
#include <cstdint>
#include "recomp.h"

// Liga o pak virtual na porta 0 com a imagem em `path` (cria um pak de fabrica se nao existir ou tiver tamanho errado). false se falhou.
bool wdc_pak_init(const char* path);
bool wdc_pak_enabled();
void wdc_pak_flush();    // grava a imagem se mudou desde a ultima gravacao

#ifdef __cplusplus
extern "C" {
#endif
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
