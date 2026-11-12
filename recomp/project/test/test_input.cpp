// Teste do mapeamento XInput -> botoes N64 (esquemas Classico e Moderno). Compilar com build_test_input.cmd.
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "../src/wdc_input.h"

// stubs: o mapeamento nao usa a janela nem a RDRAM
bool wdc_window_key(int) { return false; }
uint8_t* wdc_get_rdram() { return nullptr; }

namespace {
constexpr uint16_t N_A = 0x8000, N_B = 0x4000, N_Z = 0x2000, N_START = 0x1000, N_DU = 0x0800, N_DD = 0x0400, N_DL = 0x0200, N_DR = 0x0100,
                   N_L = 0x0020, N_R = 0x0010, N_CU = 0x0008, N_CD = 0x0004, N_CL = 0x0002, N_CR = 0x0001;
constexpr uint16_t XB_DU = 0x0001, XB_DD = 0x0002, XB_DL = 0x0004, XB_DR = 0x0008, XB_START = 0x0010, XB_LB = 0x0100, XB_RB = 0x0200, XB_A = 0x1000,
                   XB_B = 0x2000, XB_X = 0x4000, XB_Y = 0x8000;
int g_fail = 0, g_n = 0;
void check(bool ok, const char* what) {
    g_n++;
    if (!ok) { std::printf("FALHOU: %s\n", what); g_fail++; }
}
WdcPadRaw pad(uint16_t b, uint8_t lt = 0, uint8_t rt = 0, int16_t lx = 0, int16_t ly = 0, int16_t rx = 0, int16_t ry = 0) { return WdcPadRaw{b, lt, rt, lx, ly, rx, ry}; }
uint16_t classic(const WdcPadRaw& p) { return wdc_input_map_pad(p).buttons; }
uint16_t modern_race(const WdcPadRaw& p) { return wdc_input_map_pad_modern(p, true).buttons; }
uint16_t modern_menu(const WdcPadRaw& p) { return wdc_input_map_pad_modern(p, false).buttons; }
}  // namespace

int main() {
    // ---- Classico: todos os 14 botoes do N64 alcancaveis ----
    check(classic(pad(XB_A)) == N_A, "classico: A -> A");
    check(classic(pad(XB_B)) == N_B, "classico: B -> B");
    check(classic(pad(XB_X)) == N_B, "classico: X -> B");
    check(classic(pad(0, 255, 0)) == N_Z, "classico: LT -> Z");
    check(classic(pad(0, 0, 255)) == N_Z, "classico: RT -> Z");
    check(classic(pad(XB_LB)) == N_L, "classico: LB -> L");
    check(classic(pad(XB_RB)) == N_R, "classico: RB -> R");
    check(classic(pad(XB_START)) == N_START, "classico: Start");
    check(classic(pad(XB_DU)) == N_DU && classic(pad(XB_DD)) == N_DD && classic(pad(XB_DL)) == N_DL && classic(pad(XB_DR)) == N_DR, "classico: direcional");
    check(classic(pad(0, 0, 0, 0, 0, 0, 30000)) == N_CU, "classico: analogico direito para cima -> C-cima");
    check(classic(pad(0, 0, 0, 0, 0, 0, -30000)) == N_CD, "classico: analogico direito para baixo -> C-baixo");
    check(classic(pad(0, 0, 0, 0, 0, -30000, 0)) == N_CL, "classico: analogico direito para a esquerda -> C-esquerda");
    check(classic(pad(0, 0, 0, 0, 0, 30000, 0)) == N_CR, "classico: analogico direito para a direita -> C-direita");
    check(classic(pad(0, 0, 0, 0, 0, 0, 9000)) == 0 && classic(pad(0, 0, 0, 0, 0, 9000, 0)) == 0, "classico: analogico direito abaixo do limiar nao aciona C");
    check(classic(pad(0, 0, 0, 0, 0, 14000, 0)) == N_CR, "classico: analogico direito a ~43% aciona C (limiar 40%)");
    check(classic(pad(XB_Y)) == N_CU, "classico: Y -> C-cima (atalho)");
    check(classic(pad(XB_A | XB_B, 255, 0)) == (N_A | N_B | N_Z), "classico: combinacao de botoes");
    // analogico esquerdo: zona morta de 20% e fim de escala
    const WdcInput dz = wdc_input_map_pad(pad(0, 0, 0, 3000, 0));
    check(dz.x == 0.f && dz.y == 0.f, "analogico esquerdo: zona morta");
    const WdcInput full = wdc_input_map_pad(pad(0, 0, 0, 32767, 0));
    check(std::fabs(full.x - 1.f) < 0.01f, "analogico esquerdo: fim de escala");

    // ---- Moderno em corrida ao vivo ----
    check(modern_race(pad(0, 0, 255)) == N_A, "moderno/corrida: RT -> A (acelera)");
    check(modern_race(pad(0, 255, 0)) == N_B, "moderno/corrida: LT -> B (freia)");
    check(modern_race(pad(XB_X)) == N_Z, "moderno/corrida: X/Quadrado -> Z (desce marcha)");
    check(modern_race(pad(XB_A)) == N_R, "moderno/corrida: A/Cruz -> R (sobe marcha)");
    check(modern_race(pad(XB_Y)) == N_CU, "moderno/corrida: Y/Triangulo -> C-cima (troca a visao)");
    check(modern_race(pad(XB_LB)) == N_Z && modern_race(pad(XB_RB)) == N_R, "moderno/corrida: LB/RB repetem descer/subir marcha");
    check(modern_race(pad(XB_B)) == 0, "moderno/corrida: B/Circulo sem funcao");
    check(modern_race(pad(XB_A)) != N_A, "moderno/corrida: A/Cruz NAO acelera");
    check(modern_race(pad(0, 0, 255, 0, 0, 0, 30000)) == (N_A | N_CU), "moderno/corrida: acelera + C-cima pelo analogico direito");
    check(modern_race(pad(XB_START)) == N_START, "moderno/corrida: Start");
    check(modern_race(pad(XB_DU)) == N_DU, "moderno/corrida: direcional");

    // ---- Moderno fora da corrida (menus, pausa, resultados) = classico ----
    check(modern_menu(pad(XB_A)) == N_A, "moderno/menu: A/Cruz -> A (confirma)");
    check(modern_menu(pad(XB_B)) == N_B, "moderno/menu: B/Circulo -> B (volta)");
    check(modern_menu(pad(0, 255, 0)) == N_Z, "moderno/menu: LT -> Z (trocar cor do carro, espelhar pista)");
    check(modern_menu(pad(XB_LB)) == N_L && modern_menu(pad(XB_RB)) == N_R, "moderno/menu: LB/RB -> L/R");
    check(modern_menu(pad(0, 0, 0, 0, 0, 30000, 0)) == N_CR, "moderno/menu: analogico direito -> C (navegar sub-telas)");

    std::printf("%d verificacoes, %d falhas\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
