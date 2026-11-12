// Teste dos saves em pasta fixa (wdc_saves): migracao so copia e nunca sobrescreve; backup datado sem repeticao e com limite; nunca toca no save.
// Build: ver build_test_saves.cmd
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include "wdc_saves.h"

namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FALHA linha %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream f(p, std::ios::binary); f << s; }
static std::string get(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
static int nback(const fs::path& d) { int n = 0; for (auto& e : fs::directory_iterator(d)) { (void)e; n++; } return n; }

int main() {
    const fs::path t = fs::temp_directory_path() / "wdc_test_saves";
    fs::remove_all(t);
    const fs::path oldf = t / "build_a" / "wdc_pak0.mpk", newf = t / "saves" / "wdc_pak0.mpk";
    put(oldf, "SAVE-ANTIGO");
    // migracao: copia, o antigo continua la
    CHECK(wdc_saves_migrate(oldf, newf));
    CHECK(get(newf) == "SAVE-ANTIGO" && get(oldf) == "SAVE-ANTIGO");
    // o jogador progride no save novo; outra build (pasta com outro save antigo) NAO pode sobrescrever
    put(newf, "SAVE-NOVO-PROGRESSO");
    put(t / "build_b" / "wdc_pak0.mpk", "OUTRO-ANTIGO");
    CHECK(!wdc_saves_migrate(t / "build_b" / "wdc_pak0.mpk", newf));
    CHECK(get(newf) == "SAVE-NOVO-PROGRESSO");
    CHECK(!wdc_saves_migrate(t / "inexistente.mpk", t / "saves" / "x.mpk"));
    // backup: cria 1; repetir sem mudar nao cria; mudar cria outro (nomes com a data: espera 1 s para diferir)
    wdc_saves_backup(newf, 2);
    const fs::path bdir = t / "saves" / "backups";
    CHECK(nback(bdir) == 1);
    wdc_saves_backup(newf, 2);
    CHECK(nback(bdir) == 1);
    for (int k = 0; k < 3; k++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        put(newf, "VERSAO-" + std::to_string(k));
        wdc_saves_backup(newf, 2);
    }
    CHECK(nback(bdir) == 2);                 // limite de 2 backups por save
    CHECK(get(newf) == "VERSAO-2");          // o save em si nunca e tocado
    wdc_saves_backup(t / "nao_existe.mpk");  // sem arquivo: nada acontece
    CHECK(wdc_saves_dir().empty() || fs::exists(wdc_saves_dir()));
    fs::remove_all(t);
    std::printf(fails ? "%d FALHAS\n" : "todos os testes de saves passaram\n", fails);
    return fails ? 1 : 0;
}
