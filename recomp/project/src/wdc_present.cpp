// Apresentador em GPU (ver wdc_present.h).
#include "wdc_present.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {
// ---------------------------------------------------------------------------------------------------------------------------------
// Estado compartilhado com a thread do RDP
// ---------------------------------------------------------------------------------------------------------------------------------
// Ritmo (v-sync ligado): cada quadro vem com o instante do retrace de VI que o gerou (t_vi). A thread do RDP entrega os quadros em rajadas
// (esperas de SYNC_FULL de ate dezenas de ms), entao exibir "o mais recente a cada vblank" alterna 1/3 vblanks. Em vez disso reconstruimos a linha do tempo
// REGULAR do VI (a thread de VI do ultramodern dorme ate instantes absolutos t0 + k/60 s): T_k = base + k*P + L, com base = menor (t_vi - k*P) visto
// (com lenta adaptacao a deriva de relogio) e L = latencia (buffer de jitter). O quadro k vai para o 1o vblank com T_k <= agora + 1/4 de vblank.
struct Frame {
    std::shared_ptr<uint8_t[]> px;   // RGBA8, imutavel depois de publicado
    unsigned w = 0, h = 0;
    int64_t t_vi = 0;                // instante do retrace de VI que gerou o quadro
    int64_t due_us = 0;              // T_k: instante a partir do qual pode ser exibido
    uint64_t seq = 0;
};
constexpr double kViPeriodUs = 1e6 / 60.0;     // o ultramodern gera retracos de VI a 60 Hz exatos
std::atomic<int64_t> g_latency_us{17000};      // buffer de jitter: ~1 retraco (+1 quadro quando o RDP segura um quadro para filtrar piscadas: wdc_present_set_extra_latency_us)
constexpr size_t kMaxQueued = 8;
std::mutex g_mu;
std::deque<Frame> g_queue;           // quadros publicados e ainda nao exibidos (mais antigo primeiro)
uint64_t g_seq = 0;                  // numero do ultimo quadro publicado
double g_base_us = 0.0;              // linha do tempo regular: instante estimado do VI 0
bool g_base_init = false;
int g_base_above = 0;
double g_lag_peak = 20000.0;          // pico (us) do atraso VI -> entrega do quadro, com decaimento lento (folga adaptativa)                // quadros seguidos com a diferenca muito acima da base
std::atomic<int64_t> g_vblank_us{8333};   // periodo do vblank (media movel dos intervalos entre apresentacoes)
HANDLE g_new_frame = nullptr;        // evento: ha quadro novo (usado com v-sync desligado)

std::atomic<bool> g_active{false};
std::atomic<bool> g_quit{false};
std::thread g_thread;
HWND g_hwnd = nullptr;
WdcConfig g_cfg;

// estatisticas
std::atomic<uint64_t> g_published{0}, g_displayed_new{0}, g_repeats{0}, g_dropped{0}, g_presents{0}, g_late{0}, g_overdue{0};
std::atomic<uint64_t> g_lag_hist[7];    // atraso entre o retrace de VI e a entrega do quadro pela thread do RDP: <5, <10, <17, <25, <40, <80, >=80 ms
std::atomic<uint64_t> g_hold_hist[6];
std::atomic<double> g_lead_sum_ms{0}, g_lead_min_ms{1e9}, g_lead_max_ms{-1e9};   // folga (instante previsto - agora) ao exibir um quadro novo   // por quadro novo: quantos vblanks o quadro ANTERIOR ficou na tela (1, 2, 3, 4, 5, 6+)
std::atomic<double> g_interval_sum_ms{0}, g_interval_max_ms{0};
std::string g_adapter_name;
unsigned g_sc_w = 0, g_sc_h = 0;
bool g_tearing = false;

int64_t now_us() {
    static const LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e6 / (double)freq.QuadPart);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Shaders (HLSL compilado em tempo de execucao com d3dcompiler_47.dll, que acompanha o Windows 10/11)
// ---------------------------------------------------------------------------------------------------------------------------------
const char* kHlsl = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
cbuffer CB : register(b0) { float2 texSize; float2 scale; uint mode; float3 pad; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
// Catmull-Rom (bicubico) em 9 amostras bilineares (Jimenez/Pharr): bem mais suave que o bilinear ao ampliar, sem o borrao; o clamp final contem o "ringing".
float3 sample_catmull_rom(float2 uv) {
    float2 pos = uv * texSize;
    float2 c = floor(pos - 0.5) + 0.5;
    float2 f = pos - c;
    // Mitchell-Netravali (B = C = 1/3): bem menos "halo"/ringing nas bordas de alto contraste (texto, HUD) que o Catmull-Rom (B = 0, C = 1/2), um pouco mais macio.
    float2 e = 1.0 + f;
    float2 w0 = (-7.0 / 18.0) * e * e * e + 2.0 * e * e - (10.0 / 3.0) * e + 16.0 / 9.0;
    float2 w1 = (7.0 / 6.0) * f * f * f - 2.0 * f * f + 8.0 / 9.0;
    float2 g = 1.0 - f;
    float2 w2 = (7.0 / 6.0) * g * g * g - 2.0 * g * g + 8.0 / 9.0;
    float2 h = 2.0 - f;
    float2 w3 = (-7.0 / 18.0) * h * h * h + 2.0 * h * h - (10.0 / 3.0) * h + 16.0 / 9.0;
    float2 w12 = w1 + w2;
    float2 t0 = (c - 1.0) / texSize, t12 = (c + w2 / w12) / texSize, t3 = (c + 2.0) / texSize;
    float3 r = 0;
    r += tex.SampleLevel(smp, float2(t0.x, t0.y), 0).rgb * (w0.x * w0.y);
    r += tex.SampleLevel(smp, float2(t12.x, t0.y), 0).rgb * (w12.x * w0.y);
    r += tex.SampleLevel(smp, float2(t3.x, t0.y), 0).rgb * (w3.x * w0.y);
    r += tex.SampleLevel(smp, float2(t0.x, t12.y), 0).rgb * (w0.x * w12.y);
    r += tex.SampleLevel(smp, float2(t12.x, t12.y), 0).rgb * (w12.x * w12.y);
    r += tex.SampleLevel(smp, float2(t3.x, t12.y), 0).rgb * (w3.x * w12.y);
    r += tex.SampleLevel(smp, float2(t0.x, t3.y), 0).rgb * (w0.x * w3.y);
    r += tex.SampleLevel(smp, float2(t12.x, t3.y), 0).rgb * (w12.x * w3.y);
    r += tex.SampleLevel(smp, float2(t3.x, t3.y), 0).rgb * (w3.x * w3.y);
    return saturate(r);
}
float4 ps_main(VSOut i) : SV_Target {
    float2 uv = i.uv;
    if (mode == 3) return float4(sample_catmull_rom(uv), 1.0);
    if (mode == 2) {   // sharp-bilinear: texels nitidos com bordas suavizadas (so a transicao entre texels e interpolada)
        float2 sc = max(scale, 1.0);
        float2 px = uv * texSize;
        float2 tf = floor(px);
        float2 cd = frac(px) - 0.5;
        float2 range = 0.5 - 0.5 / sc;
        float2 f = (cd - clamp(cd, -range, range)) * sc + 0.5;
        uv = (tf + f) / texSize;
    }
    return float4(tex.Sample(smp, uv).rgb, 1.0);
}
)";

struct CBData { float texW, texH, scaleX, scaleY; uint32_t mode; float pad[3]; };

struct Gfx {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain2> sc;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> smp_point, smp_linear;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    unsigned tex_w = 0, tex_h = 0;
    HANDLE waitable = nullptr;
    UINT sc_flags = 0;
};

// Quadros em voo permitidos pelo swapchain (WDC_PRESENT_LATENCY=1..3; padrao 2 = folga contra vblanks perdidos, +1 vblank de latencia; 1 = menor latencia).
UINT frame_latency() {
    const char* e = std::getenv("WDC_PRESENT_LATENCY");
    const int v = e ? std::atoi(e) : 2;
    return (UINT)std::max(1, std::min(3, v));
}

bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
    ComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(kHlsl, std::strlen(kHlsl), "wdc_present", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
    if (FAILED(hr)) {
        std::printf("[present] erro de shader (%s): %s\n", entry, err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    return true;
}

bool make_rtv(Gfx& g) {
    g.rtv.Reset();
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(g.sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
    return SUCCEEDED(g.dev->CreateRenderTargetView(bb.Get(), nullptr, &g.rtv));
}

bool init_gfx(Gfx& g) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, (UINT)std::size(levels), D3D11_SDK_VERSION, &g.dev, &got, &g.ctx);
    if (FAILED(hr)) { std::printf("[present] D3D11CreateDevice falhou (0x%08X)\n", (unsigned)hr); return false; }
    ComPtr<IDXGIDevice> dxdev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(g.dev.As(&dxdev)) || FAILED(dxdev->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) { std::printf("[present] DXGI indisponivel\n"); return false; }
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    char nm[128];
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, nm, sizeof nm, nullptr, nullptr);
    g_adapter_name = nm;
    BOOL allow = FALSE;
    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(factory.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow);
    g_tearing = allow == TRUE;
    g.sc_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);

    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = 0; d.Height = 0;   // tamanho da area cliente
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 3;
    d.Scaling = DXGI_SCALING_NONE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = g.sc_flags;
    ComPtr<IDXGISwapChain1> sc1;
    hr = factory->CreateSwapChainForHwnd(g.dev.Get(), g_hwnd, &d, nullptr, nullptr, &sc1);
    if (FAILED(hr) || FAILED(sc1.As(&g.sc))) { std::printf("[present] CreateSwapChainForHwnd falhou (0x%08X)\n", (unsigned)hr); return false; }
    factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);   // tela cheia e do jogo (F11), nao do DXGI
    g.sc->SetMaximumFrameLatency(frame_latency());
    g.waitable = g.sc->GetFrameLatencyWaitableObject();
    DXGI_SWAP_CHAIN_DESC1 got_d{};
    g.sc->GetDesc1(&got_d);
    g_sc_w = got_d.Width; g_sc_h = got_d.Height;
    if (!make_rtv(g)) { std::printf("[present] nao criou a view do back buffer\n"); return false; }

    ComPtr<ID3DBlob> vsb, psb;
    if (!compile("vs_main", "vs_4_0", vsb) || !compile("ps_main", "ps_4_0", psb)) return false;
    if (FAILED(g.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.vs)) ||
        FAILED(g.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.ps))) { std::printf("[present] nao criou os shaders\n"); return false; }
    D3D11_SAMPLER_DESC sd{};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    g.dev->CreateSamplerState(&sd, &g.smp_point);
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    g.dev->CreateSamplerState(&sd, &g.smp_linear);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CBData);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &g.cb))) return false;
    return true;
}

void upload(Gfx& g, const Frame& f) {
    if (!g.tex || g.tex_w != f.w || g.tex_h != f.h) {
        g.srv.Reset(); g.tex.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = f.w; td.Height = f.h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &g.tex)) || FAILED(g.dev->CreateShaderResourceView(g.tex.Get(), nullptr, &g.srv))) { g.tex.Reset(); g.srv.Reset(); return; }
        g.tex_w = f.w; g.tex_h = f.h;
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(g.tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    for (unsigned y = 0; y < f.h; y++) std::memcpy((uint8_t*)m.pData + (size_t)y * m.RowPitch, f.px.get() + (size_t)y * f.w * 4, (size_t)f.w * 4);
    g.ctx->Unmap(g.tex.Get(), 0);
}

// Desenha a imagem (4:3, campo duplicado na vertical) centralizada, com barras pretas.
void draw(Gfx& g) {
    const float black[4] = {0, 0, 0, 1};
    g.ctx->OMSetRenderTargets(1, g.rtv.GetAddressOf(), nullptr);
    g.ctx->ClearRenderTargetView(g.rtv.Get(), black);
    if (!g.srv) return;
    const float cw = (float)g_sc_w, ch = (float)g_sc_h;
    const float dh_src = g.tex_h < g.tex_w / 2 ? (float)g.tex_h * 2.0f : (float)g.tex_h;   // campo de 640x240 vira 640x480
    float w = cw, h = cw * dh_src / (float)g.tex_w;
    if (g_cfg.aspect == WdcAspect::Stretch) { w = cw; h = ch; }   // esticar: preenche a janela (deforma a imagem)
    else if (h > ch) { h = ch; w = ch * (float)g.tex_w / dh_src; }
    D3D11_VIEWPORT vp{(cw - w) * 0.5f, (ch - h) * 0.5f, w, h, 0.0f, 1.0f};
    g.ctx->RSSetViewports(1, &vp);
    const uint32_t mode = g_cfg.filter == WdcFilter::Nearest ? 0u : g_cfg.filter == WdcFilter::Bilinear ? 1u : g_cfg.filter == WdcFilter::Sharp ? 2u : 3u;
    CBData cbd{(float)g.tex_w, (float)g.tex_h, w / (float)g.tex_w, h / (float)g.tex_h, mode, {0, 0, 0}};
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g.ctx->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { std::memcpy(m.pData, &cbd, sizeof cbd); g.ctx->Unmap(g.cb.Get(), 0); }
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(g.ps.Get(), nullptr, 0);
    g.ctx->PSSetConstantBuffers(0, 1, g.cb.GetAddressOf());
    g.ctx->PSSetShaderResources(0, 1, g.srv.GetAddressOf());
    ID3D11SamplerState* s = g_cfg.filter == WdcFilter::Nearest ? g.smp_point.Get() : g.smp_linear.Get();
    g.ctx->PSSetSamplers(0, 1, &s);
    g.ctx->Draw(3, 0);
}

// Depuracao: WDC_PRESENT_SHOT=<arquivo.bmp> [WDC_PRESENT_SHOT_AT=<segundos>] grava o back buffer ja desenhado (a captura de janela do Windows nao ve swapchains flip).
void save_backbuffer_bmp(Gfx& g, const char* path) {
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(g.sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
    D3D11_TEXTURE2D_DESC td{};
    bb->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &st))) return;
    g.ctx->CopyResource(st.Get(), bb.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    FILE* f = std::fopen(path, "wb");
    if (f) {
        const uint32_t row = (td.Width * 3 + 3) & ~3u, size = row * td.Height;
        uint8_t hdr[54] = {'B', 'M'};
        auto put32 = [&](int off, uint32_t v) { std::memcpy(hdr + off, &v, 4); };
        put32(2, 54 + size); put32(10, 54); put32(14, 40); put32(18, td.Width); put32(22, td.Height);
        hdr[26] = 1; hdr[28] = 24; put32(34, size);
        std::fwrite(hdr, 1, 54, f);
        std::vector<uint8_t> line(row);
        for (int y = (int)td.Height - 1; y >= 0; y--) {
            const uint8_t* s = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
            for (unsigned x = 0; x < td.Width; x++) { line[x * 3] = s[x * 4 + 2]; line[x * 3 + 1] = s[x * 4 + 1]; line[x * 3 + 2] = s[x * 4]; }
            std::fwrite(line.data(), 1, row, f);
        }
        std::fclose(f);
        std::printf("[present] back buffer gravado em %s (%ux%u)\n", path, td.Width, td.Height);
    }
    g.ctx->Unmap(st.Get(), 0);
}

// Periodo nominal do vblank do monitor em que a janela esta (frequencia inteira do modo de video corrente). A estimativa medida so pode variar +-10% em torno dele.
double nominal_vblank_us() {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) return 1e6 / (double)dm.dmDisplayFrequency;
    }
    return 1e6 / 60.0;
}

void resize_if_needed(Gfx& g) {
    RECT rc;
    if (!GetClientRect(g_hwnd, &rc)) return;
    const unsigned cw = (unsigned)std::max<LONG>(rc.right - rc.left, 1), ch = (unsigned)std::max<LONG>(rc.bottom - rc.top, 1);
    if (cw == g_sc_w && ch == g_sc_h) return;
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g.rtv.Reset();
    if (FAILED(g.sc->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, g.sc_flags))) return;
    DXGI_SWAP_CHAIN_DESC1 d{};
    g.sc->GetDesc1(&d);
    g_sc_w = d.Width; g_sc_h = d.Height;
    g.sc->SetMaximumFrameLatency(frame_latency());
    make_rtv(g);
}

void thread_main(std::promise<bool> ready) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);   // perder um vblank por causa de carga da CPU vira judder
    Gfx g;
    const bool ok = init_gfx(g);
    ready.set_value(ok);
    if (!ok) return;
    const char* shot_path = std::getenv("WDC_PRESENT_SHOT");
    const double shot_at = std::getenv("WDC_PRESENT_SHOT_AT") ? std::atof(std::getenv("WDC_PRESENT_SHOT_AT")) : 10.0;
    const int64_t t_start = now_us();
    bool shot_done = false;
    int64_t last_present_us = 0;
    unsigned cur_hold = 0;   // vblanks desde o ultimo quadro novo
    double recent[16] = {};
    size_t recent_n = 0;
    double nominal_us = nominal_vblank_us();
    g_vblank_us = (int64_t)nominal_us;
    int64_t iter = 0, next_iter = 0;   // iteracoes do laco de vblank; iteracao em que o proximo quadro deve aparecer
    const bool vsync = g_cfg.vsync == WdcVsync::On;
    while (!g_quit.load()) {
        if (IsIconic(g_hwnd)) { Sleep(30); continue; }
        if (vsync) {
            WaitForSingleObjectEx(g.waitable, 100, TRUE);   // libera uma vez por vblank
        } else {
            WaitForSingleObject(g_new_frame, 50);           // apresenta assim que o quadro fica pronto
        }
        if (g_quit.load()) break;
        resize_if_needed(g);
        iter++;
        if ((iter & 255) == 0) { nominal_us = nominal_vblank_us(); }   // a janela pode ter mudado de monitor
        Frame f;
        bool have_new = false;
        {
            std::lock_guard<std::mutex> l(g_mu);
            if (vsync) {
                // Agendamento por vblank: um quadro de VI dura N vblanks (N = round(periodo do VI / periodo do vblank): 2 a 120 Hz, 1 a 60 Hz). Depois de exibir um
                // quadro, o proximo so e exibido N iteracoes depois (padrao 2-2-2, sem alternar 1/3 por causa de pequenas oscilacoes de tempo); a diferenca entre o
                // instante previsto do quadro e o vblank corrente ("folga") so corrige o agendamento quando passa de 3/4 de vblank (deriva de relogio).
                const int64_t now = now_us(), vb = g_vblank_us.load();
                while (g_queue.size() >= 2 && g_queue[1].due_us <= now) { g_queue.pop_front(); g_dropped++; }   // ja ha um mais novo e vencido: o antigo nunca aparece
                if (!g_queue.empty()) {
                    const int64_t lead = g_queue.front().due_us - now;   // > 0: ainda nao e a hora
                    const bool scheduled = iter >= next_iter;
                    if ((scheduled && lead <= vb * 3 / 4) || lead < -vb * 3 / 4) {
                        f = std::move(g_queue.front());
                        g_queue.pop_front();
                        have_new = true;
                        int64_t n = std::max<int64_t>(1, (int64_t)((double)kViPeriodUs / (double)vb + 0.5));
                        if (lead < -vb * 3 / 4 && n > 1) n--;   // o quadro saiu mais de 3/4 de vblank depois do previsto: encurta este intervalo para recuperar o atraso (uma vez)
                        next_iter = iter + n;
                    }
                }
            } else if (!g_queue.empty()) {   // sem v-sync: sempre o mais recente, assim que chega
                g_dropped += g_queue.size() - 1;
                f = std::move(g_queue.back());
                g_queue.clear();
                have_new = true;
            }
        }
        if (have_new) {
            if (cur_hold) g_hold_hist[std::min<unsigned>(cur_hold, 6) - 1]++;
            cur_hold = 1;
            if (vsync) {
                const double lead_ms = (double)(f.due_us - now_us()) / 1000.0;
                g_lead_sum_ms = g_lead_sum_ms.load() + lead_ms;
                if (lead_ms < g_lead_min_ms.load()) g_lead_min_ms = lead_ms;
                if (lead_ms > g_lead_max_ms.load()) g_lead_max_ms = lead_ms;
            }
            if (vsync && now_us() - f.due_us > g_vblank_us.load() * 3 / 2) g_overdue++;   // chegou mais de 1,5 vblank depois do previsto (o buffer de jitter nao bastou)
            upload(g, f);
            g_displayed_new++;
        } else if (!vsync) {
            continue;   // sem v-sync so apresenta quando ha quadro novo
        } else {
            g_repeats++;
            cur_hold++;
        }
        draw(g);
        if (shot_path && !shot_done && (double)(now_us() - t_start) / 1e6 >= shot_at && g.srv) { save_backbuffer_bmp(g, shot_path); shot_done = true; }
        const HRESULT hr = g.sc->Present(vsync ? 1 : 0, (!vsync && g_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) { std::printf("[present] dispositivo perdido (0x%08X): voltando ao GDI\n", (unsigned)hr); g_active = false; break; }
        const int64_t t = now_us();
        if (last_present_us) {
            const double ms = (double)(t - last_present_us) / 1000.0;
            g_interval_sum_ms = g_interval_sum_ms.load() + ms;
            if (ms > g_interval_max_ms.load()) g_interval_max_ms = ms;
            if (vsync) {
                const double vb = (double)g_vblank_us.load() / 1000.0;
                if (ms > vb * 1.5 && ms < 100.0) g_late++;   // perdeu um vblank (excluindo pausas longas: janela minimizada)
                if (ms > 3.0 && ms < 40.0) {   // periodo do vblank: mediana dos ultimos 16 intervalos (robusta a vblanks perdidos e a apresentacoes em rajada)
                    recent[recent_n++ & 15] = ms;
                    double tmp[16];
                    const int cnt = (int)std::min<size_t>(recent_n, 16);
                    std::memcpy(tmp, recent, sizeof(double) * (size_t)cnt);
                    std::nth_element(tmp, tmp + cnt / 2, tmp + cnt);
                    g_vblank_us = (int64_t)std::max(nominal_us * 0.9, std::min(nominal_us * 1.1, (double)tmp[cnt / 2] * 1000.0));
                }
            }
        }
        last_present_us = t;
        g_presents++;
    }
    if (g.waitable) CloseHandle(g.waitable);
}
}  // namespace

bool wdc_present_init(void* hwnd, const WdcConfig& cfg) {
    g_hwnd = (HWND)hwnd;
    g_cfg = cfg;
    g_new_frame = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::promise<bool> p;
    auto fut = p.get_future();
    g_thread = std::thread(thread_main, std::move(p));
    const bool ok = fut.get();
    if (!ok) { g_thread.join(); return false; }
    if (cfg.fps60 && !std::getenv("WDC_NO_DEGLITCH")) g_latency_us = 17000 + 16700;   // o RDP segura 1 quadro para filtrar piscadas (wdc_rdp_vk.cpp, g_deglitch)
    g_active = true;
    std::printf("[present] D3D11 em \"%s\": swapchain %ux%u, v-sync %s%s, filtro %s\n", g_adapter_name.c_str(), g_sc_w, g_sc_h, cfg.vsync == WdcVsync::On ? "on" : "off",
                (cfg.vsync == WdcVsync::Off && g_tearing) ? " (tearing permitido: VRR)" : "", cfg.filter == WdcFilter::Nearest ? "nearest" : cfg.filter == WdcFilter::Bilinear ? "bilinear" : cfg.filter == WdcFilter::Sharp ? "sharp" : "bicubic");
    return true;
}

bool wdc_present_active() { return g_active.load(); }

int64_t wdc_present_now_us() { return now_us(); }

void wdc_present_frame(const uint8_t* rgba, unsigned w, unsigned h, int64_t t_vi_us) {
    if (!w || !h || !g_active.load()) return;
    std::shared_ptr<uint8_t[]> buf = std::make_shared_for_overwrite<uint8_t[]>((size_t)w * h * 4);
    std::memcpy(buf.get(), rgba, (size_t)w * h * 4);
    {
        std::lock_guard<std::mutex> l(g_mu);
        const uint64_t k = ++g_seq;
        const double off = (double)t_vi_us - (double)k * kViPeriodUs;
        if (!g_base_init) { g_base_us = off; g_base_init = true; }
        else if (off < g_base_us) { g_base_us = off; g_base_above = 0; }   // o retrace real nunca vem antes do previsto: a menor diferenca e a linha do tempo
        else {
            g_base_us += (off - g_base_us) * 0.001;                          // acompanha devagar a deriva de relogio
            // Se o VI pulou retraces (a thread de VI atrasou e recuperou), k*P subestima o tempo real e a diferenca sobe de vez: re-sincroniza depois de 30 quadros.
            if (off - g_base_us > 0.75 * kViPeriodUs) { if (++g_base_above >= 30) { g_base_us = off; g_base_above = 0; } }
            else g_base_above = 0;
        }
        // Folga ADAPTATIVA: acompanha o atraso real de entrega do RDP (pico com decaimento lento): em 4x a GPU entrega os quadros mais tarde e uma folga fixa vira uma
        // sequencia irregular de 1 e 2 vblanks (travadinhas); com a folga acima do atraso tipico + margem, o ritmo fica uniforme. Piso = g_latency_us, teto = 70 ms.
        {
            const double lag_us = (double)(now_us() - t_vi_us);
            g_lag_peak = lag_us > g_lag_peak ? lag_us : g_lag_peak - (g_lag_peak - lag_us) * 0.01;
        }
        const int64_t lat = std::min<int64_t>(70000, std::max<int64_t>(g_latency_us.load(), (int64_t)g_lag_peak + 5000));
        g_queue.push_back(Frame{std::move(buf), w, h, t_vi_us, (int64_t)(g_base_us + (double)k * kViPeriodUs) + lat, k});
        if (g_queue.size() > kMaxQueued) { g_queue.pop_front(); g_dropped++; }   // janela minimizada ou apresentador parado
    }
    {
        const double lag_ms = (double)(now_us() - t_vi_us) / 1000.0;
        g_lag_hist[lag_ms < 5 ? 0 : lag_ms < 10 ? 1 : lag_ms < 17 ? 2 : lag_ms < 25 ? 3 : lag_ms < 40 ? 4 : lag_ms < 80 ? 5 : 6]++;
    }
    g_published++;
    SetEvent(g_new_frame);
}

void wdc_present_shutdown() {
    if (!g_thread.joinable()) return;
    g_quit = true;
    g_active = false;
    SetEvent(g_new_frame);
    g_thread.join();
}

void wdc_present_set_extra_latency_us(int64_t extra_us) { g_latency_us = 17000 + std::max<int64_t>(0, extra_us); }

void wdc_present_print_stats() {
    if (g_adapter_name.empty()) return;
    const uint64_t n = g_presents.load();
    std::printf("[present] %s: apresentacoes=%llu (quadros novos=%llu, repetidos=%llu), publicados=%llu, descartados=%llu, atrasados>1,5 vblank=%llu, intervalo medio=%.2f ms (vblank ~%.2f ms), maximo=%.2f ms, vblanks perdidos=%llu\n",
                g_cfg.vsync == WdcVsync::On ? "v-sync on" : "v-sync off", (unsigned long long)n, (unsigned long long)g_displayed_new.load(), (unsigned long long)g_repeats.load(),
                (unsigned long long)g_published.load(), (unsigned long long)g_dropped.load(), (unsigned long long)g_overdue.load(), n > 1 ? g_interval_sum_ms.load() / (double)(n - 1) : 0.0,
                (double)g_vblank_us.load() / 1000.0, g_interval_max_ms.load(), (unsigned long long)g_late.load());
    uint64_t tot = 0;
    for (auto& h : g_hold_hist) tot += h.load();
    if (g_displayed_new.load()) std::printf("[present] folga (previsto - agora) ao exibir: media=%.1f ms, minimo=%.1f, maximo=%.1f (alvo: 0 a +6 ms; negativo = chegou depois do previsto)\n", g_lead_sum_ms.load() / (double)g_displayed_new.load(), g_lead_min_ms.load(), g_lead_max_ms.load());
    std::printf("[present] atraso VI->entrega do quadro (ms):");
    {
        static const char* kLag[7] = {"<5", "<10", "<17", "<25", "<40", "<80", ">=80"};
        uint64_t lt = 0;
        for (auto& h : g_lag_hist) lt += h.load();
        for (int i = 0; i < 7; i++) std::printf(" %s=%.1f%%", kLag[i], lt ? 100.0 * (double)g_lag_hist[i].load() / (double)lt : 0.0);
    }
    std::printf("\n");
    std::printf("[present] vblanks que cada quadro novo ficou na tela (ideal: todos iguais, 2 a 120 Hz / 1 a 60 Hz):");
    for (int i = 0; i < 6; i++) std::printf(" %d%s=%.1f%%", i + 1, i == 5 ? "+" : "", tot ? 100.0 * (double)g_hold_hist[i].load() / (double)tot : 0.0);
    std::printf("\n");
}
