// Controller Pak virtual (ver wdc_pak.h).
//
// Convencoes: os argumentos seguem a ABI o32 (a0..a3 = r4..r7; do quinto em diante na pilha, sp+0x10...). A memoria do jogo e acessada
// pelos macros MEM_* do N64Recomp com enderecos KSEG0 estendidos com sinal. Layout do OSPfs na memoria do jogo (libultra 2.0I, 0x68 bytes):
//   +0 status  +4 queue  +8 channel  +0xC id[32]  +0x2C label[32]  +0x4C version  +0x50 dir_size  +0x54 inode_table  +0x58 minode_table
//   +0x5C dir_table  +0x60 inode_start_page  +0x64 banks  +0x65 activebank
// OSPfsState (0x20 bytes): +0 file_size  +4 game_code  +8 company_code(u16)  +0xA ext_name[4]  +0xE game_name[16].
#include "wdc_pak.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include "wdc_pfs.h"

extern double wdc_now_s();

namespace {
std::mutex g_mu;
wdcpfs::Pak g_pak;
std::string g_path;
bool g_enabled = false;
uint64_t g_calls = 0, g_writes = 0;
const bool g_log = std::getenv("WDC_PAKLOG") != nullptr;

const char* err_name(int e) {
    static const char* n[] = {"ok", "NOPACK", "NEW_PACK", "INCONSISTENT", "CONTRFAIL", "INVALID", "BAD_DATA", "DATA_FULL", "DIR_FULL", "EXIST", "ID_FATAL", "DEVICE"};
    return e >= 0 && e < 12 ? n[e] : "?";
}

void save_locked() {
    if (!g_enabled || !g_pak.dirty) return;
    const std::string tmp = g_path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write((const char*)g_pak.mem.data(), (std::streamsize)g_pak.mem.size());
        if (!f) { std::printf("[pak] erro ao gravar %s\n", tmp.c_str()); return; }
    }
    if (!MoveFileExA(tmp.c_str(), g_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { std::printf("[pak] erro ao trocar %s\n", g_path.c_str()); return; }
    g_pak.dirty = false;
    g_writes++;
}

// ---- acesso a memoria do jogo ----
void load_pfs(uint8_t* rdram, gpr a, wdcpfs::Pfs& p) {
    p.status = MEM_W(0, a);
    p.channel = MEM_W(8, a);
    for (int i = 0; i < 32; i++) { p.id[i] = MEM_BU(0xC + i, a); p.label[i] = MEM_BU(0x2C + i, a); }
    p.version = MEM_W(0x4C, a);
    p.dir_size = MEM_W(0x50, a);
    p.inode_table = MEM_W(0x54, a);
    p.minode_table = MEM_W(0x58, a);
    p.dir_table = MEM_W(0x5C, a);
    p.inode_start_page = MEM_W(0x60, a);
    p.banks = MEM_BU(0x64, a);
    p.activebank = MEM_BU(0x65, a);
}

void store_pfs(uint8_t* rdram, gpr a, const wdcpfs::Pfs& p, bool all) {
    MEM_W(0, a) = p.status;
    MEM_W(8, a) = p.channel;
    MEM_BU(0x65, a) = p.activebank;
    if (!all) return;
    for (int i = 0; i < 32; i++) { MEM_BU(0xC + i, a) = p.id[i]; MEM_BU(0x2C + i, a) = p.label[i]; }
    MEM_W(0x4C, a) = p.version;
    MEM_W(0x50, a) = p.dir_size;
    MEM_W(0x54, a) = p.inode_table;
    MEM_W(0x58, a) = p.minode_table;
    MEM_W(0x5C, a) = p.dir_table;
    MEM_W(0x60, a) = p.inode_start_page;
    MEM_BU(0x64, a) = p.banks;
}

gpr arg(uint8_t* rdram, recomp_context* ctx, int n) { return n < 4 ? (&ctx->r4)[n] : (gpr)(int64_t)MEM_W(n * 4, ctx->r29); }

bool read_bytes(uint8_t* rdram, gpr a, uint8_t* out, int n) {
    if (a == 0) return false;
    for (int i = 0; i < n; i++) out[i] = MEM_BU(i, a);
    return true;
}

void finish(recomp_context* ctx, const char* what, int ret) {
    ctx->r2 = (gpr)(int32_t)ret;
    if (g_log) std::printf("[pak %7.2fs] %s -> %s\n", wdc_now_s(), what, err_name(ret));
}
}  // namespace

bool wdc_pak_init(const char* path) {
    std::lock_guard<std::mutex> l(g_mu);
    g_path = path;
    g_pak = wdcpfs::Pak{};
    g_pak.count = (uint32_t)GetTickCount64();
    bool have = false;
    {
        std::ifstream f(g_path, std::ios::binary | std::ios::ate);
        if (f && (size_t)f.tellg() == wdcpfs::kPakBytes) {
            f.seekg(0);
            f.read((char*)g_pak.mem.data(), (std::streamsize)g_pak.mem.size());
            have = (bool)f;
        }
    }
    g_pak.plugged = true;
    if (!have) {
        wdcpfs::FormatPak(g_pak);
        g_enabled = true;
        save_locked();
        std::printf("[pak] novo Controller Pak de 32 KB criado em %s\n", g_path.c_str());
    } else {
        g_enabled = true;
        std::printf("[pak] Controller Pak carregado de %s\n", g_path.c_str());
    }
    g_pak.dirty = false;
    return g_enabled;
}

bool wdc_pak_enabled() { return g_enabled; }

void wdc_pak_flush() {
    std::lock_guard<std::mutex> l(g_mu);
    save_locked();
    if (g_enabled) std::printf("[pak] chamadas osPfs*: %llu; gravacoes da imagem: %llu\n", (unsigned long long)g_calls, (unsigned long long)g_writes);
}

// ---------------------------------------------------------------------------------------------
// Hooks osPfs*
// ---------------------------------------------------------------------------------------------
extern "C" void wdc_hook_osPfsInitPak(uint8_t* rdram, recomp_context* ctx) {
    const gpr mq = ctx->r4, pfs_a = ctx->r5;
    const int channel = (int)ctx->r6;
    std::lock_guard<std::mutex> l(g_mu);
    g_calls++;
    if (!g_enabled || channel != 0 || pfs_a == 0) { finish(ctx, "osPfsInitPak", wdcpfs::kNoPack); return; }
    wdcpfs::Pfs pfs;
    wdcpfs::Core core(g_pak);
    const int ret = core.InitPak(pfs, channel);
    MEM_W(4, pfs_a) = (int32_t)mq;                 // pfs->queue
    store_pfs(rdram, pfs_a, pfs, pfs.banks != 0);
    save_locked();
    finish(ctx, "osPfsInitPak", ret);
}

extern "C" void wdc_hook_osPfsRepairId(uint8_t* rdram, recomp_context* ctx) {
    const gpr pfs_a = ctx->r4;
    std::lock_guard<std::mutex> l(g_mu);
    g_calls++;
    if (!g_enabled || pfs_a == 0) { finish(ctx, "osPfsRepairId", wdcpfs::kNoPack); return; }
    wdcpfs::Pfs pfs;
    load_pfs(rdram, pfs_a, pfs);
    wdcpfs::Core core(g_pak);
    const int ret = core.RepairId(pfs);
    store_pfs(rdram, pfs_a, pfs, true);
    save_locked();
    finish(ctx, "osPfsRepairId", ret);
}

extern "C" void wdc_hook_osPfsFileState(uint8_t* rdram, recomp_context* ctx) {
    const gpr pfs_a = ctx->r4, st_a = ctx->r6;
    const int file_no = (int)ctx->r5;
    std::lock_guard<std::mutex> l(g_mu);
    g_calls++;
    if (!g_enabled || pfs_a == 0) { finish(ctx, "osPfsFileState", wdcpfs::kNoPack); return; }
    wdcpfs::Pfs pfs;
    load_pfs(rdram, pfs_a, pfs);
    wdcpfs::Core core(g_pak);
    wdcpfs::FileState st;
    const int ret = core.FileState(pfs, file_no, &st);
    store_pfs(rdram, pfs_a, pfs, false);
    if (ret == 0 && st_a != 0) {
        MEM_W(0, st_a) = (int32_t)st.file_size;
        MEM_W(4, st_a) = (int32_t)st.game_code;
        MEM_HU(8, st_a) = st.company_code;
        for (int i = 0; i < 4; i++) MEM_BU(0xA + i, st_a) = (uint8_t)st.ext_name[i];
        for (int i = 0; i < 16; i++) MEM_BU(0xE + i, st_a) = (uint8_t)st.game_name[i];
    }
    save_locked();
    finish(ctx, "osPfsFileState", ret);
}

extern "C" void wdc_hook_osPfsFindFile(uint8_t* rdram, recomp_context* ctx) {
    const gpr pfs_a = ctx->r4, name_a = ctx->r7, ext_a = arg(rdram, ctx, 4), fno_a = arg(rdram, ctx, 5);
    const uint16_t company = (uint16_t)ctx->r5;
    const uint32_t game_code = (uint32_t)ctx->r6;
    std::lock_guard<std::mutex> l(g_mu);
    g_calls++;
    if (!g_enabled || pfs_a == 0) { finish(ctx, "osPfsFindFile", wdcpfs::kNoPack); return; }
    wdcpfs::Pfs pfs;
    load_pfs(rdram, pfs_a, pfs);
    uint8_t name[16], ext[4];
    const bool has_name = read_bytes(rdram, name_a, name, 16), has_ext = read_bytes(rdram, ext_a, ext, 4);
    wdcpfs::Core core(g_pak);
    int fno = -1;
    const int ret = core.FindFile(pfs, company, game_code, has_name ? name : nullptr, has_ext ? ext : nullptr, &fno);
    store_pfs(rdram, pfs_a, pfs, false);
    if (fno_a != 0) MEM_W(0, fno_a) = fno;
    save_locked();
    finish(ctx, "osPfsFindFile", ret);
}

extern "C" void wdc_hook_osPfsAllocateFile(uint8_t* rdram, recomp_context* ctx) {
    const gpr pfs_a = ctx->r4, name_a = ctx->r7, ext_a = arg(rdram, ctx, 4), fno_a = arg(rdram, ctx, 6);
    const uint16_t company = (uint16_t)ctx->r5;
    const uint32_t game_code = (uint32_t)ctx->r6;
    const int size = (int)arg(rdram, ctx, 5);
    std::lock_guard<std::mutex> l(g_mu);
    g_calls++;
    if (!g_enabled || pfs_a == 0) { finish(ctx, "osPfsAllocateFile", wdcpfs::kNoPack); return; }
    wdcpfs::Pfs pfs;
    load_pfs(rdram, pfs_a, pfs);
    uint8_t name[16] = {}, ext[4] = {};
    read_bytes(rdram, name_a, name, 16);
    read_bytes(rdram, ext_a, ext, 4);
    wdcpfs::Core core(g_pak);
    int fno = -1;
    const int ret = core.AllocateFile(pfs, company, game_code, name, ext, size, &fno);
    store_pfs(rdram, pfs_a, pfs, false);
    if (fno_a != 0) MEM_W(0, fno_a) = fno;
    save_locked();
    if (g_log) std::printf("[pak] AllocateFile \"%.16s\".\"%.4s\" %d bytes -> arquivo %d\n", name, ext, size, fno);
    finish(ctx, "osPfsAllocateFile", ret);
}

extern "C" void wdc_hook_osPfsDeleteFile(uint8_t* rdram, recomp_context* ctx) {
    const gpr pfs_a = ctx->r4, name_a = ctx->r7, ext_a = arg(rdram, ctx, 4);
    const uint16_t company = (uint16_t)ctx->r5;
    const uint32_t game_code = (uint32_t)ctx->r6;
    std::lock_guard<std::mutex> l(g_mu);
    g_calls++;
    if (!g_enabled || pfs_a == 0) { finish(ctx, "osPfsDeleteFile", wdcpfs::kNoPack); return; }
    wdcpfs::Pfs pfs;
    load_pfs(rdram, pfs_a, pfs);
    uint8_t name[16] = {}, ext[4] = {};
    read_bytes(rdram, name_a, name, 16);
    read_bytes(rdram, ext_a, ext, 4);
    wdcpfs::Core core(g_pak);
    const int ret = core.DeleteNote(pfs, company, game_code, name, ext);
    store_pfs(rdram, pfs_a, pfs, false);
    save_locked();
    finish(ctx, "osPfsDeleteFile", ret);
}

extern "C" void wdc_hook_osPfsReadWriteFile(uint8_t* rdram, recomp_context* ctx) {
    const gpr pfs_a = ctx->r4, buf_a = arg(rdram, ctx, 5);
    const int file_no = (int)ctx->r5;
    const uint8_t flag = (uint8_t)ctx->r6;
    const int offset = (int)ctx->r7, size = (int)arg(rdram, ctx, 4);
    std::lock_guard<std::mutex> l(g_mu);
    g_calls++;
    if (!g_enabled || pfs_a == 0) { finish(ctx, "osPfsReadWriteFile", wdcpfs::kNoPack); return; }
    wdcpfs::Pfs pfs;
    load_pfs(rdram, pfs_a, pfs);
    wdcpfs::Core core(g_pak);
    int ret;
    uint64_t hash = 1469598103934665603ull;   // FNV-1a dos dados (log WDC_PAKLOG: confere o que foi gravado com o que foi lido depois)
    if (size <= 0 || size > 0x100000 || buf_a == 0) {
        ret = wdcpfs::kInvalid;
    } else {
        std::vector<uint8_t> data((size_t)size);
        if (flag == wdcpfs::kWrite) for (int i = 0; i < size; i++) data[(size_t)i] = MEM_BU(i, buf_a);
        ret = core.ReadWriteFile(pfs, file_no, flag, offset, size, data.data());
        if (flag == wdcpfs::kRead && ret == 0) for (int i = 0; i < size; i++) MEM_BU(i, buf_a) = data[(size_t)i];
        for (uint8_t b : data) { hash ^= b; hash *= 1099511628211ull; }
    }
    store_pfs(rdram, pfs_a, pfs, false);
    save_locked();
    if (g_log) std::printf("[pak] %s arquivo %d offset %d tamanho %d dados=%016llx\n", flag == wdcpfs::kWrite ? "escrita" : "leitura", file_no, offset, size, (unsigned long long)hash);
    finish(ctx, "osPfsReadWriteFile", ret);
}

extern "C" void wdc_hook_osPfsFreeBlocks(uint8_t* rdram, recomp_context* ctx) {
    const gpr pfs_a = ctx->r4, out_a = ctx->r5;
    std::lock_guard<std::mutex> l(g_mu);
    g_calls++;
    if (!g_enabled || pfs_a == 0) { finish(ctx, "osPfsFreeBlocks", wdcpfs::kNoPack); return; }
    wdcpfs::Pfs pfs;
    load_pfs(rdram, pfs_a, pfs);
    wdcpfs::Core core(g_pak);
    int bytes = 0;
    const int ret = core.FreeBlocks(pfs, &bytes);
    store_pfs(rdram, pfs_a, pfs, false);
    if (ret == 0 && out_a != 0) MEM_W(0, out_a) = bytes;
    save_locked();
    finish(ctx, "osPfsFreeBlocks", ret);
}

extern "C" void wdc_hook_osPfsNumFiles(uint8_t* rdram, recomp_context* ctx) {
    const gpr pfs_a = ctx->r4, max_a = ctx->r5, used_a = ctx->r6;
    std::lock_guard<std::mutex> l(g_mu);
    g_calls++;
    if (!g_enabled || pfs_a == 0) {
        if (max_a) MEM_W(0, max_a) = 0;
        if (used_a) MEM_W(0, used_a) = 0;
        finish(ctx, "osPfsNumFiles", wdcpfs::kNoPack);
        return;
    }
    wdcpfs::Pfs pfs;
    load_pfs(rdram, pfs_a, pfs);
    wdcpfs::Core core(g_pak);
    int mx = 0, used = 0;
    const int ret = core.NumFiles(pfs, &mx, &used);
    store_pfs(rdram, pfs_a, pfs, false);
    if (ret == 0) {
        if (max_a) MEM_W(0, max_a) = mx;
        if (used_a) MEM_W(0, used_a) = used;
    }
    save_locked();
    finish(ctx, "osPfsNumFiles", ret);
}

extern "C" void wdc_hook_osPfsChecker(uint8_t* rdram, recomp_context* ctx) {
    const gpr pfs_a = ctx->r4;
    std::lock_guard<std::mutex> l(g_mu);
    g_calls++;
    if (!g_enabled || pfs_a == 0) { finish(ctx, "osPfsChecker", wdcpfs::kNoPack); return; }
    wdcpfs::Pfs pfs;
    load_pfs(rdram, pfs_a, pfs);
    wdcpfs::Core core(g_pak);
    const int ret = core.Checker(pfs);
    store_pfs(rdram, pfs_a, pfs, true);
    save_locked();
    finish(ctx, "osPfsChecker", ret);
}
