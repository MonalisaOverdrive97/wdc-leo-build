// Audio do WDC: emulacao da AI (interface de audio) + saida real (WASAPI, modo compartilhado), captura WAV (--wav) e estatisticas.
//
// O WDC mistura o audio na CPU (sem tarefa de audio no RSP) num anel circular de 16 KB (0x803F0400): chama osAiSetNextBuffer com o ANEL
// INTEIRO sempre que osAiGetStatus mostra espaco e escreve novas amostras logo ATRAS da posicao de reproducao (calculada de osAiGetLength).
// Isso so funciona se a AI se comportar como o hardware: um DMA em curso + um pendente (status "cheio"), com a RDRAM lida NO MOMENTO de
// tocar, e osAiGetLength = bytes que faltam do DMA em curso. O ultramodern copia as amostras ao enfileirar e responde "nunca cheio";
// por isso o projeto toma osAiSetNextBuffer/osAiGetStatus/osAiGetLength (wdc_hooks.h) e le a RDRAM sob demanda.
#pragma once
#include <cstddef>
#include <cstdint>
#include "recomp.h"

void wdc_audio_init(const char* wav_path, bool real_output);
void wdc_audio_shutdown();                       // para o dispositivo, grava o WAV, imprime estatisticas
void wdc_audio_set_frequency(uint32_t hz);       // callback do ultramodern (osAiSetFrequency)
// callbacks do ultramodern que nao usamos (a AI e emulada aqui): no-ops
void wdc_audio_queue_samples(int16_t*, size_t);
size_t wdc_audio_frames_remaining();

#ifdef __cplusplus
extern "C" {
#endif
void wdc_hook_osAiSetNextBuffer(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osAiGetStatus(uint8_t* rdram, recomp_context* ctx);
void wdc_hook_osAiGetLength(uint8_t* rdram, recomp_context* ctx);
#ifdef __cplusplus
}
#endif
