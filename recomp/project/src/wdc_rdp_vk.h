// Backend de RDP do projeto: ParaLLEl-RDP (Vulkan compute; thirdparty/parallel-rdp-standalone, MIT).
// Interface sem tipos do Vulkan/Granite para que o restante do projeto (que inclui headers do runtime) nao dependa deles.
#pragma once
#include <cstddef>
#include <cstdint>

#ifdef WDC_RDP_VK

// Cria contexto Vulkan sem janela, escolhe a GPU (gpu_index < 0 = primeira discreta) e importa `rdram` (ponteiro base do RDRAM do
// jogo, alinhado a 64 KiB) como buffer da GPU. Devolve false se a GPU/driver nao serve (o chamador cai no consumidor nulo).
bool wdc_rdp_vk_init(uint8_t* rdram, size_t rdram_size, int gpu_index);
bool wdc_rdp_vk_active();

// Lote de comandos RDP completos: sequencia de [n_palavras, palavra0..palavra(n-1)] (palavras no valor big-endian do RDP).
// Chamado pela thread de RSP; a thread de RDP os repassa ao ParaLLEl-RDP. A cada SYNC_FULL a thread de RDP espera a GPU e entao
// entrega OS_EVENT_DP ao jogo (wdc_post_dp).
void wdc_rdp_vk_commands(const uint32_t* batch, size_t nwords);

// Registradores de VI atuais (14 palavras na ordem de ultramodern::renderer::ViRegs = VIRegister do ParaLLEl-RDP). Marca fim de quadro.
void wdc_rdp_vk_vi(const uint32_t regs[14]);

// Modo interativo: a cada fim de quadro (VI) faz o scanout e entrega o RGBA8 (w x h, campo de 640x240 ou quadro cheio) a `fn`, na thread de RDP.
// Modo Enhanced: osViSwapBuffer avisa qual framebuffer passou a aguardar o scanout (portao de buffer; ver wdc_rdp_vk.cpp).
void wdc_rdp_vk_note_swap(uint32_t fb_addr);
// fn recebe cada quadro do scanout (RGBA8) e o instante (microssegundos, relogio de wdc_present_now_us) do retrace de VI que o gerou.
void wdc_rdp_vk_set_present(void (*fn)(const uint8_t* rgba, unsigned w, unsigned h, int64_t t_vi_us));

// Pede uma captura PNG do scanout (VI) no primeiro quadro depois de `after_seconds` desde o inicio do processo.
void wdc_rdp_vk_request_shot(const char* path, double after_seconds);
bool wdc_rdp_vk_shot_done();
// Capturas adicionais: depois da primeira, mais `extra_shots` a cada `interval` segundos (arquivo_01.png, arquivo_02.png...)
void wdc_rdp_vk_shot_sequence(int extra_shots, double interval);

// Despeja o color image do RDRAM (apos os comandos ja enfileirados terem terminado na GPU) como PNG: 16 bpp (RGBA5551) ou 32 bpp.
void wdc_rdp_vk_dump_fb(const char* path, uint32_t addr, uint32_t width, uint32_t height, uint32_t size_bits);

void wdc_rdp_vk_print_stats();
#else   // sem ParaLLEl-RDP (-DWDC_RDP_VK=OFF): o consumidor nulo de wdc_rsp_hw.cpp entrega o evento DP
static inline bool wdc_rdp_vk_init(uint8_t*, size_t, int) { return false; }
static inline bool wdc_rdp_vk_active() { return false; }
static inline void wdc_rdp_vk_commands(const uint32_t*, size_t) {}
static inline void wdc_rdp_vk_vi(const uint32_t*) {}
static inline void wdc_rdp_vk_set_present(void (*)(const uint8_t*, unsigned, unsigned, int64_t)) {}
static inline void wdc_rdp_vk_note_swap(uint32_t) {}
static inline void wdc_rdp_vk_request_shot(const char*, double) {}
static inline bool wdc_rdp_vk_shot_done() { return false; }
static inline void wdc_rdp_vk_shot_sequence(int, double) {}
static inline void wdc_rdp_vk_dump_fb(const char*, uint32_t, uint32_t, uint32_t, uint32_t) {}
static inline void wdc_rdp_vk_print_stats() {}
#endif
