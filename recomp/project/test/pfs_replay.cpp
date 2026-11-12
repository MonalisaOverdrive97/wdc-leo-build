// Executa um roteiro de operacoes PFS sobre wdc_pfs.cpp e imprime, por operacao, o codigo de retorno, as saidas e um hash da imagem.
// O mesmo roteiro e executado pelo codigo ORIGINAL da libultra do jogo (Unicorn) em tools/recomp/difftest_pfs.py; as saidas devem ser iguais.
// Uso: pfs_replay <roteiro.txt> [imagem_inicial.bin]   (sem imagem: pak em branco; "factory" no roteiro = pak de fabrica)
// Roteiro (uma operacao por linha; hex sem prefixo):
//   image zero|factory | initpak | repairid | alloc CC GGGGGGGG nome ext tamanho | find CC GGGGGGGG nome|- ext|- | delete CC GGGGGGGG nome ext
//   state N | rw N flag offset tamanho [hexdados] | free | numfiles | checker | corrupt bloco byte xor | unplug | plug | count N
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "wdc_pfs.h"

using namespace wdcpfs;

static uint64_t fnv(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
static void name16(uint8_t* out, const std::string& s) { std::memset(out, 0, 16); std::memcpy(out, s.data(), s.size() < 16 ? s.size() : 16); }
static void ext4(uint8_t* out, const std::string& s) { std::memset(out, 0, 4); std::memcpy(out, s.data(), s.size() < 4 ? s.size() : 4); }
static std::vector<uint8_t> unhex(const std::string& h) {
    std::vector<uint8_t> v;
    for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)std::stoi(h.substr(i, 2), nullptr, 16));
    return v;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "uso: pfs_replay roteiro.txt [imagem.bin] | pfs_replay --factory saida.bin\n"); return 2; }
    if (std::string(argv[1]) == "--factory" && argc > 2) {   // grava a imagem do pak de fabrica (para o teste diferencial)
        Pak fp;
        FormatPak(fp);
        std::ofstream o(argv[2], std::ios::binary);
        o.write((const char*)fp.mem.data(), (std::streamsize)fp.mem.size());
        return 0;
    }
    Pak pak;
    pak.plugged = true;
    if (argc > 2) {
        std::ifstream f(argv[2], std::ios::binary);
        std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (d.size() == (size_t)kPakBytes) std::memcpy(pak.mem.data(), d.data(), d.size());
    }
    Core core(pak);
    Pfs pfs;
    std::ifstream in(argv[1]);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string op;
        ss >> op;
        std::string out;
        char buf[256];
        int ret = 0;
        auto parse_cc = [&](uint16_t& cc, uint32_t& gc) { std::string a, b; ss >> a >> b; cc = (uint16_t)std::stoul(a, nullptr, 16); gc = (uint32_t)std::stoul(b, nullptr, 16); };
        if (op == "image") {
            std::string k; ss >> k;
            if (k == "factory") FormatPak(pak); else pak.mem.fill(0);
            pak.plugged = true;
            pfs = Pfs{};
        } else if (op == "initpak") { ret = core.InitPak(pfs, 0); }
        else if (op == "repairid") { ret = core.RepairId(pfs); }
        else if (op == "checker") { ret = core.Checker(pfs); }
        else if (op == "alloc") {
            uint16_t cc; uint32_t gc; std::string nm, ex; int size;
            parse_cc(cc, gc); ss >> nm >> ex >> size;
            uint8_t a[16], e[4]; name16(a, nm); ext4(e, ex);
            int fno = -7;
            ret = core.AllocateFile(pfs, cc, gc, a, e, size, &fno);
            std::snprintf(buf, sizeof buf, " file=%d", fno); out = buf;
        } else if (op == "find") {
            uint16_t cc; uint32_t gc; std::string nm, ex;
            parse_cc(cc, gc); ss >> nm >> ex;
            uint8_t a[16], e[4]; name16(a, nm == "-" ? "" : nm); ext4(e, ex == "-" ? "" : ex);
            int fno = -7;
            ret = core.FindFile(pfs, cc, gc, nm == "-" ? nullptr : a, ex == "-" ? nullptr : e, &fno);
            std::snprintf(buf, sizeof buf, " file=%d", fno); out = buf;
        } else if (op == "delete") {
            uint16_t cc; uint32_t gc; std::string nm, ex;
            parse_cc(cc, gc); ss >> nm >> ex;
            uint8_t a[16], e[4]; name16(a, nm); ext4(e, ex);
            ret = core.DeleteNote(pfs, cc, gc, a, e);
        } else if (op == "state") {
            int fno; ss >> fno;
            FileState st;
            ret = core.FileState(pfs, fno, &st);
            if (ret == 0) {
                std::snprintf(buf, sizeof buf, " size=%u company=%04X game=%08X name=%016llx ext=%08x", st.file_size, st.company_code, st.game_code,
                              (unsigned long long)fnv((const uint8_t*)st.game_name, 16), (unsigned)fnv((const uint8_t*)st.ext_name, 4));
                out = buf;
            }
        } else if (op == "rw") {
            int fno, flag, off, size; std::string hex;
            ss >> fno >> flag >> off >> size >> hex;
            std::vector<uint8_t> data(size > 0 ? (size_t)size : 0);
            if (flag == kWrite && hex != "-") data = unhex(hex);
            if (size <= 0) data.resize(1);
            ret = core.ReadWriteFile(pfs, fno, (uint8_t)flag, off, size, data.data());
            if (flag == kRead && ret == 0) { std::snprintf(buf, sizeof buf, " data=%016llx", (unsigned long long)fnv(data.data(), (size_t)size)); out = buf; }
        } else if (op == "free") {
            int bytes = -1; ret = core.FreeBlocks(pfs, &bytes);
            std::snprintf(buf, sizeof buf, " bytes=%d", bytes); out = buf;
        } else if (op == "numfiles") {
            int mx = -1, used = -1; ret = core.NumFiles(pfs, &mx, &used);
            std::snprintf(buf, sizeof buf, " max=%d used=%d", mx, used); out = buf;
        } else if (op == "corrupt") {
            int blk, off, x; ss >> blk >> off >> x;
            pak.mem[(size_t)blk * 32 + (size_t)off] ^= (uint8_t)x;
        } else if (op == "unplug") { pak.plugged = false; }
        else if (op == "plug") { pak.plugged = true; }
        else if (op == "count") { unsigned c; ss >> c; pak.count = c; }
        std::printf("%d %s ret=%d%s pfs=%d,%d,%d,%d img=%016llx\n", n++, op.c_str(), ret, out.c_str(), pfs.status, pfs.activebank, pfs.banks, pfs.dir_size,
                    (unsigned long long)fnv(pak.mem.data(), pak.mem.size()));
        if (const char* st = std::getenv("PFS_STOP")) {   // PFS_STOP=n PFS_DUMP=arq: grava a imagem depois da operacao n e para
            if (n - 1 == std::atoi(st)) {
                if (const char* dp = std::getenv("PFS_DUMP")) { std::ofstream o(dp, std::ios::binary); o.write((const char*)pak.mem.data(), (std::streamsize)pak.mem.size()); }
                return 0;
            }
        }
    }
    return 0;
}
