// Setup (launcher). Janela Win32 desenhada com GDI+ (sem dependencias alem do Windows): arte do jogo a esquerda, escolhas a direita.
// Tres paginas: PRINCIPAL (modo de jogo e esquema de controles), PERSONALIZAR CONTROLES (botao a botao, para o esquema escolhido) e IMAGEM
// (resolucao interna, tela, suavizacao, v-sync, tela cheia). Tudo vai para wdc_config.toml ao apertar JOGAR; Sair nao grava nada.
#include "wdc_setup.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "wdc_config.h"
#include "wdc_input.h"
#include "wdc_setup_art.h"
#include "wdc_version.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
using std::min;   // o gdiplus.h usa min/max e NOMINMAX os remove
using std::max;
#include <gdiplus.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>

using namespace Gdiplus;

namespace {
// ---- paleta (a do anuncio do jogo: preto, amarelo, vermelho) ----
const Color kBgTop(255, 14, 16, 20), kBgBottom(255, 26, 29, 36);
const Color kYellow(255, 247, 196, 0), kRed(255, 214, 40, 40);
const Color kText(255, 236, 238, 242), kMuted(255, 150, 156, 168), kDim(255, 98, 103, 114);
const Color kCard(255, 30, 34, 42), kCardHover(255, 38, 43, 53), kCardSel(255, 48, 42, 14);

enum class Page { Main, Controls, Image };

// ids dos elementos clicaveis
enum Id : int {
    IdNone = -1,
    IdMode0 = 0, IdMode1, IdCtl0, IdCtl1, IdPlay, IdQuit, IdImagePage, IdCustomPage,   // pagina principal
    IdShortcut,                                                                       // pagina principal: atalho na area de trabalho
    IdBack, IdDefaults,                                                               // paginas internas: OK e Restaurar padrao
    IdTab0, IdTab1,                                                                   // personalizar: esquema que esta sendo editado
    IdImg = 100,                                                                      // imagem: IdImg + grupo * 10 + opcao
    IdRowBtn = 200, IdRowPrev = 210, IdRowNext = 220,                                 // personalizar: + indice da acao
};
enum ImgGroup { GScale = 0, GAspect, GFilter, GVsync, GFull, GCount };

struct Widget { RectF r; int id; };

struct Setup {
    HWND hwnd = nullptr;
    float s = 1.0f;                 // fator de DPI
    Page page = Page::Main;
    int sel_mode = 0;               // 0 Legacy, 1 Enhanced
    int sel_ctl = 0;                // 0 Classico, 1 Moderno
    // imagem (indices das opcoes mostradas)
    int img[GCount] = {0, 0, 0, 0, 0};   // GScale: 0 auto,1 1x,2 2x,3 4x | GAspect: 0 4:3,1 esticar | GFilter: 0 nitido,1 suave,2 pixels,3 bicubico | GVsync: 0 ligada,1 desligada | GFull: 0 janela,1 tela cheia
    bool filter_touched = false;
    // controles personalizados
    int ed_scheme = 0;              // esquema em edicao (0 classico, 1 moderno)
    uint32_t bind[2][WA_COUNT] = {};
    bool bind_dirty[2] = {false, false};
    int capture = -1;               // acao esperando o botao do controle (-1 = nenhuma)
    bool cap_armed = false;         // soltou tudo antes de comecar a ouvir
    bool pad_present = false;
    int hover = IdNone;
    int shortcut_state = 0;         // 0 = nao criado, 1 = criado, 2 = falhou
    bool play = false;
    bool done = false;
    ULONG_PTR gdip = 0;
    Bitmap* art = nullptr;
    std::vector<Widget> widgets;    // preenchido a cada pintura (modo imediato): o clique usa o desenho mais recente
};
Setup g;

// area cliente em unidades logicas: arte (477x640) + painel de 563 -> 1040x640
constexpr float kArtW = 477.f, kW = 1040.f, kH = 640.f;
constexpr float kX0 = kArtW + 36, kPW = kW - kArtW - 72;   // coluna de conteudo

float S(float v) { return v * g.s; }

void rounded(GraphicsPath& p, RectF r, float rad) {
    const float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

void text(Graphics& gr, const wchar_t* t, const wchar_t* family, float size_px, FontStyle style, const Color& c, float x, float y, float w = 0, StringAlignment al = StringAlignmentNear) {
    FontFamily ff(family);
    Font f(&ff, size_px, style, UnitPixel);
    SolidBrush b(c);
    StringFormat sf;
    sf.SetAlignment(al);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    RectF box(x, y, w > 0 ? w : 4000.f, size_px * 1.6f);
    gr.DrawString(t, -1, &f, box, &sf, &b);
}

RectF reg(int id, RectF r) { g.widgets.push_back({r, id}); return r; }

void card(Graphics& gr, int id, RectF r, bool selected, const wchar_t* tag, const wchar_t* title, const wchar_t* line1, const wchar_t* line2) {
    reg(id, r);
    GraphicsPath path;
    rounded(path, r, S(10));
    SolidBrush fill(selected ? kCardSel : (g.hover == id ? kCardHover : kCard));
    gr.FillPath(&fill, &path);
    Pen pen(selected ? kYellow : Color(255, 58, 63, 75), selected ? S(2.f) : S(1.f));
    gr.DrawPath(&pen, &path);
    // marcador de selecao (bolinha)
    const float cx = r.X + S(26), cy = r.Y + r.Height / 2, rr = S(9);
    Pen ring(selected ? kYellow : kMuted, S(2.f));
    gr.DrawEllipse(&ring, cx - rr, cy - rr, rr * 2, rr * 2);
    if (selected) { SolidBrush dot(kYellow); gr.FillEllipse(&dot, cx - S(4.5f), cy - S(4.5f), S(9), S(9)); }
    const float tx = r.X + S(52);
    text(gr, title, L"Impact", S(21), FontStyleRegular, selected ? kYellow : kText, tx, r.Y + S(8));
    text(gr, tag, L"Segoe UI", S(11), FontStyleBold, kMuted, r.X + r.Width - S(150), r.Y + S(12), S(138), StringAlignmentFar);
    text(gr, line1, L"Segoe UI", S(12.5f), FontStyleRegular, kText, tx, r.Y + S(36), r.Width - S(64));
    text(gr, line2, L"Segoe UI", S(12.5f), FontStyleRegular, kMuted, tx, r.Y + S(55), r.Width - S(64));
}

void button(Graphics& gr, int id, RectF r, const wchar_t* label, bool primary) {
    reg(id, r);
    GraphicsPath path;
    rounded(path, r, S(8));
    const bool hot = g.hover == id;
    if (primary) {
        LinearGradientBrush br(r, hot ? Color(255, 255, 214, 40) : kYellow, hot ? kYellow : Color(255, 226, 170, 0), LinearGradientModeVertical);
        gr.FillPath(&br, &path);
        text(gr, label, L"Impact", S(24), FontStyleRegular, Color(255, 20, 20, 24), r.X, r.Y + S(9), r.Width, StringAlignmentCenter);
    } else {
        SolidBrush br(hot ? kCardHover : kCard);
        gr.FillPath(&br, &path);
        Pen pen(Color(255, 58, 63, 75), S(1.f));
        gr.DrawPath(&pen, &path);
        text(gr, label, L"Segoe UI", S(16), FontStyleBold, kText, r.X, r.Y + S(13), r.Width, StringAlignmentCenter);
    }
}

// botao pequeno (atalho para uma pagina)
void pill(Graphics& gr, int id, RectF r, const wchar_t* label) {
    reg(id, r);
    GraphicsPath path;
    rounded(path, r, r.Height / 2);
    const bool hot = g.hover == id;
    SolidBrush br(hot ? Color(255, 60, 52, 14) : kCard);
    gr.FillPath(&br, &path);
    Pen pen(hot ? kYellow : Color(255, 90, 96, 110), S(1.2f));
    gr.DrawPath(&pen, &path);
    text(gr, label, L"Segoe UI", S(12), FontStyleBold, hot ? kYellow : kText, r.X, r.Y + (r.Height - S(19)) / 2, r.Width, StringAlignmentCenter);
}

// opcao de uma fileira (estilo "segmentado")
void chip(Graphics& gr, int id, RectF r, const wchar_t* label, bool selected, bool dim = false) {
    reg(id, r);
    GraphicsPath path;
    rounded(path, r, S(8));
    SolidBrush fill(selected ? kCardSel : (g.hover == id ? kCardHover : kCard));
    gr.FillPath(&fill, &path);
    Pen pen(selected ? (dim ? Color(255, 140, 112, 20) : kYellow) : Color(255, 58, 63, 75), selected ? S(2.f) : S(1.f));
    gr.DrawPath(&pen, &path);
    const Color tc = selected ? (dim ? Color(255, 170, 140, 30) : kYellow) : (dim ? kDim : kText);
    text(gr, label, L"Segoe UI", S(13), FontStyleBold, tc, r.X, r.Y + (r.Height - S(21)) / 2, r.Width, StringAlignmentCenter);
}

// ============================ atalho na area de trabalho ============================
bool create_desktop_shortcut() {
    wchar_t exe[MAX_PATH * 2] = {};
    if (!GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])))) return false;
    PWSTR desk = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) return false;
    const std::wstring lnk = std::wstring(desk) + L"\\World Driver Championship.lnk";
    CoTaskMemFree(desk);
    const bool com_ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    bool ok = false;
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
        std::wstring dir(exe);
        dir = dir.substr(0, dir.find_last_of(L"\\"));
        sl->SetPath(exe);
        sl->SetWorkingDirectory(dir.c_str());
        sl->SetDescription(L"World Driver Championship - Leo Build");
        sl->SetIconLocation(exe, 0);
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
            ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
            pf->Release();
        }
        sl->Release();
    }
    if (com_ok) CoUninitialize();
    return ok;
}

// ============================ pagina principal ============================
void paint_main(Graphics& gr) {
    const float x0 = S(kX0), w = S(kPW);
    pill(gr, IdShortcut, RectF(x0 + w - S(214), S(32), S(214), S(26)),
         g.shortcut_state == 1 ? L"ATALHO CRIADO ✓" : g.shortcut_state == 2 ? L"NÃO DEU: TENTE DE NOVO" : L"ATALHO NA ÁREA DE TRABALHO");
    text(gr, L"" WDC_BUILD_NAME_W L"  " WDC_BUILD_VERSION_W L"  •  " WDC_BUILD_CHANNEL_W, L"Segoe UI", S(11), FontStyleBold, kMuted, x0, S(66), w, StringAlignmentFar);
    const float card_h = 78, gap = 10;
    text(gr, L"MODO DE JOGO", L"Segoe UI", S(12), FontStyleBold, kRed, x0, S(110));
    pill(gr, IdImagePage, RectF(x0 + w - S(124), S(104), S(124), S(26)), L"IMAGEM…  [I]");
    float y = 138;
    card(gr, IdMode0, RectF(x0, S(y), w, S(card_h)), g.sel_mode == 0, L"[1]", L"LEGACY",
         L"Como no Nintendo 64: recompilação pura.", L"30 fps em corrida, resolução original, sem melhorias.");
    card(gr, IdMode1, RectF(x0, S(y + card_h + gap), w, S(card_h)), g.sel_mode == 1, L"[2]", L"ENHANCED",
         L"60 fps em corrida e imagem melhorada.", L"Resolução interna ampliada, v-sync e suavização.");
    y = 138 + 2 * card_h + gap + 58;
    text(gr, L"CONTROLES (XINPUT)", L"Segoe UI", S(12), FontStyleBold, kRed, x0, S(336));
    pill(gr, IdCustomPage, RectF(x0 + w - S(150), S(330), S(150), S(26)), L"PERSONALIZAR…  [P]");
    card(gr, IdCtl0, RectF(x0, S(y), w, S(card_h)), g.sel_ctl == 0, L"[3]", L"CLÁSSICO",
         L"Layout do Nintendo 64: A acelera, B freia, R/Z marchas.", L"Analógico direito = botões C (Y = C-cima, câmera).");
    card(gr, IdCtl1, RectF(x0, S(y + card_h + gap), w, S(card_h)), g.sel_ctl == 1, L"[4]", L"MODERNO",
         L"Em corrida: RT acelera, LT freia, Y troca a visão.", L"X/□ desce marcha, A/✕ sobe marcha. Menus = clássico.");
    button(gr, IdPlay, RectF(x0, S(kH - 78), w * 0.62f, S(48)), L"JOGAR", true);
    button(gr, IdQuit, RectF(x0 + w * 0.62f + S(14), S(kH - 78), w * 0.38f - S(14), S(48)), L"Sair", false);
    text(gr, L"Enter joga  •  Esc sai  •  1 a 4 escolhem  •  vale na próxima partida", L"Segoe UI", S(11), FontStyleRegular, kMuted, x0, S(kH - 22));
}

// ============================ pagina de imagem ============================
void img_group(Graphics& gr, int grp, float y, const wchar_t* title, const wchar_t* const* labels, int n, const wchar_t* caption, bool dim) {
    const float x0 = S(kX0), w = S(kPW), gapx = S(8);
    text(gr, title, L"Segoe UI", S(12), FontStyleBold, kRed, x0, S(y));
    const float cw = (w - gapx * (n - 1)) / n;
    for (int i = 0; i < n; i++) chip(gr, IdImg + grp * 10 + i, RectF(x0 + i * (cw + gapx), S(y + 20), cw, S(34)), labels[i], g.img[grp] == i, dim);
    text(gr, caption, L"Segoe UI", S(12), FontStyleRegular, dim ? kDim : kMuted, x0, S(y + 59), w);
}

void paint_image(Graphics& gr) {
    const float x0 = S(kX0), w = S(kPW);
    text(gr, L"AJUSTES DE IMAGEM", L"Impact", S(22), FontStyleRegular, kText, x0, S(98));
    const bool legacy = g.sel_mode == 0;
    {   // resolucao interna
        static const wchar_t* L_[] = {L"AUTO", L"1×", L"2×", L"4×"};
        static const wchar_t* cap[] = {
            L"Escolhe pela placa de vídeo: 4× em placa dedicada, 2× em integrada.",
            L"Resolução original do Nintendo 64 (a mais leve).",
            L"Boa e leve: roda bem até em placas integradas.",
            L"Alta: bordas e texturas bem mais nítidas. Para placa dedicada."};
        img_group(gr, GScale, 138, legacy ? L"RESOLUÇÃO INTERNA   (só vale no modo Enhanced)" : L"RESOLUÇÃO INTERNA", L_, 4,
                  legacy ? L"No modo Legacy a imagem usa a resolução original; esta escolha vale quando você usar o Enhanced." : cap[g.img[GScale]], legacy);
    }
    {   // tela
        static const wchar_t* L_[] = {L"4:3 ORIGINAL", L"ESTICAR"};
        static const wchar_t* cap[] = {L"Mantém a proporção do jogo, com barras pretas nas laterais.",
                                       L"Preenche toda a janela. Em telas largas a imagem fica esticada."};
        img_group(gr, GAspect, 224, L"TELA", L_, 2, cap[g.img[GAspect]], false);
    }
    {   // suavizacao
        static const wchar_t* L_[] = {L"NÍTIDO", L"SUAVE", L"PIXELS", L"BICÚBICO"};
        static const wchar_t* cap[] = {L"Bordas definidas, sem serrilhado: o equilíbrio ideal.",
                                       L"Imagem mais macia (filtro bilinear).",
                                       L"Pixels quadrados, sem nenhum filtro.",
                                       L"O mais bonito ao ampliar: contornos limpos e curvas suaves."};
        img_group(gr, GFilter, 310, L"SUAVIZAÇÃO AO AMPLIAR PARA A JANELA", L_, 4, cap[g.img[GFilter]], false);
    }
    {   // v-sync
        static const wchar_t* L_[] = {L"LIGADO", L"DESLIGADO"};
        static const wchar_t* cap[] = {L"Sincroniza com o monitor: sem rasgos na imagem (recomendado).",
                                       L"Menor atraso. Use só com monitor G-Sync/FreeSync; pode rasgar a imagem."};
        img_group(gr, GVsync, 396, L"SINCRONIZAÇÃO VERTICAL (V-SYNC)", L_, 2, cap[g.img[GVsync]], false);
    }
    {   // tela cheia
        static const wchar_t* L_[] = {L"JANELA", L"TELA CHEIA"};
        static const wchar_t* cap[] = {L"Abre numa janela. F11 alterna para tela cheia durante o jogo.",
                                       L"Abre em tela cheia. F11 volta para a janela durante o jogo."};
        img_group(gr, GFull, 482, L"AO ABRIR O JOGO", L_, 2, cap[g.img[GFull]], false);
    }
    button(gr, IdBack, RectF(x0, S(kH - 78 + 8), w * 0.62f, S(48 - 4)), L"OK", true);
    button(gr, IdDefaults, RectF(x0 + w * 0.62f + S(14), S(kH - 78 + 8), w * 0.38f - S(14), S(48 - 4)), L"Padrão", false);
    text(gr, L"Enter/Esc voltam  •  as escolhas são gravadas ao apertar JOGAR", L"Segoe UI", S(11), FontStyleRegular, kMuted, x0, S(kH - 22));
}

// ============================ pagina de controles ============================
const wchar_t* kActionName[WA_COUNT] = {L"ACELERAR", L"FREAR", L"FREIO SUAVE", L"MARCHA ACIMA", L"MARCHA ABAIXO", L"TROCAR VISÃO"};
const wchar_t* kActionHint[WA_COUNT] = {L"Botão A do Nintendo 64", L"Botão B: freio forte e ré", L"Botão L: freia sem travar", L"Botão R: câmbio manual", L"Botão Z: câmbio manual",
                                        L"C-cima: câmera interna / externa"};

std::wstring bind_label(uint32_t m) {
    if (!m) return L"SEM BOTÃO";
    std::wstring t;
    for (int i = 0; i < kWdcPickableCount; i++)
        if (m & kWdcPickableSources[i]) {
            if (!t.empty()) t += L" + ";
            const char* nm = wdc_input_source_label(kWdcPickableSources[i]);
            t += std::wstring(nm, nm + std::strlen(nm));
        }
    return t.empty() ? L"?" : t;
}

void paint_controls(Graphics& gr) {
    const float x0 = S(kX0), w = S(kPW);
    text(gr, L"PERSONALIZAR CONTROLES", L"Impact", S(22), FontStyleRegular, kText, x0, S(98));
    {   // esquema em edicao
        const float gapx = S(8), cw = (w - gapx) / 2;
        chip(gr, IdTab0, RectF(x0, S(134), cw, S(34)), L"CLÁSSICO  [1]", g.ed_scheme == 0);
        chip(gr, IdTab1, RectF(x0 + cw + gapx, S(134), cw, S(34)), L"MODERNO  [2]", g.ed_scheme == 1);
        text(gr, g.ed_scheme == 0 ? L"Vale em todo o jogo (menus e corrida)." : L"Vale só em corrida; nos menus vale o Clássico.", L"Segoe UI", S(12), FontStyleRegular, kMuted, x0, S(173), w);
    }
    float y = 198;
    for (int a = 0; a < WA_COUNT; a++, y += 52) {
        const RectF r(x0, S(y), w, S(46));
        GraphicsPath path;
        rounded(path, r, S(8));
        SolidBrush fill(g.capture == a ? kCardSel : kCard);
        gr.FillPath(&fill, &path);
        Pen pen(g.capture == a ? kYellow : Color(255, 58, 63, 75), g.capture == a ? S(2.f) : S(1.f));
        gr.DrawPath(&pen, &path);
        text(gr, kActionName[a], L"Segoe UI", S(14.5f), FontStyleBold, kText, r.X + S(14), r.Y + S(5), S(190));
        text(gr, kActionHint[a], L"Segoe UI", S(11.5f), FontStyleRegular, kMuted, r.X + S(14), r.Y + S(25), S(210));
        // controles do lado direito: [<] [valor] [>]
        const float bw = S(30), vw = S(150), gp = S(6);
        const float rx = r.X + r.Width - S(12) - (bw + gp + vw + gp + bw);
        const float by = r.Y + S(8), bh = S(30);
        chip(gr, IdRowPrev + a, RectF(rx, by, bw, bh), L"‹", false);
        const bool cap = g.capture == a;
        const std::wstring label = cap ? (g.pad_present ? L"APERTE O BOTÃO…" : L"SEM CONTROLE: USE ‹ ›") : bind_label(g.bind[g.ed_scheme][a]);
        const RectF vr(rx + bw + gp, by, vw, bh);
        chip(gr, IdRowBtn + a, vr, label.c_str(), cap);
        chip(gr, IdRowNext + a, RectF(rx + bw + gp + vw + gp, by, bw, bh), L"›", false);
    }
    text(gr, g.capture >= 0 ? L"Aperte o botão desejado no controle  •  Del = sem botão  •  Esc cancela"
                            : L"Clique no botão do meio e aperte a tecla no controle, ou use as setas ‹ ›.", L"Segoe UI", S(12), FontStyleRegular, g.capture >= 0 ? kYellow : kMuted, x0, S(512), w);
    text(gr, L"Fixos: analógico esquerdo = direção  •  analógico direito = botões C  •  Start = pausa", L"Segoe UI", S(11.5f), FontStyleRegular, kMuted, x0, S(531), w);
    button(gr, IdBack, RectF(x0, S(kH - 78 + 8), w * 0.62f, S(48 - 4)), L"OK", true);
    button(gr, IdDefaults, RectF(x0 + w * 0.62f + S(14), S(kH - 78 + 8), w * 0.38f - S(14), S(48 - 4)), L"Padrão", false);
    text(gr, L"Enter/Esc voltam  •  as escolhas são gravadas ao apertar JOGAR", L"Segoe UI", S(11), FontStyleRegular, kMuted, x0, S(kH - 22));
}

void paint(HDC dc) {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    const int W = rc.right - rc.left, H = rc.bottom - rc.top;
    Bitmap buf(W, H, PixelFormat32bppPARGB);
    Graphics gr(&buf);
    gr.SetSmoothingMode(SmoothingModeAntiAlias);
    gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.widgets.clear();
    // fundo do painel direito
    LinearGradientBrush bg(Rect(0, 0, W, H), kBgTop, kBgBottom, LinearGradientModeVertical);
    gr.FillRectangle(&bg, 0, 0, W, H);
    // arte a esquerda (proporcao original), com uma sombra suave na borda direita
    if (g.art && g.art->GetLastStatus() == Ok) {
        const float ah = (float)H, aw = ah * (float)kArtW / kH;   // 477x640 -> altura da janela
        gr.DrawImage(g.art, RectF(0, 0, aw, ah));
        LinearGradientBrush fade(RectF(aw - S(46), 0, S(46), ah), Color(0, 14, 16, 20), Color(235, 14, 16, 20), LinearGradientModeHorizontal);
        gr.FillRectangle(&fade, aw - S(46), 0.f, S(46), ah);
    }
    // faixa vermelha/amarela de destaque
    SolidBrush stripeR(kRed), stripeY(kYellow);
    gr.FillRectangle(&stripeR, S(kArtW) - S(3), 0.f, S(3), (float)H);
    gr.FillRectangle(&stripeY, S(kArtW) - S(6), 0.f, S(3), (float)H);

    const float x0 = S(kX0);
    text(gr, L"WORLD DRIVER", L"Impact", S(40), FontStyleRegular, kYellow, x0, S(26));
    text(gr, L"CHAMPIONSHIP  •  RECOMPILAÇÃO PARA PC", L"Segoe UI", S(12.5f), FontStyleBold, kMuted, x0, S(72));
    if (g.page == Page::Main) paint_main(gr);
    else if (g.page == Page::Image) paint_image(gr);
    else paint_controls(gr);

    Graphics wnd(dc);
    wnd.DrawImage(&buf, 0, 0, W, H);
}

int hit_test(int x, int y) {
    for (auto it = g.widgets.rbegin(); it != g.widgets.rend(); ++it) if (it->r.Contains((REAL)x, (REAL)y)) return it->id;
    return IdNone;
}

void redraw() { InvalidateRect(g.hwnd, nullptr, FALSE); }

void finish(bool play) { g.play = play; g.done = true; DestroyWindow(g.hwnd); }

void stop_capture() {
    if (g.capture >= 0) KillTimer(g.hwnd, 1);
    g.capture = -1;
}

void go_page(Page p) {
    stop_capture();
    g.page = p;
    if (p == Page::Controls) g.ed_scheme = g.sel_ctl;   // abre editando o esquema escolhido
    redraw();
}

// a fonte "src" passa a ser so da acao "a" no esquema em edicao (tira das outras para nao disparar duas acoes)
void assign(int a, uint32_t src) {
    const int s = g.ed_scheme;
    if (src) for (int o = 0; o < WA_COUNT; o++) if (o != a && (g.bind[s][o] & src)) g.bind[s][o] &= ~src;
    g.bind[s][a] = src;
    g.bind_dirty[s] = true;
}

void cycle(int a, int dir) {   // ‹ › sem controle: sem botao -> A -> B -> X -> Y -> LB -> RB -> LT -> RT -> L3 -> R3 -> BACK -> sem botao
    const int n = kWdcPickableCount + 1;
    int idx = 0;   // 0 = sem botao; i + 1 = kWdcPickableSources[i]
    const uint32_t cur = g.bind[g.ed_scheme][a];
    if (cur) {
        idx = -1;
        for (int i = 0; i < kWdcPickableCount; i++) if (cur == kWdcPickableSources[i]) idx = i + 1;
        if (idx < 0) idx = dir > 0 ? n - 1 : 1;   // combinacao (ex.: B+X): ‹ › recomeca da ponta
    }
    idx = ((idx + dir) % n + n) % n;
    assign(a, idx == 0 ? 0 : kWdcPickableSources[idx - 1]);
}

void start_capture(int a) {
    stop_capture();
    g.capture = a;
    g.cap_armed = false;
    uint32_t cur = 0;
    g.pad_present = wdc_input_probe_pad(&cur);
    SetTimer(g.hwnd, 1, 30, nullptr);
}

void poll_capture() {
    uint32_t pressed = 0;
    g.pad_present = wdc_input_probe_pad(&pressed);
    if (!g.pad_present) { redraw(); return; }
    uint32_t pickable = 0;
    for (int i = 0; i < kWdcPickableCount; i++) pickable |= kWdcPickableSources[i];
    if (!g.cap_armed) { if (!(pressed & pickable)) g.cap_armed = true; redraw(); return; }   // espera soltar o que ja estava apertado
    for (int i = 0; i < kWdcPickableCount; i++)
        if (pressed & kWdcPickableSources[i]) {
            const int a = g.capture;
            stop_capture();
            assign(a, kWdcPickableSources[i]);
            redraw();
            return;
        }
}

void restore_defaults() {
    if (g.page == Page::Controls) {
        stop_capture();
        for (int a = 0; a < WA_COUNT; a++) g.bind[g.ed_scheme][a] = wdc_input_default_binding(g.ed_scheme, a);
        g.bind_dirty[g.ed_scheme] = true;
    } else if (g.page == Page::Image) {
        g.img[GScale] = 0; g.img[GAspect] = 0; g.img[GFilter] = g.sel_mode == 1 ? 0 : 1; g.img[GVsync] = 0; g.img[GFull] = 0;
        g.filter_touched = false;
    }
    redraw();
}

void click(int id) {
    if (id >= IdRowBtn && id < IdRowBtn + (int)WA_COUNT) { start_capture(id - IdRowBtn); redraw(); return; }
    if (id >= IdRowPrev && id < IdRowPrev + (int)WA_COUNT) { stop_capture(); cycle(id - IdRowPrev, -1); redraw(); return; }
    if (id >= IdRowNext && id < IdRowNext + (int)WA_COUNT) { stop_capture(); cycle(id - IdRowNext, +1); redraw(); return; }
    if (id >= IdImg && id < IdImg + GCount * 10) {
        const int grp = (id - IdImg) / 10, opt = (id - IdImg) % 10;
        g.img[grp] = opt;
        if (grp == GFilter) g.filter_touched = true;
        redraw();
        return;
    }
    switch (id) {
    case IdMode0: g.sel_mode = 0; if (!g.filter_touched) g.img[GFilter] = 1; break;
    case IdMode1: g.sel_mode = 1; if (!g.filter_touched) g.img[GFilter] = 0; break;
    case IdCtl0: g.sel_ctl = 0; break;
    case IdCtl1: g.sel_ctl = 1; break;
    case IdShortcut: g.shortcut_state = create_desktop_shortcut() ? 1 : 2; break;
    case IdImagePage: go_page(Page::Image); return;
    case IdCustomPage: go_page(Page::Controls); return;
    case IdBack: go_page(Page::Main); return;
    case IdDefaults: restore_defaults(); return;
    case IdTab0: stop_capture(); g.ed_scheme = 0; break;
    case IdTab1: stop_capture(); g.ed_scheme = 1; break;
    case IdPlay: finish(true); return;
    case IdQuit: finish(false); return;
    }
    redraw();
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paint(dc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        if (w == 1 && g.capture >= 0) poll_capture();
        return 0;
    case WM_MOUSEMOVE: {
        const int id = hit_test((short)LOWORD(l), (short)HIWORD(l));
        if (id != g.hover) { g.hover = id; redraw(); }
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: g.hover = IdNone; redraw(); return 0;
    case WM_SETCURSOR:
        if (g.hover != IdNone) { SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649))); return TRUE; }
        break;
    case WM_LBUTTONDOWN: {
        const int id = hit_test((short)LOWORD(l), (short)HIWORD(l));
        if (id != IdNone) click(id);
        else if (g.capture >= 0) { stop_capture(); redraw(); }   // clicar fora cancela a captura
        return 0;
    }
    case WM_KEYDOWN:
        if (g.capture >= 0) {   // captura de botao: so Esc (cancela) e Del (sem botao) valem
            if (w == VK_ESCAPE) { stop_capture(); redraw(); }
            else if (w == VK_DELETE || w == VK_BACK) { const int a = g.capture; stop_capture(); assign(a, 0); redraw(); }
            return 0;
        }
        if (g.page == Page::Main) {
            if (w == VK_RETURN) finish(true);
            else if (w == VK_ESCAPE) finish(false);
            else if (w >= '1' && w <= '4') click((int)(w - '1'));
            else if (w >= VK_NUMPAD1 && w <= VK_NUMPAD4) click((int)(w - VK_NUMPAD1));
            else if (w == 'I') go_page(Page::Image);
            else if (w == 'P') go_page(Page::Controls);
        } else {
            if (w == VK_RETURN || w == VK_ESCAPE || w == VK_BACK) go_page(Page::Main);
            else if (g.page == Page::Controls && (w == '1' || w == VK_NUMPAD1)) click(IdTab0);
            else if (g.page == Page::Controls && (w == '2' || w == VK_NUMPAD2)) click(IdTab1);
        }
        return 0;
    case WM_CLOSE: finish(false); return 0;
    case WM_DESTROY: KillTimer(h, 1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool save_png(HWND h, const char* path) {
    RECT rc;
    GetClientRect(h, &rc);
    const int W = rc.right, H = rc.bottom;
    HDC dc = GetDC(h);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(h, mem, PW_CLIENTONLY);
    SelectObject(mem, old);
    Bitmap img(bmp, nullptr);
    CLSID png;
    UINT n = 0, sz = 0;
    GetImageEncodersSize(&n, &sz);
    std::string buf(sz, '\0');
    GetImageEncoders(n, sz, (ImageCodecInfo*)buf.data());
    bool ok = false;
    for (UINT i = 0; i < n; i++) {
        const ImageCodecInfo& c = ((ImageCodecInfo*)buf.data())[i];
        if (std::wcscmp(c.MimeType, L"image/png") == 0) { png = c.Clsid; ok = true; break; }
    }
    if (ok) {
        const std::wstring wp(path, path + std::strlen(path));
        ok = img.Save(wp.c_str(), &png, nullptr) == Ok;
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(h, dc);
    return ok;
}

// ---- gravacao ----
const char* const kScaleText[4] = {"\"auto\"", "1", "2", "4"};
const char* const kFilterText[4] = {"\"sharp\"", "\"bilinear\"", "\"nearest\"", "\"bicubic\""};

void save_all(const std::filesystem::path& dir) {
    wdc_cfg_save_value(dir, "", "mode", g.sel_mode == 1 ? "\"enhanced\"" : "\"legacy\"");
    wdc_cfg_save_value(dir, "controls", "scheme", g.sel_ctl == 1 ? "\"modern\"" : "\"classic\"");
    for (int s = 0; s < 2; s++) {
        if (!g.bind_dirty[s]) continue;
        for (int a = 0; a < WA_COUNT; a++) {
            const std::string v = g.bind[s][a] == wdc_input_default_binding(s, a) ? "\"default\"" : "\"" + wdc_input_binding_to_text(g.bind[s][a]) + "\"";
            wdc_cfg_save_value(dir, s == 0 ? "controls.classic" : "controls.modern", kWdcActionKeys[a], v);
        }
    }
    wdc_cfg_save_value(dir, "enhanced", "internal_scale", kScaleText[g.img[GScale]]);
    wdc_cfg_save_value(dir, "display", "aspect", g.img[GAspect] == 1 ? "\"stretch\"" : "\"4:3\"");
    if (g.filter_touched) wdc_cfg_save_value(dir, "display", "filter", kFilterText[g.img[GFilter]]);
    wdc_cfg_save_value(dir, "display", "vsync", g.img[GVsync] == 1 ? "\"off\"" : "\"on\"");
    wdc_cfg_save_value(dir, "display", "fullscreen", g.img[GFull] == 1 ? "true" : "false");
}
}  // namespace

bool wdc_setup_run(const std::filesystem::path& config_dir, const char* shot_path) {
    SetProcessDPIAware();
    g = Setup{};
    GdiplusStartupInput in;
    if (GdiplusStartup(&g.gdip, &in, nullptr) != Ok) return true;   // sem GDI+: segue sem setup
    {   // dpi
        HDC sdc = GetDC(nullptr);
        g.s = (float)GetDeviceCaps(sdc, LOGPIXELSX) / 96.0f;
        ReleaseDC(nullptr, sdc);
    }
    const WdcConfig& c = wdc_cfg();
    g.sel_mode = c.enhanced ? 1 : 0;
    g.sel_ctl = c.controls == WdcControls::Modern ? 1 : 0;
    g.img[GScale] = c.scale_choice == 0 ? 0 : c.scale_choice == 1 ? 1 : c.scale_choice == 2 ? 2 : 3;
    g.img[GAspect] = c.aspect == WdcAspect::Stretch ? 1 : 0;
    g.img[GFilter] = c.filter == WdcFilter::Sharp ? 0 : c.filter == WdcFilter::Bilinear ? 1 : c.filter == WdcFilter::Nearest ? 2 : 3;
    g.filter_touched = c.filter_explicit;
    g.img[GVsync] = c.vsync == WdcVsync::Off ? 1 : 0;
    g.img[GFull] = c.fullscreen ? 1 : 0;
    for (int s = 0; s < 2; s++) for (int a = 0; a < WA_COUNT; a++) g.bind[s][a] = wdc_input_binding(s, a);
    if (const char* pg = std::getenv("WDC_SETUP_PAGE")) {   // depuracao (com --setup-shot): captura a pagina pedida
        if (!std::strcmp(pg, "image")) g.page = Page::Image;
        else if (!std::strcmp(pg, "controls")) { g.page = Page::Controls; g.ed_scheme = g.sel_ctl; }
        else if (!std::strcmp(pg, "modern")) { g.page = Page::Controls; g.ed_scheme = 1; }
    }
    if (IStream* st = SHCreateMemStream(kSetupArt, (UINT)kSetupArtSize)) {
        g.art = Bitmap::FromStream(st);
        st->Release();
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"WdcSetupWindow";
    RegisterClassW(&wc);
    RECT r{0, 0, (LONG)S(kW), (LONG)S(kH)};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    const int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"World Driver Championship - Setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, sx, sy, ww, wh,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!g.hwnd) return true;
    SetForegroundWindow(g.hwnd);
    if (shot_path) {   // depuracao: desenha, captura e fecha
        UpdateWindow(g.hwnd);
        Sleep(300);
        save_png(g.hwnd, shot_path);
        DestroyWindow(g.hwnd);
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    delete g.art;
    g.art = nullptr;
    GdiplusShutdown(g.gdip);
    if (!g.play) return false;
    save_all(config_dir);
    return true;
}
