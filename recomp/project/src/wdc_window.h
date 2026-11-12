// Janela Win32 do modo interativo (--play): apresenta o scanout do ParaLLEl-RDP (GDI, sem swapchain) e recebe o teclado.
// A janela roda na propria thread (laco de mensagens); o estado do teclado so existe enquanto ela tem o foco.
#pragma once
#include <cstdint>

bool wdc_window_create(const char* title, int client_w, int client_h);   // false se nao conseguiu criar
void wdc_window_destroy();
// Quadro RGBA8 (como devolvido pelo scanout; w x h pode ser um campo de 640x240: e esticado para 4:3). Seguro de qualquer thread.
void wdc_window_present(const uint8_t* rgba, unsigned w, unsigned h, int64_t t_vi_us);
bool wdc_window_closed();                 // o usuario fechou a janela ou apertou Esc
bool wdc_window_key(int vk);              // tecla (codigo VK_*) pressionada agora
void wdc_window_set_title_suffix(const char* s);   // texto extra no titulo (ex.: FPS)
