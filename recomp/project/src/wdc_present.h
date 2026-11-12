// Apresentador em GPU (D3D11 + DXGI flip model): v-sync, escalonamento por shader (nearest / bilinear / sharp-bilinear) e tela cheia sem bordas.
// Roda na propria thread e NUNCA bloqueia quem entrega quadros (a thread do RDP): wdc_present_frame so copia o quadro e publica o ponteiro.
// O ritmo de apresentacao vem do monitor (waitable swapchain): cada vblank apresenta o quadro mais recente (um quadro de 60 Hz fica 2 vblanks a 120 Hz).
#pragma once
#include <cstdint>

#include "wdc_config.h"

bool wdc_present_init(void* hwnd, const WdcConfig& cfg);   // false = nao conseguiu criar o dispositivo (use o GDI)
bool wdc_present_active();                                  // true entre um init bem-sucedido e o shutdown/uma falha de dispositivo
int64_t wdc_present_now_us();   // relogio monotonico (microssegundos) usado nos instantes dos quadros
void wdc_present_frame(const uint8_t* rgba, unsigned w, unsigned h, int64_t t_vi_us);   // quadro RGBA8 (campo de 640x240 e esticado para 4:3) e o instante do retrace de VI; seguro de qualquer thread
void wdc_present_set_extra_latency_us(int64_t extra_us);   // folga a mais no buffer de jitter (o RDP segura 1 quadro para filtrar piscadas: ~16700 us)
void wdc_present_shutdown();
void wdc_present_print_stats();
