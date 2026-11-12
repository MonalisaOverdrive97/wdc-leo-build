// Modelo de hardware do RSP/RDP para o ucode grafico do WDC recompilado (LLE).
// Incluido pelo arquivo gerado pelo RSPRecomp patchado (`extra_include` em recomp/rsp/wdc_ucode.toml), DEPOIS de
// librecomp/rsp.hpp: redefine os macros de DMA com a semantica real do hardware e fornece RSP_MFC0/RSP_MTC0
// (SP_STATUS, DPC_*, espera de DMA). A implementacao esta em wdc_rsp_hw.cpp.
#pragma once
#include <cstdint>

// O VU do runtime (librecomp/rsp_vu_impl.hpp) carrega bytes COM sinal em LUV/LHV/LFV; usamos a copia corrigida de
// recomp/runtime_shim/librecomp (tools/recomp/patch_rsp_vu.py). Falha a compilacao se o cabecalho original for pego.
#if defined(ACCH) && !defined(WDC_RSP_VU_PATCHED)
#error "rsp_vu_impl.hpp sem a correcao de LUV/LHV/LFV: ajuste a ordem dos -I (recomp/runtime_shim antes de librecomp)"
#endif

// ---- DMA do RSP (SP_RD_LEN / SP_WR_LEN) ----
// Comprimento = ((len & 0xFFF) | 7) + 1 bytes por linha, ((len >> 12) & 0xFF) + 1 linhas, salto de RDRAM = len >> 20 apos cada
// linha; enderecos alinhados a 8 (DMEM/IMEM com bit 12 = IMEM). O macro padrao do librecomp ignora linhas/salto e nao arredonda.
void wdc_rsp_dma(uint8_t* rdram, bool to_rdram, uint32_t mem_addr, uint32_t dram_addr, uint32_t len);
#undef SET_DMA_MEM
#undef SET_DMA_DRAM
#undef DO_DMA_READ
#undef DO_DMA_WRITE
#define SET_DMA_MEM(mem_addr) dma_mem_address = (mem_addr)
#define SET_DMA_DRAM(dram_addr) dma_dram_address = (dram_addr)
#define DO_DMA_READ(rd_len) wdc_rsp_dma(rdram, false, dma_mem_address, dma_dram_address, (rd_len))
#define DO_DMA_WRITE(wr_len) wdc_rsp_dma(rdram, true, dma_mem_address, dma_dram_address, (wr_len))

// ---- COP0 (patch `cop0_hooks` do RSPRecomp): numeracao de registradores do RSP ----
//  0 SP_MEM_ADDR  1 SP_DRAM_ADDR  2 SP_RD_LEN  3 SP_WR_LEN  4 SP_STATUS  5 SP_DMA_FULL  6 SP_DMA_BUSY  7 SP_SEMAPHORE
//  8 DPC_START    9 DPC_END      10 DPC_CURRENT 11 DPC_STATUS 12 DPC_CLOCK 13 DPC_BUFBUSY 14 DPC_PIPEBUSY 15 DPC_TMEM
uint32_t wdc_rsp_mfc0(uint8_t* rdram, int reg);
void wdc_rsp_mtc0(uint8_t* rdram, int reg, uint32_t value);
#define RSP_MFC0(reg) wdc_rsp_mfc0(rdram, (reg))
#define RSP_MTC0(reg, value) wdc_rsp_mtc0(rdram, (reg), (value))

// ---- contadores de execucao (label_hooks do RSPRecomp patchado): 0x111C / 0x1180 = despacho de comandos nos fluxos A / B (r28 = w0) ----
void wdc_rsp_label_hit(uint32_t addr, uint32_t w0);
#define RSP_LABEL_HIT(addr) wdc_rsp_label_hit((addr), r28)
