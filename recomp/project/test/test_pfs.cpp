// Testes do PFS do Controller Pak (wdc_pfs.{h,cpp}).
// Build: recomp\project\test\build_test_pfs.cmd
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "wdc_pfs.h"

using namespace wdcpfs;

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { std::printf("FALHA linha %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define CHECK_EQ(a, b) do { checks++; long long A__ = (long long)(a), B__ = (long long)(b); if (A__ != B__) { std::printf("FALHA linha %d: %s == %lld, esperado %lld\n", __LINE__, #a, A__, B__); fails++; } } while (0)

static const uint16_t kCompany = 0x3031;       // "01"
static const uint32_t kGame = 0x4E574F45;      // "NWOE"

static void name16(uint8_t* out, const char* s) { std::memset(out, 0, 16); std::strncpy((char*)out, s, 16); }
static void ext4(uint8_t* out, const char* s) { std::memset(out, 0, 4); std::strncpy((char*)out, s, 4); }

int main() {
    // --- pak em branco: ID invalido -> reparo -> formatado pelo verificador ---
    {
        Pak pak; pak.plugged = true;
        Core c(pak);
        Pfs pfs;
        CHECK_EQ(c.InitPak(pfs, 0), kIdFatal);
        CHECK_EQ(c.RepairId(pfs), 0);
        CHECK_EQ(pfs.banks, 1);
        CHECK_EQ(pfs.inode_start_page, 5);
        CHECK_EQ(c.InitPak(pfs, 0), 0);
        CHECK(pfs.status & kInitialized);
        int bytes = 0;
        CHECK_EQ(c.FreeBlocks(pfs, &bytes), 0);
        CHECK_EQ(bytes, 123 * 256);
    }
    // --- pak sem cartucho ---
    {
        Pak pak; pak.plugged = false;
        Core c(pak); Pfs pfs;
        CHECK_EQ(c.InitPak(pfs, 0), kNoPack);
    }
    // --- pak de fabrica ---
    Pak pak;
    FormatPak(pak);
    Core c(pak);
    Pfs pfs;
    CHECK_EQ(c.InitPak(pfs, 0), 0);
    CHECK_EQ(pfs.dir_size, 16);
    CHECK_EQ(pfs.inode_table, 8);
    CHECK_EQ(pfs.minode_table, 16);
    CHECK_EQ(pfs.dir_table, 24);
    int bytes = 0, maxf = 0, used = 0;
    CHECK_EQ(c.FreeBlocks(pfs, &bytes), 0);
    CHECK_EQ(bytes, 31488);
    CHECK_EQ(c.NumFiles(pfs, &maxf, &used), 0);
    CHECK_EQ(maxf, 16);
    CHECK_EQ(used, 0);
    // ID em 4 copias (blocos 1,3,4,6) e bloco de inode com soma correta
    CHECK(std::memcmp(&pak.mem[1 * 32], &pak.mem[3 * 32], 32) == 0);
    CHECK(std::memcmp(&pak.mem[1 * 32], &pak.mem[4 * 32], 32) == 0);
    CHECK(std::memcmp(&pak.mem[1 * 32], &pak.mem[6 * 32], 32) == 0);
    CHECK_EQ(pak.mem[1 * 32 + 0x1A], 1);                                   // banks
    CHECK(std::memcmp(&pak.mem[8 * 32], &pak.mem[16 * 32], 256) == 0);   // inode == copia
    CHECK_EQ(pak.mem[8 * 32 + 2 * 5 + 1], 3);                              // pagina 5 livre

    // --- arquivo de 600 bytes (3 paginas) ---
    uint8_t gname[16], ext[4];
    name16(gname, "WDC RECORDS"); ext4(ext, "SAV");
    int fno = -2;
    CHECK_EQ(c.FindFile(pfs, kCompany, kGame, gname, ext, &fno), kInvalid);
    CHECK_EQ(fno, -1);
    CHECK_EQ(c.AllocateFile(pfs, kCompany, kGame, gname, ext, 600, &fno), 0);
    CHECK_EQ(fno, 0);
    CHECK_EQ(c.AllocateFile(pfs, kCompany, kGame, gname, ext, 100, &fno), kExist);
    FileState st;
    CHECK_EQ(c.FileState(pfs, 0, &st), 0);
    CHECK_EQ(st.file_size, 768);
    CHECK_EQ(st.company_code, kCompany);
    CHECK_EQ(st.game_code, kGame);
    CHECK(std::memcmp(st.game_name, gname, 16) == 0);
    CHECK_EQ(c.FreeBlocks(pfs, &bytes), 0);
    CHECK_EQ(bytes, 120 * 256);
    CHECK_EQ(c.NumFiles(pfs, &maxf, &used), 0);
    CHECK_EQ(used, 1);

    // leitura antes de gravar: dado invalido; parametros invalidos
    std::vector<uint8_t> w(640), r(640);
    for (size_t i = 0; i < w.size(); i++) w[i] = (uint8_t)(i * 7 + 3);
    CHECK_EQ(c.ReadWriteFile(pfs, 0, kRead, 0, 640, r.data()), kBadData);
    CHECK_EQ(c.ReadWriteFile(pfs, 0, kWrite, 0, 33, w.data()), kInvalid);
    CHECK_EQ(c.ReadWriteFile(pfs, 0, kWrite, 16, 32, w.data()), kInvalid);
    CHECK_EQ(c.ReadWriteFile(pfs, 16, kWrite, 0, 32, w.data()), kInvalid);
    CHECK_EQ(c.ReadWriteFile(pfs, 0, kWrite, 0, 640, w.data()), 0);       // atravessa 2 paginas e meia
    CHECK_EQ(c.ReadWriteFile(pfs, 0, kRead, 0, 640, r.data()), 0);
    CHECK(r == w);
    // leitura parcial no meio (offset 256 = 2a pagina)
    std::vector<uint8_t> part(128);
    CHECK_EQ(c.ReadWriteFile(pfs, 0, kRead, 256, 128, part.data()), 0);
    CHECK(std::memcmp(part.data(), w.data() + 256, 128) == 0);
    // alem do fim do arquivo (3 paginas = 768 bytes)
    std::vector<uint8_t> big(256);
    CHECK(c.ReadWriteFile(pfs, 0, kWrite, 704, 128, big.data()) != 0);

    // --- persistencia: nova instancia sobre a mesma imagem ---
    {
        Pak pak2; pak2.plugged = true; pak2.mem = pak.mem;
        Core c2(pak2); Pfs pfs2;
        CHECK_EQ(c2.InitPak(pfs2, 0), 0);
        int f2 = -1;
        CHECK_EQ(c2.FindFile(pfs2, kCompany, kGame, gname, ext, &f2), 0);
        CHECK_EQ(f2, 0);
        std::vector<uint8_t> r2(640);
        CHECK_EQ(c2.ReadWriteFile(pfs2, f2, kRead, 0, 640, r2.data()), 0);
        CHECK(r2 == w);
    }

    // --- fragmentacao: A(3) B(2) (ja existe o primeiro), apaga e realoca ---
    uint8_t gB[16], gC[16];
    name16(gB, "B"); name16(gC, "C");
    int fB = -1, fC = -1;
    CHECK_EQ(c.AllocateFile(pfs, kCompany, kGame, gB, ext, 512, &fB), 0);
    CHECK_EQ(fB, 1);
    CHECK_EQ(c.DeleteNote(pfs, kCompany, kGame, gname, ext), 0);          // libera 3 paginas antes de B
    CHECK_EQ(c.AllocateFile(pfs, kCompany, kGame, gC, ext, 1280, &fC), 0); // 5 paginas: 3 livres antes de B + 2 depois
    CHECK_EQ(fC, 0);                                                      // reaproveita a entrada de diretorio livre
    std::vector<uint8_t> wc(1280), rc(1280);
    for (size_t i = 0; i < wc.size(); i++) wc[i] = (uint8_t)(255 - i);
    CHECK_EQ(c.ReadWriteFile(pfs, fC, kWrite, 0, 1280, wc.data()), 0);
    CHECK_EQ(c.ReadWriteFile(pfs, fC, kRead, 0, 1280, rc.data()), 0);
    CHECK(rc == wc);
    CHECK_EQ(c.FileState(pfs, fC, &st), 0);
    CHECK_EQ(st.file_size, 1280);
    CHECK_EQ(c.FreeBlocks(pfs, &bytes), 0);
    CHECK_EQ(bytes, (123 - 2 - 5) * 256);

    // --- apagar tudo devolve as paginas; inexistente ---
    CHECK_EQ(c.DeleteNote(pfs, kCompany, kGame, gB, ext), 0);
    CHECK_EQ(c.DeleteNote(pfs, kCompany, kGame, gC, ext), 0);
    CHECK_EQ(c.DeleteNote(pfs, kCompany, kGame, gC, ext), kInvalid);
    CHECK_EQ(c.FreeBlocks(pfs, &bytes), 0);
    CHECK_EQ(bytes, 31488);
    CHECK_EQ(c.NumFiles(pfs, &maxf, &used), 0);
    CHECK_EQ(used, 0);

    // --- limites: pak cheio e diretorio cheio ---
    CHECK_EQ(c.AllocateFile(pfs, kCompany, kGame, gname, ext, 123 * 256, &fno), 0);
    CHECK_EQ(c.AllocateFile(pfs, kCompany, kGame, gB, ext, 1, &fB), kDataFull);
    CHECK_EQ(c.DeleteNote(pfs, kCompany, kGame, gname, ext), 0);
    for (int i = 0; i < 16; i++) {
        uint8_t n[16]; char buf[16]; std::snprintf(buf, sizeof buf, "NOTE%02d", i); name16(n, buf);
        CHECK_EQ(c.AllocateFile(pfs, kCompany, kGame, n, ext, 256, &fno), 0);
    }
    CHECK_EQ(c.AllocateFile(pfs, kCompany, kGame, gB, ext, 256, &fB), kDirFull);
    CHECK_EQ(c.NumFiles(pfs, &maxf, &used), 0);
    CHECK_EQ(used, 16);
    for (int i = 0; i < 16; i++) {
        uint8_t n[16]; char buf[16]; std::snprintf(buf, sizeof buf, "NOTE%02d", i); name16(n, buf);
        CHECK_EQ(c.DeleteNote(pfs, kCompany, kGame, n, ext), 0);
    }
    CHECK_EQ(c.FreeBlocks(pfs, &bytes), 0);
    CHECK_EQ(bytes, 31488);

    // --- parametros invalidos de alocacao ---
    CHECK_EQ(c.AllocateFile(pfs, 0, kGame, gname, ext, 256, &fno), kInvalid);
    CHECK_EQ(c.AllocateFile(pfs, kCompany, kGame, gname, ext, 0, &fno), kInvalid);

    // --- tabela de inodes principal corrompida: o espelho salva ---
    {
        CHECK_EQ(c.AllocateFile(pfs, kCompany, kGame, gname, ext, 512, &fno), 0);
        CHECK_EQ(c.ReadWriteFile(pfs, fno, kWrite, 0, 512, std::vector<uint8_t>(512, 0x5A).data()), 0);
        Pak p2; p2.plugged = true; p2.mem = pak.mem;
        p2.mem[8 * 32 + 20] ^= 0xFF;                         // estraga a tabela principal
        Core c2(p2); Pfs pf2;
        CHECK_EQ(c2.InitPak(pf2, 0), 0);
        CHECK_EQ(c2.FreeBlocks(pf2, &bytes), 0);
        CHECK_EQ(bytes, 121 * 256);                          // a copia espelhada foi usada e a principal restaurada
        std::vector<uint8_t> rr(512);
        CHECK_EQ(c2.ReadWriteFile(pf2, 0, kRead, 0, 512, rr.data()), 0);
        CHECK(rr[0] == 0x5A && rr[511] == 0x5A);
    }
    // --- cadeia de arquivo corrompida (aponta para pagina invalida): o verificador remove a nota ---
    {
        Pak p2; p2.plugged = true; p2.mem = pak.mem;
        const int sp = (((int)p2.mem[(24 + fno) * 32 + 6] << 8) | p2.mem[(24 + fno) * 32 + 7]) & 0xFF;   // pagina inicial
        // zera as DUAS tabelas na entrada da pagina inicial (aponta para 0) e conserta as somas via nova escrita simples
        for (int base : {8 * 32, 16 * 32}) { p2.mem[base + 2 * sp] = 0; p2.mem[base + 2 * sp + 1] = 0; }
        // recalcula a soma (bytes a partir da entrada 5)
        uint32_t s = 0; for (int i = 10; i < 256; i++) s += p2.mem[8 * 32 + i];
        p2.mem[8 * 32 + 1] = (uint8_t)s; p2.mem[16 * 32 + 1] = (uint8_t)s;
        Core c2(p2); Pfs pf2;
        c2.InitPak(pf2, 0);
        CHECK(pf2.status & kCorrupted);
        CHECK_EQ(c2.NumFiles(pf2, &maxf, &used), 0);
        CHECK_EQ(used, 0);
        CHECK_EQ(c2.FreeBlocks(pf2, &bytes), 0);
        CHECK_EQ(bytes, 31488);
    }
    // --- troca de pak: ID diferente -> NEW_PACK ---
    {
        Pak p2; p2.plugged = true; p2.mem = pak.mem;
        Pfs pf2; Core c2(p2);
        CHECK_EQ(c2.InitPak(pf2, 0), 0);
        p2.count = 0x77777;                                  // o campo `random` do ID vem de osGetCount: outro pak = outro ID
        FormatPak(p2);
        CHECK_EQ(c2.FreeBlocks(pf2, &bytes), kNewPack);
        CHECK_EQ(c2.AllocateFile(pf2, kCompany, kGame, gname, ext, 256, &fno), kNewPack);
    }
    // --- pak removido no meio ---
    {
        Pak p2; p2.plugged = true; p2.mem = pak.mem;
        Pfs pf2; Core c2(p2);
        CHECK_EQ(c2.InitPak(pf2, 0), 0);
        p2.plugged = false;
        CHECK_EQ(c2.FreeBlocks(pf2, &bytes), kNoPack);
    }
    // --- sem InitPak: status nao inicializado ---
    {
        Pak p2; p2.plugged = true; p2.mem = pak.mem;
        Pfs pf2; Core c2(p2);
        CHECK_EQ(c2.FreeBlocks(pf2, &bytes), kInvalid);
        CHECK_EQ(c2.FileState(pf2, 0, &st), kInvalid);
    }
    std::printf("%d verificacoes, %d falhas\n", checks, fails);
    return fails ? 1 : 0;
}
