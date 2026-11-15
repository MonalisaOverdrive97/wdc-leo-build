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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
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
// (Tentativa descartada em 2026-11-12: ler a RDRAM com atraso (60/40 ms) para dar folga a tarefa de audio. Na maquina do usuario o som passou a picotar o tempo todo:
// o anel que o jogo reescreve nao garante que o dado de ha 40 ms ainda esteja la. Voltou a leitura no instante nominal.)
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

// ---- reamostragem de alta qualidade (taxa do jogo -> taxa do dispositivo) ----
// Interpolador polifasico de sinc com janela de Kaiser (32 pontos, 1024 fases, corte em 0,46 da taxa do jogo): a interpolacao linear usada antes deixava
// "imagens" espectrais e atenuava os agudos, o que soa como chiado e aspereza em mixagens cheias. O historico guarda copias dos quadros ja tocados
// (o jogo reescreve o anel logo atras da posicao de reproducao, entao reler a RDRAM para tras daria dados novos).
constexpr int kTaps = 32, kPhases = 1024;
std::vector<float> g_poly;          // kPhases * kTaps
float g_hist_l[kTaps], g_hist_r[kTaps];
int g_hist_head = 0;                // proximo indice a gravar; o quadro mais recente esta em head-1
bool g_hq = true;                   // WDC_AUDIO_LINEAR=1 volta para a interpolacao linear (comparacao)

double bessel_i0(double x) {
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 40; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; }
    return s;
}
void build_poly() {
    g_poly.assign((size_t)kPhases * kTaps, 0.f);
    const double fc = 0.46, beta = 7.5, half = kTaps / 2.0, pi = 3.14159265358979323846, i0b = bessel_i0(beta);
    for (int ph = 0; ph < kPhases; ph++) {
        const double frac = (double)ph / kPhases;
        double sum = 0.0;
        double h[kTaps];
        for (int j = 0; j < kTaps; j++) {
            const double t = (double)(j - (kTaps / 2 - 1)) - frac;   // posicao do tap em relacao ao ponto de interpolacao (em quadros de origem)
            const double x = 2.0 * fc * t;
            const double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(pi * x) / (pi * x);
            const double r = t / half;
            const double w = std::fabs(r) >= 1.0 ? 0.0 : bessel_i0(beta * std::sqrt(1.0 - r * r)) / i0b;
            h[j] = 2.0 * fc * sinc * w;
            sum += h[j];
        }
        for (int j = 0; j < kTaps; j++) g_poly[(size_t)ph * kTaps + j] = (float)(h[j] / sum);   // ganho DC = 1 em cada fase
    }
}
inline void push_hist(float L, float R) {
    g_hist_l[g_hist_head] = L; g_hist_r[g_hist_head] = R;
    g_hist_head = (g_hist_head + 1) % kTaps;
}

// ---- diagnostico: "anel velho" (o jogo nao reescreveu o anel a tempo e a AI tocou dados da volta anterior = som de "ranhura") ----
// Guarda o quadro lido em cada posicao do anel; uma sequencia de >= 64 quadros nao silenciosos IDENTICOS aos lidos uma volta antes (186 ms) so acontece com dado velho.
struct StaleDet {
    std::vector<uint32_t> last;
    const uint8_t* p = nullptr;
    uint32_t run = 0, run_start = 0;
    uint64_t events = 0, frames = 0;
    double t_first = -1;
} g_sd;
uint64_t g_pump_gaps = 0;                    // vezes em que o jogo ficou > 120 ms sem chamar osAiGetStatus (a "bomba" de audio parou)
double g_pump_gap_max = 0.0, g_pump_last = -1.0;
uint64_t g_jump_big = 0;                     // avancos da AI > 2048 quadros (93 ms) entre duas leituras de osAiGetLength: acima do limite de mistura do jogo
uint32_t g_jump_max = 0;
uint64_t g_len_last_played = 0;

inline void stale_note(int li, int ri) {
    const uint32_t wd = ((uint32_t)(uint16_t)li << 16) | (uint16_t)ri;
    if (g_sd.p != g_cur.p || g_sd.last.size() != g_cur.frames) { g_sd.p = g_cur.p; g_sd.last.assign(g_cur.frames, 0u); g_sd.run = 0; }
    uint32_t& prev = g_sd.last[g_pos];
    if (wd == prev && (std::abs(li) + std::abs(ri)) > 64) {
        if (!g_sd.run) g_sd.run_start = g_pos;
        g_sd.run++;
    } else {
        if (g_sd.run >= 64) {
            g_sd.events++; g_sd.frames += g_sd.run;
            if (g_sd.t_first < 0) g_sd.t_first = wdc_now_s();
            if (g_sd.events <= 40) std::printf("[audio %7.2fs] ANEL VELHO: %u quadros (%.0f ms) repetidos da volta anterior (posicao %u)\n", wdc_now_s(), g_sd.run, 1000.0 * g_sd.run / std::max<uint32_t>(1, g_rate.load()), g_sd.run_start);
        }
        g_sd.run = 0;
    }
    prev = wd;
}

// Consome um quadro do DMA em curso (WAV e estatisticas); ao fim do DMA o pendente assume. Chamado com g_mu travado.
void consume_one_locked() {
    float L, R;
    int li, ri;
    frame_at(g_cur, g_pos, L, R, li, ri);
    push_hist(L, R);
    stale_note(li, ri);
    g_peak = std::max({g_peak, std::abs(li), std::abs(ri)});
    g_sumsq += (double)li * li + (double)ri * ri;
    if (li || ri) g_nonzero++;
    g_frames_played++;
    if (!g_wav_path.empty()) { g_wav.push_back((int16_t)li); g_wav.push_back((int16_t)ri); }
    if (++g_pos >= g_cur.frames) {
        g_pos = 0;
        if (g_pend_valid) { g_cur = g_pend; g_pend_valid = false; }
        else g_cur_valid = false;   // AI ociosa
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

// Volume mestre e limitador suave SO na saida para o dispositivo (o WAV de captura e a AI continuam com o sinal original do jogo). O mix do jogo satura em +-32768
// nas colisoes/motor no talo e soa como "explosao" no alto-falante; -2,5 dB de folga e um joelho suave tiram o pior do pico. WDC_AUDIO_GAIN (0,1 a 1,0) ajusta; 1,0 = sem folga.
float g_out_gain = 0.75f;
inline float soft_limit(float x) {
    const float k = 0.6f, a = std::fabs(x);
    if (a <= k) return x;
    const float o = k + (1.f - k) * std::tanh((a - k) / (1.f - k));
    return x < 0.f ? -o : o;
}
std::atomic<uint64_t> g_dev_starved{0};   // vezes em que o dispositivo ficou sem nenhum quadro (a thread de audio atrasou mais que o buffer)

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
            frame_at(g_cur, g_pos, a0, b0, li, ri);
            float sl, sr;
            if (g_hq) {
                // 32 pontos: n-15..n-1 do historico (copias), n..n+16 do anel (adiante da posicao de reproducao); apos o fim do DMA, segue o pendente ou repete o ultimo quadro
                const float* h = &g_poly[(size_t)std::min<int>(kPhases - 1, (int)(g_frac * kPhases)) * kTaps];
                float accl = 0.f, accr = 0.f;
                for (int j = 0; j < kTaps / 2 - 1; j++) {   // j = 0..14  -> quadros n-15 .. n-1
                    const int idx = (g_hist_head - (kTaps / 2 - 1) + j + 2 * kTaps) % kTaps;
                    accl += h[j] * g_hist_l[idx]; accr += h[j] * g_hist_r[idx];
                }
                float lastl = a0, lastr = b0;
                for (int j = kTaps / 2 - 1; j < kTaps; j++) {   // j = 15..31 -> quadros n .. n+16
                    const uint32_t k = g_pos + (uint32_t)(j - (kTaps / 2 - 1));
                    float fl, fr; int xl, xr;
                    if (k < g_cur.frames) { frame_at(g_cur, k, fl, fr, xl, xr); lastl = fl; lastr = fr; }
                    else if (g_pend_valid && k - g_cur.frames < g_pend.frames) { frame_at(g_pend, k - g_cur.frames, fl, fr, xl, xr); lastl = fl; lastr = fr; }
                    accl += h[j] * lastl; accr += h[j] * lastr;
                }
                sl = accl; sr = accr;
            } else {
                if (g_pos + 1 < g_cur.frames) frame_at(g_cur, g_pos + 1, a1, b1, li, ri);
                else if (g_pend_valid) frame_at(g_pend, 0, a1, b1, li, ri);
                else { a1 = a0; b1 = b0; }
                const float a = (float)g_frac;
                sl = (1.f - a) * a0 + a * a1; sr = (1.f - a) * b0 + a * b1;
            }
            L = soft_limit((sl / 32768.f) * g_out_gain);
            R = soft_limit((sr / 32768.f) * g_out_gain);
            g_frac += step;
            while (g_frac >= 1.0 && g_cur_valid) { g_frac -= 1.0; consume_one_locked(); }
        } else if (g_first_t >= 0) {
            g_underruns++;
            static bool in_gap = false;
            if (!in_gap) { in_gap = true; static int lines = 0; if (lines++ < 30) std::printf("[audio %7.2fs] AI sem dados: silencio ate o jogo enfileirar mais audio\n", wdc_now_s()); }
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
    // buffer de 100 ms (o padrao do modo compartilhado tem so ~22 ms: qualquer atraso da thread de audio vira estalo); preenchemos ate 50 ms a frente
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000, 0, wf, nullptr);
    if (FAILED(hr)) {   // refaz o cliente com o buffer padrao
        client->Release();
        client = nullptr;
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
        if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, wf, nullptr);
    }
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
        DWORD mmcss_task = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task);   // prioridade de multimidia: o Windows nao deixa a thread de audio ser atrasada por carga
        client->Start();
        const UINT32 target = std::min<UINT32>(buf_frames, (UINT32)((double)wf->nSamplesPerSec * 0.050));
        while (!g_stop) {
            WaitForSingleObject(ev, 100);
            UINT32 pad = 0;
            if (FAILED(client->GetCurrentPadding(&pad))) break;
            if (pad == 0 && g_cur_valid) { const uint64_t k = ++g_dev_starved; if (k <= 20) std::printf("[audio %7.2fs] dispositivo sem quadros (thread de audio atrasada)\n", wdc_now_s()); }
            const UINT32 n = target > pad ? target - pad : 0;
            if (!n) continue;
            BYTE* data = nullptr;
            if (FAILED(rc->GetBuffer(n, &data))) break;
            render(data, n, wf->nSamplesPerSec, wf->nChannels, is_float);
            rc->ReleaseBuffer(n, 0);
        }
        client->Stop();
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
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
    if (const char* e = std::getenv("WDC_AUDIO_GAIN")) g_out_gain = std::max(0.1f, std::min(1.0f, (float)std::atof(e)));
    g_hq = std::getenv("WDC_AUDIO_LINEAR") == nullptr;
    if (g_hq) build_poly();
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
    std::printf("[audio %7.2fs] frequencia = %u Hz\n", wdc_now_s(), (unsigned)g_rate.load());
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
    if (!g_cur_valid) { g_cur = d; g_cur_valid = true; g_pos = 0; g_frac = 0.0; g_null_last = Clock::now(); g_null_acc = 0.0; }
    else { g_pend = d; g_pend_valid = true; }
    ctx->r2 = 0;
}

// osAiGetStatus: bit 31 = cheio (DMA em curso + pendente), bit 30 = ocupado [ext: AI_STATUS_REG; o jogo so testa o sinal].
extern "C" void wdc_hook_osAiGetStatus(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_real) null_advance_locked();
    g_status_calls++;
    {   // a "bomba" de audio do jogo (func_80042FA8) chama isto uma vez por quadro; uma pausa longa = quadro do jogo atrasado = o anel pode esvaziar
        const double now = wdc_now_s();
        if (g_pump_last >= 0.0) {
            const double dt = now - g_pump_last;
            if (dt > g_pump_gap_max) g_pump_gap_max = dt;
            if (dt > 0.120) { g_pump_gaps++; if (g_pump_gaps <= 30) std::printf("[audio %7.2fs] bomba de audio do jogo parada por %.0f ms (quadro do jogo atrasado)\n", now, dt * 1000.0); }
        }
        g_pump_last = now;
    }
    uint32_t st = 0;
    if (g_cur_valid) st |= 0x40000000u;
    if (g_cur_valid && g_pend_valid) st |= 0x80000000u;
    ctx->r2 = (gpr)(int32_t)st;
}

// osAiGetLength: bytes que faltam do DMA em curso (0 se ociosa).
extern "C" void wdc_hook_osAiGetLength(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_real) null_advance_locked();
    {   // quantos quadros a AI andou desde a leitura anterior (o jogo mistura no maximo 2048 quadros por chamada: mais que isso deixa um buraco de dado velho)
        const uint64_t adv = g_frames_played - g_len_last_played;
        g_len_last_played = g_frames_played;
        if (adv > g_jump_max) g_jump_max = (uint32_t)adv;
        if (adv > 2048) { g_jump_big++; if (g_jump_big <= 20) std::printf("[audio %7.2fs] a AI andou %llu quadros (%.0f ms) entre duas leituras: acima do limite de mistura do jogo (2048)\n", wdc_now_s(), (unsigned long long)adv, 1000.0 * (double)adv / std::max<uint32_t>(1, g_rate.load())); }
    }
    ctx->r2 = g_cur_valid ? (gpr)(uint32_t)((g_cur.frames - g_pos) * 4u) : 0;
}

// Enhanced: a bomba de audio e a maquina de estados da musica contam QUADROS do jogo (ver gen_wdc_toml.py). Com o jogo a 60 Hz (retraco a retraco) pulamos uma chamada em cada duas;
// se o quadro durou 2 retracos ou mais (30 Hz, demo, travada), executa sempre (cadencia original).
extern "C" uint64_t wdc_vi_ticks_c();
static uint64_t g_gate_skipped[2] = {0, 0}, g_gate_calls[2] = {0, 0};
extern "C" int wdc_audio_gate(int id) {
    static const bool off = std::getenv("WDC_NO_AUDIO_GATE") != nullptr;   // diagnostico: desliga o 30 Hz do audio (comparacao A/B)
    if (off) return 0;
    static uint64_t last_call[2] = {~0ull, ~0ull};
    static bool skipped_last[2] = {false, false};
    const uint64_t now = wdc_vi_ticks_c();
    const uint64_t d = last_call[id] == ~0ull ? 2 : now - last_call[id];
    last_call[id] = now;
    g_gate_calls[id]++;
    if (d >= 2) { skipped_last[id] = false; return 0; }
    if (skipped_last[id]) { skipped_last[id] = false; return 0; }
    skipped_last[id] = true;
    g_gate_skipped[id]++;
    return 1;
}

void wdc_audio_shutdown() {
    g_stop = true;
    if (g_dev_thread.joinable()) g_dev_thread.join();
    std::lock_guard<std::mutex> l(g_mu);
    const double secs = g_rate.load() ? (double)g_frames_played / g_rate.load() : 0.0;
    std::printf("[audio] osAiSetNextBuffer: %llu chamadas (%llu recusadas por AI cheia), %llu quadros enfileirados; osAiGetStatus: %llu; "
                "tocados %llu quadros (%.2f s a %u Hz); pico %d; RMS %.1f; quadros nao nulos %llu; saida real: %s; faltas no dispositivo: %llu; vezes sem quadros no dispositivo: %llu\n",
                (unsigned long long)g_calls, (unsigned long long)g_rejected, (unsigned long long)g_frames_in, (unsigned long long)g_status_calls,
                (unsigned long long)g_frames_played, secs, (unsigned)g_rate.load(), g_peak,
                g_frames_played ? std::sqrt(g_sumsq / (2.0 * (double)g_frames_played)) : 0.0, (unsigned long long)g_nonzero, g_real ? "sim" : "nao",
                (unsigned long long)g_underruns, (unsigned long long)g_dev_starved.load());
    if (g_calls) std::printf("[audio] primeira entrega em %.2fs, ultima em %.2fs\n", g_first_t, g_last_t);
    std::printf("[audio] audio a 30 Hz no Enhanced: bomba %llu de %llu chamadas puladas, musica %llu de %llu\n", (unsigned long long)g_gate_skipped[0], (unsigned long long)g_gate_calls[0],
                (unsigned long long)g_gate_skipped[1], (unsigned long long)g_gate_calls[1]);
    std::printf("[audio] diagnostico: anel velho %llu ocorrencias (%llu quadros); bomba parada > 120 ms: %llu vezes (maior pausa %.0f ms); avanco da AI maior que 2048 quadros: %llu vezes (maior avanco %u quadros); reamostragem: %s\n",
                (unsigned long long)g_sd.events, (unsigned long long)g_sd.frames, (unsigned long long)g_pump_gaps, g_pump_gap_max * 1000.0, (unsigned long long)g_jump_big, g_jump_max,
                g_hq ? "sinc 32 pontos" : "linear");
    if (!g_wav_path.empty()) {
        write_wav(g_wav_path, g_wav, g_rate.load());
        std::printf("[audio] WAV gravado em %s (%zu quadros, %u Hz)\n", g_wav_path.c_str(), g_wav.size() / 2, (unsigned)g_rate.load());
    }
}
