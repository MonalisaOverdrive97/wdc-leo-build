// Audio do WDC (ver wdc_audio.h): emulacao da AI + saida WASAPI + captura WAV.
//
// Formato: o jogo grava quadros estereo (L, R) big-endian; a RDRAM do runtime guarda cada palavra de 32 bits em ordem do host, entao a
// palavra lida de um quadro e (L << 16) | R. A AI le a RDRAM no momento de tocar (o jogo reescreve o anel logo atras da posicao de
// reproducao). Saida: a thread do dispositivo (WASAPI, shared, event-driven) converte da taxa do jogo (22 kHz) para o formato de
// mixagem do Windows com interpolacao linear. Sem dispositivo (ou --no-audio-out) o tempo real avanca a reproducao.
#include "wdc_audio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

extern double wdc_now_s();

namespace {
using Clock = std::chrono::steady_clock;

std::mutex g_mu;
// AI: um DMA em curso (cur) e um pendente (pend); a RDRAM e lida quando o quadro e tocado.
struct Desc { const uint8_t* p = nullptr; uint32_t frames = 0; };
Desc g_cur, g_pend;
bool g_cur_valid = false, g_pend_valid = false;
uint32_t g_pos = 0;                          // quadro atual dentro de g_cur
// ATRASO DE LEITURA. O jogo escreve o anel de 16 KB logo a frente da posicao de reproducao (a tarefa de audio do RSP roda na mesma CPU que a de video) e a AI emulada le a
// RDRAM no instante em que o quadro toca. Se a tarefa de audio atrasa (CPU ocupada: 60 fps no modo Enhanced, GPU, antivirus...), a AI toca dados VELHOS do anel (da volta
// anterior): estalos de poucas amostras e, com o motor no talo, "estouros". Para dar folga, a reproducao le os quadros g_lag_frames DEPOIS do instante nominal, sem mudar o
// que o jogo ve (posicao, status e comprimento do DMA seguem o relogio nominal). A folga maxima segura e o anel menos a antecedencia com que o jogo escreve (~120 ms).
struct Seg { const uint8_t* p; uint32_t frames; uint64_t start; };
std::deque<Seg> g_segs;                      // DMAs ja iniciados (mais recente no fim), com a posicao absoluta (em quadros) em que comecam
uint64_t g_stream = 0;                       // quadros consumidos no total (posicao nominal da AI)
uint32_t g_lag_frames = 0;                   // atraso de leitura em quadros (na taxa do jogo)
double g_lag_ms = 0.0;                       // pedido (ms); convertido em quadros quando a taxa e conhecida
double g_frac = 0.0;                         // posicao fracionaria entre g_pos e g_pos+1 (resample para o dispositivo)
std::atomic<uint32_t> g_rate{32000};

// modelo nulo (sem dispositivo): o tempo real avanca a reproducao
bool g_real = false;
double g_null_acc = 0.0;
Clock::time_point g_null_last = Clock::now();

// estatisticas
uint64_t g_calls = 0, g_frames_in = 0, g_frames_played = 0, g_underruns = 0, g_rejected = 0, g_nonzero = 0, g_status_calls = 0;
int g_peak = 0;
double g_sumsq = 0.0;
double g_first_t = -1, g_last_t = 0;

// captura WAV (o que a AI tocou, na taxa do jogo)
std::string g_wav_path;
std::vector<int16_t> g_wav;

std::thread g_dev_thread;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_dev_ok{false};

inline void frame_at(const Desc& d, uint32_t k, float& L, float& R, int& li, int& ri) {
    const uint32_t w = *(const uint32_t*)(d.p + (size_t)k * 4);
    li = (int16_t)(w >> 16);
    ri = (int16_t)(w & 0xFFFF);
    L = (float)li;
    R = (float)ri;
}

// Quadro na posicao absoluta r do fluxo (com o atraso de leitura); silencio antes do primeiro DMA. Chamado com g_mu travado.
void stream_frame(int64_t r, float& L, float& R, int& li, int& ri) {
    L = R = 0.f; li = ri = 0;
    if (r < 0) return;
    for (auto it = g_segs.rbegin(); it != g_segs.rend(); ++it)
        if ((uint64_t)r >= it->start) {
            if ((uint64_t)r < it->start + it->frames) frame_at(Desc{it->p, it->frames}, (uint32_t)((uint64_t)r - it->start), L, R, li, ri);
            return;
        }
}
void trim_segs() {
    const uint64_t keep_from = g_stream > g_lag_frames + 2 ? g_stream - g_lag_frames - 2 : 0;
    while (g_segs.size() > 2 && g_segs.front().start + g_segs.front().frames < keep_from) g_segs.pop_front();
}

// Consome um quadro do DMA em curso (WAV e estatisticas); ao fim do DMA o pendente assume. Chamado com g_mu travado.
void consume_one_locked() {
    float L, R;
    int li, ri;
    stream_frame((int64_t)g_stream - (int64_t)g_lag_frames, L, R, li, ri);
    g_peak = std::max({g_peak, std::abs(li), std::abs(ri)});
    g_sumsq += (double)li * li + (double)ri * ri;
    if (li || ri) g_nonzero++;
    g_frames_played++;
    if (!g_wav_path.empty()) { g_wav.push_back((int16_t)li); g_wav.push_back((int16_t)ri); }
    g_stream++;
    if (++g_pos >= g_cur.frames) {
        g_pos = 0;
        if (g_pend_valid) { g_cur = g_pend; g_pend_valid = false; g_segs.push_back({g_cur.p, g_cur.frames, g_stream}); }
        else g_cur_valid = false;   // AI ociosa
        trim_segs();
    }
}

// Modelo nulo: avanca pela tempo decorrido. Chamado com g_mu travado.
void null_advance_locked() {
    const auto t = Clock::now();
    g_null_acc += std::chrono::duration<double>(t - g_null_last).count() * g_rate.load();
    g_null_last = t;
    while (g_null_acc >= 1.0 && g_cur_valid) { g_null_acc -= 1.0; consume_one_locked(); }
    if (!g_cur_valid) g_null_acc = 0.0;
}

void write_wav(const std::string& path, const std::vector<int16_t>& s, uint32_t rate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::printf("[audio] nao consegui abrir %s\n", path.c_str()); return; }
    const uint32_t bytes = (uint32_t)(s.size() * 2), br = rate * 4;
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(2); w32(rate); w32(br); w16(4); w16(16);
    std::fwrite("data", 1, 4, f); w32(bytes);
    std::fwrite(s.data(), 2, s.size(), f);
    std::fclose(f);
}

// Preenche `n` quadros do dispositivo (taxa dev_rate), `ch` canais, float32 ou int16.
void render(void* out, uint32_t n, uint32_t dev_rate, uint32_t ch, bool is_float) {
    std::lock_guard<std::mutex> l(g_mu);
    const double step = (double)g_rate.load() / (double)dev_rate;
    float* of = (float*)out;
    int16_t* oi = (int16_t*)out;
    for (uint32_t i = 0; i < n; i++) {
        float L = 0.f, R = 0.f;
        if (g_cur_valid) {
            float a0, b0, a1, b1;
            int li, ri;
            const int64_t rd = (int64_t)g_stream - (int64_t)g_lag_frames;
            stream_frame(rd, a0, b0, li, ri);
            stream_frame(rd + 1, a1, b1, li, ri);
            const float a = (float)g_frac;
            L = ((1.f - a) * a0 + a * a1) / 32768.f;
            R = ((1.f - a) * b0 + a * b1) / 32768.f;
            g_frac += step;
            while (g_frac >= 1.0 && g_cur_valid) { g_frac -= 1.0; consume_one_locked(); }
        } else if (g_first_t >= 0) {
            g_underruns++;
        }
        if (is_float) {
            for (uint32_t c = 0; c < ch; c++) of[i * ch + c] = c == 0 ? L : c == 1 ? R : 0.f;
        } else {
            for (uint32_t c = 0; c < ch; c++) oi[i * ch + c] = (int16_t)std::lround((c == 0 ? L : c == 1 ? R : 0.f) * 32767.f);
        }
    }
}

void device_thread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* rc = nullptr;
    WAVEFORMATEX* wf = nullptr;
    HANDLE ev = nullptr;
    auto fail = [&](const char* what, HRESULT hr) {
        std::printf("[audio] WASAPI: %s falhou (0x%08lX); usando o modelo nulo\n", what, (unsigned long)hr);
    };
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) { fail("MMDeviceEnumerator", hr); goto done; }
    hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (FAILED(hr)) { fail("GetDefaultAudioEndpoint", hr); goto done; }
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr)) { fail("Activate", hr); goto done; }
    hr = client->GetMixFormat(&wf);
    if (FAILED(hr)) { fail("GetMixFormat", hr); goto done; }
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, wf, nullptr);
    if (FAILED(hr)) { fail("Initialize", hr); goto done; }
    ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    client->SetEventHandle(ev);
    hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&rc);
    if (FAILED(hr)) { fail("GetService", hr); goto done; }
    {
        UINT32 buf_frames = 0;
        client->GetBufferSize(&buf_frames);
        bool is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto* ex = (const WAVEFORMATEXTENSIBLE*)wf;
            is_float = ex->SubFormat.Data1 == 3;   // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = 00000003-...
        }
        std::printf("[audio] WASAPI ativo: %lu Hz, %u canais, %s, buffer %u quadros\n", (unsigned long)wf->nSamplesPerSec, (unsigned)wf->nChannels,
                    is_float ? "float32" : "int16", buf_frames);
        g_dev_ok = true;
        client->Start();
        while (!g_stop) {
            WaitForSingleObject(ev, 100);
            UINT32 pad = 0;
            if (FAILED(client->GetCurrentPadding(&pad))) break;
            const UINT32 n = buf_frames - pad;
            if (!n) continue;
            BYTE* data = nullptr;
            if (FAILED(rc->GetBuffer(n, &data))) break;
            render(data, n, wf->nSamplesPerSec, wf->nChannels, is_float);
            rc->ReleaseBuffer(n, 0);
        }
        client->Stop();
    }
done:
    if (rc) rc->Release();
    if (client) client->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    if (wf) CoTaskMemFree(wf);
    if (ev) CloseHandle(ev);
    CoUninitialize();
    g_dev_ok = false;
}
}  // namespace

void wdc_audio_init(const char* wav_path, bool real_output) {
    if (wav_path) g_wav_path = wav_path;
    g_lag_ms = 40.0;   // padrao: 40 ms (WDC_AUDIO_LAG_MS=0 desliga; maximo 110)
    if (const char* e = std::getenv("WDC_AUDIO_LAG_MS")) g_lag_ms = std::max(0.0, std::min(110.0, std::atof(e)));
    g_real = real_output;
    if (real_output) {
        g_dev_thread = std::thread(device_thread);
        for (int i = 0; i < 100 && !g_dev_ok; i++) Sleep(20);   // espera o dispositivo (ate 2 s)
        if (!g_dev_ok) g_real = false;
    }
}

void wdc_audio_set_frequency(uint32_t hz) {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_real) null_advance_locked();
    g_rate = hz ? hz : 32000;
    g_lag_frames = (uint32_t)(g_lag_ms * 0.001 * (double)g_rate.load());
    std::printf("[audio %7.2fs] frequencia = %u Hz (atraso de leitura %.0f ms = %u quadros)\n", wdc_now_s(), (unsigned)g_rate.load(), g_lag_ms, g_lag_frames);
}

void wdc_audio_queue_samples(int16_t*, size_t) {}
size_t wdc_audio_frames_remaining() { return 0; }

// osAiSetNextBuffer(void* buff, u32 size): com o DMA anterior ainda cheio devolve -1 (libultra: __osAiDeviceBusy) [ext].
extern "C" void wdc_hook_osAiSetNextBuffer(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t addr = (uint32_t)ctx->r4, bytes = (uint32_t)ctx->r5;
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_real) null_advance_locked();
    g_calls++;
    if (g_first_t < 0) g_first_t = wdc_now_s();
    g_last_t = wdc_now_s();
    if (g_cur_valid && g_pend_valid) { g_rejected++; ctx->r2 = (gpr)(int64_t)-1; return; }
    Desc d;
    d.p = rdram + (addr & 0x1FFFFFFF);
    d.frames = bytes / 4;
    if (!d.frames) { ctx->r2 = 0; return; }
    g_frames_in += d.frames;
    if (!g_cur_valid) { g_cur = d; g_cur_valid = true; g_pos = 0; g_frac = 0.0; g_null_last = Clock::now(); g_null_acc = 0.0; g_segs.push_back({d.p, d.frames, g_stream}); trim_segs(); }
    else { g_pend = d; g_pend_valid = true; }
    ctx->r2 = 0;
}

// osAiGetStatus: bit 31 = cheio (DMA em curso + pendente), bit 30 = ocupado [ext: AI_STATUS_REG; o jogo so testa o sinal].
extern "C" void wdc_hook_osAiGetStatus(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_real) null_advance_locked();
    g_status_calls++;
    uint32_t st = 0;
    if (g_cur_valid) st |= 0x40000000u;
    if (g_cur_valid && g_pend_valid) st |= 0x80000000u;
    ctx->r2 = (gpr)(int32_t)st;
}

// osAiGetLength: bytes que faltam do DMA em curso (0 se ociosa).
extern "C" void wdc_hook_osAiGetLength(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_real) null_advance_locked();
    ctx->r2 = g_cur_valid ? (gpr)(uint32_t)((g_cur.frames - g_pos) * 4u) : 0;
}

void wdc_audio_shutdown() {
    g_stop = true;
    if (g_dev_thread.joinable()) g_dev_thread.join();
    std::lock_guard<std::mutex> l(g_mu);
    const double secs = g_rate.load() ? (double)g_frames_played / g_rate.load() : 0.0;
    std::printf("[audio] osAiSetNextBuffer: %llu chamadas (%llu recusadas por AI cheia), %llu quadros enfileirados; osAiGetStatus: %llu; "
                "tocados %llu quadros (%.2f s a %u Hz); pico %d; RMS %.1f; quadros nao nulos %llu; saida real: %s; faltas no dispositivo: %llu\n",
                (unsigned long long)g_calls, (unsigned long long)g_rejected, (unsigned long long)g_frames_in, (unsigned long long)g_status_calls,
                (unsigned long long)g_frames_played, secs, (unsigned)g_rate.load(), g_peak,
                g_frames_played ? std::sqrt(g_sumsq / (2.0 * (double)g_frames_played)) : 0.0, (unsigned long long)g_nonzero, g_real ? "sim" : "nao",
                (unsigned long long)g_underruns);
    if (g_calls) std::printf("[audio] primeira entrega em %.2fs, ultima em %.2fs\n", g_first_t, g_last_t);
    if (!g_wav_path.empty()) {
        write_wav(g_wav_path, g_wav, g_rate.load());
        std::printf("[audio] WAV gravado em %s (%zu quadros, %u Hz)\n", g_wav_path.c_str(), g_wav.size() / 2, (unsigned)g_rate.load());
    }
}
