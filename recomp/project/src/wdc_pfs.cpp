// PFS do Controller Pak (ver wdc_pfs.h). Algoritmos e codigos de retorno seguem thirdparty/libreultra/src/io (2.0I).
#include "wdc_pfs.h"

#include <cstring>

namespace wdcpfs {

#define RET_IF(x) do { int r__ = (x); if (r__ != 0) return r__; } while (0)

// ---------------------------------------------------------------------------------------------
// Dispositivo
// ---------------------------------------------------------------------------------------------
int Pak::read_block(uint16_t addr, uint8_t* out32) {
    if (!plugged) return kNoPack;
    if (addr >= kPakBytes / kBlockSize) { std::memset(out32, 0, kBlockSize); return 0; }
    std::memcpy(out32, &mem[(size_t)addr * kBlockSize], kBlockSize);
    return 0;
}

int Pak::write_block(uint16_t addr, const uint8_t* in32) {
    if (!plugged) return kNoPack;
    if (addr >= kPakBytes / kBlockSize) return 0;   // inclui o seletor de banco (bloco 1024): num pak de 32 KB e ignorado
    if (std::memcmp(&mem[(size_t)addr * kBlockSize], in32, kBlockSize) != 0) {   // so marca para gravar em disco se o conteudo mudou
        std::memcpy(&mem[(size_t)addr * kBlockSize], in32, kBlockSize);
        dirty = true;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// (De)serializacao big-endian
// ---------------------------------------------------------------------------------------------
namespace {
inline uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
inline uint64_t rd64(const uint8_t* p) { return ((uint64_t)rd32(p) << 32) | rd32(p + 4); }
inline void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
inline void wr32(uint8_t* p, uint32_t v) { wr16(p, (uint16_t)(v >> 16)); wr16(p + 2, (uint16_t)v); }
inline void wr64(uint8_t* p, uint64_t v) { wr32(p, (uint32_t)(v >> 32)); wr32(p + 4, (uint32_t)v); }
}  // namespace

uint16_t Core::SumCalc(const uint8_t* p, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i++) sum = (sum + p[i]) & 0xFFFF;
    return (uint16_t)sum;
}

void Core::IdCheckSum(const uint8_t* id32, uint16_t* sum, uint16_t* isum) {
    uint16_t s = 0, is = 0;
    for (int j = 0; j < 28; j += 2) {
        const uint16_t d = rd16(id32 + j);
        s = (uint16_t)(s + d);
        is = (uint16_t)(is + (uint16_t)~d);
    }
    *sum = s;
    *isum = is;
}

void Core::PackIdToBytes(const PackId& id, uint8_t out[32]) {
    wr32(out, id.repaired); wr32(out + 4, id.random); wr64(out + 8, id.serial_mid); wr64(out + 0x10, id.serial_low);
    wr16(out + 0x18, id.deviceid); out[0x1A] = id.banks; out[0x1B] = id.version; wr16(out + 0x1C, id.checksum); wr16(out + 0x1E, id.inverted);
}

Core::PackId Core::PackIdFromBytes(const uint8_t in[32]) {
    PackId id;
    id.repaired = rd32(in); id.random = rd32(in + 4); id.serial_mid = rd64(in + 8); id.serial_low = rd64(in + 0x10);
    id.deviceid = rd16(in + 0x18); id.banks = in[0x1A]; id.version = in[0x1B]; id.checksum = rd16(in + 0x1C); id.inverted = rd16(in + 0x1E);
    return id;
}

void Core::DirToBytes(const Dir& d, uint8_t out[32]) {
    wr32(out, d.game_code); wr16(out + 4, d.company_code); wr16(out + 6, d.start_page); out[8] = d.status; out[9] = (uint8_t)d.reserved;
    wr16(out + 0xA, d.data_sum); std::memcpy(out + 0xC, d.ext_name, 4); std::memcpy(out + 0x10, d.game_name, 16);
}

Core::Dir Core::DirFromBytes(const uint8_t in[32]) {
    Dir d;
    d.game_code = rd32(in); d.company_code = rd16(in + 4); d.start_page = rd16(in + 6); d.status = in[8]; d.reserved = (int8_t)in[9];
    d.data_sum = rd16(in + 0xA); std::memcpy(d.ext_name, in + 0xC, 4); std::memcpy(d.game_name, in + 0x10, 16);
    return d;
}

void Core::InodeToBytes(const Inode& i, uint8_t out[256]) { for (int k = 0; k < 128; k++) wr16(out + 2 * k, i.e[k]); }
void Core::InodeFromBytes(const uint8_t in[256], Inode* i) { for (int k = 0; k < 128; k++) i->e[k] = rd16(in + 2 * k); }

// ---------------------------------------------------------------------------------------------
// Camada de baixo nivel (contpfs.c)
// ---------------------------------------------------------------------------------------------
int Core::SelectBank(Pfs& pfs) {
    uint8_t temp[kBlockSize];
    std::memset(temp, pfs.activebank, sizeof temp);
    return pak_.write_block(1024, temp);
}

int Core::ActiveBankZero(Pfs& pfs) {
    if (pfs.activebank != 0) {
        pfs.activebank = 0;
        RET_IF(SelectBank(pfs));
    }
    return 0;
}

void Core::FillPfsFromId(Pfs& pfs, const uint8_t id32[32]) {
    std::memcpy(pfs.id, id32, 32);
    pfs.version = id32[0x1B];
    pfs.banks = id32[0x1A];
    pfs.inode_start_page = pfs.banks * 2 + 3;
    pfs.dir_size = 16;
    pfs.inode_table = 8;
    pfs.minode_table = pfs.banks * kOnePage + 8;
    pfs.dir_table = pfs.minode_table + pfs.banks * kOnePage;
}

int Core::RepairPackId(Pfs& pfs, const uint8_t bad32[32], uint8_t new32[32]) {
    const PackId bad = PackIdFromBytes(bad32);
    PackId nid{};
    uint8_t temp[32], comp[32];
    int i, j;
    RET_IF(ActiveBankZero(pfs));
    nid.repaired = 0xFFFFFFFFu;
    nid.random = pak_.count++;
    nid.serial_mid = bad.serial_mid;
    nid.serial_low = bad.serial_low;
    for (j = 0; j < kMaxBanks;) {
        pfs.activebank = (uint8_t)j;
        RET_IF(SelectBank(pfs));
        RET_IF(pak_.read_block(0, temp));
        temp[0] = (uint8_t)(j | 0x80);
        for (i = 1; i < 32; i++) temp[i] = (uint8_t)~temp[i];
        RET_IF(pak_.write_block(0, temp));
        RET_IF(pak_.read_block(0, comp));
        for (i = 0; i < 32; i++) if (comp[i] != temp[i]) break;
        if (i != 32) break;
        if (j > 0) {
            pfs.activebank = 0;
            RET_IF(SelectBank(pfs));
            RET_IF(pak_.read_block(0, temp));
            if (temp[0] != 128) break;
        }
        j++;
    }
    pfs.activebank = 0;
    RET_IF(SelectBank(pfs));
    const uint8_t mask = j > 0 ? 1 : 0;
    nid.deviceid = (uint16_t)((bad.deviceid & (uint16_t)~1) | mask);
    nid.banks = (uint8_t)j;
    nid.version = bad.version;
    PackIdToBytes(nid, new32);
    IdCheckSum(new32, &nid.checksum, &nid.inverted);
    PackIdToBytes(nid, new32);
    static const uint16_t index[4] = {1, 3, 4, 6};
    for (i = 0; i < 4; i++) RET_IF(pak_.write_block(index[i], new32));
    RET_IF(pak_.read_block(1, temp));
    if (std::memcmp(temp, new32, 32) != 0) return kIdFatal;
    return 0;
}

int Core::CheckPackId(Pfs& pfs, uint8_t id32[32]) {
    static const uint16_t index[4] = {1, 3, 4, 6};
    uint16_t sum, isum;
    int i, j;
    RET_IF(ActiveBankZero(pfs));
    for (i = 1; i < 4; i++) {
        RET_IF(pak_.read_block(index[i], id32));
        IdCheckSum(id32, &sum, &isum);
        if (rd16(id32 + 0x1C) == sum && rd16(id32 + 0x1E) == isum) break;
    }
    if (i == 4) return kIdFatal;
    for (j = 0; j < 4; j++)
        if (j != i) RET_IF(pak_.write_block(index[j], id32));
    return 0;
}

int Core::GetId(Pfs& pfs) {
    uint8_t temp[32], newid[32];
    uint16_t sum, isum;
    const uint8_t* id = temp;
    RET_IF(ActiveBankZero(pfs));
    RET_IF(pak_.read_block(1, temp));
    IdCheckSum(temp, &sum, &isum);
    if (rd16(temp + 0x1C) != sum || rd16(temp + 0x1E) != isum) {
        const int ret = CheckPackId(pfs, temp);
        if (ret == kIdFatal) {
            RET_IF(RepairPackId(pfs, temp, newid));
            id = newid;
        } else if (ret != 0) {
            return ret;
        }
    }
    if ((rd16(id + 0x18) & 1) == 0) {
        uint8_t again[32];
        RET_IF(RepairPackId(pfs, id, again));
        std::memcpy(newid, again, 32);
        id = newid;
        if ((rd16(id + 0x18) & 1) == 0) return kDevice;
    }
    FillPfsFromId(pfs, id);
    RET_IF(pak_.read_block(7, pfs.label));
    return 0;
}

int Core::CheckId(Pfs& pfs) {
    uint8_t temp[32];
    RET_IF(ActiveBankZero(pfs));
    int ret = pak_.read_block(1, temp);
    if (ret != 0) {
        if (ret != 2) return ret;
        RET_IF(pak_.read_block(1, temp));
    }
    if (std::memcmp(pfs.id, temp, 32) != 0) return kNewPack;
    return 0;
}

int Core::RWInode(Pfs& pfs, Inode* inode, uint8_t flag, uint8_t bank) {
    uint8_t buf[256];
    RET_IF(ActiveBankZero(pfs));
    const int offset = bank > 0 ? 1 : pfs.inode_start_page;
    auto checksum = [&](const Inode& in) {
        uint8_t b[256];
        InodeToBytes(in, b);
        return SumCalc(b + 2 * offset, 256 - 2 * offset);
    };
    if (flag == kWrite) inode->e[0] = (uint16_t)((inode->e[0] & 0xFF00) | (checksum(*inode) & 0xFF));
    if (flag == kWrite) InodeToBytes(*inode, buf);
    for (int j = 0; j < 8; j++) {
        if (flag == kWrite) {
            RET_IF(pak_.write_block((uint16_t)(pfs.inode_table + bank * 8 + j), buf + j * 32));
            RET_IF(pak_.write_block((uint16_t)(pfs.minode_table + bank * 8 + j), buf + j * 32));
        } else {
            RET_IF(pak_.read_block((uint16_t)(pfs.inode_table + bank * 8 + j), buf + j * 32));
        }
    }
    if (flag == kRead) {
        InodeFromBytes(buf, inode);
        const uint8_t sum = (uint8_t)checksum(*inode);
        if (sum != (uint8_t)(inode->e[0] & 0xFF)) {
            // a tabela principal nao confere: tenta a copia espelhada
            for (int j = 0; j < kOnePage; j++) pak_.read_block((uint16_t)(pfs.minode_table + bank * kOnePage + j), buf + j * 32);
            InodeFromBytes(buf, inode);
            if (sum != (uint8_t)(inode->e[0] & 0xFF)) return kInconsistent;
            for (int j = 0; j < kOnePage; j++) pak_.write_block((uint16_t)(pfs.inode_table + bank * kOnePage + j), buf + j * 32);
        } else {
            for (int j = 0; j < kOnePage; j++) pak_.write_block((uint16_t)(pfs.minode_table + bank * kOnePage + j), buf + j * 32);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------------------------
int Core::InitPak(Pfs& pfs, int channel) {
    uint8_t temp[32], newid[32];
    uint16_t sum, isum;
    if (!pak_.plugged) return kNoPack;
    pfs.channel = channel;
    pfs.status = 0;
    pfs.activebank = 0;
    RET_IF(SelectBank(pfs));
    RET_IF(pak_.read_block(1, temp));
    IdCheckSum(temp, &sum, &isum);
    const uint8_t* id = temp;
    if (rd16(temp + 0x1C) != sum || rd16(temp + 0x1E) != isum) RET_IF(CheckPackId(pfs, temp));
    if ((rd16(id + 0x18) & 1) == 0) {
        RET_IF(RepairPackId(pfs, id, newid));
        id = newid;
        if ((rd16(id + 0x18) & 1) == 0) return kDevice;
    }
    FillPfsFromId(pfs, id);
    RET_IF(pak_.read_block(7, pfs.label));
    const int ret = Checker(pfs);
    pfs.status |= kInitialized;
    return ret;
}

int Core::RepairId(Pfs& pfs) {
    uint8_t temp[32], newid[32];
    uint16_t sum, isum;
    RET_IF(ActiveBankZero(pfs));
    RET_IF(pak_.read_block(1, temp));
    IdCheckSum(temp, &sum, &isum);
    const uint8_t* id = temp;
    if (rd16(temp + 0x1C) != sum || rd16(temp + 0x1E) != isum) {
        const int ret = CheckPackId(pfs, temp);
        if (ret == kIdFatal) {
            RET_IF(RepairPackId(pfs, temp, newid));
            id = newid;
        } else if (ret != 0) {
            return ret;
        }
    }
    if ((rd16(id + 0x18) & 1) == 0) {
        uint8_t again[32];
        RET_IF(RepairPackId(pfs, id, again));
        std::memcpy(newid, again, 32);
        id = newid;
        if ((rd16(id + 0x18) & 1) == 0) return kDevice;
    }
    FillPfsFromId(pfs, id);
    RET_IF(pak_.read_block(7, pfs.label));
    return 0;
}

#define PFS_CHECK_STATUS do { if ((pfs.status & kInitialized) == 0) return kInvalid; } while (0)
#define PFS_CHECK_ID do { if (CheckId(pfs) == kNewPack) return kNewPack; } while (0)

int Core::FileState(Pfs& pfs, int file_no, wdcpfs::FileState* state) {
    uint8_t blk[32];
    if (file_no >= pfs.dir_size || file_no < 0) return kInvalid;
    PFS_CHECK_STATUS;
    PFS_CHECK_ID;
    RET_IF(ActiveBankZero(pfs));
    RET_IF(pak_.read_block((uint16_t)(pfs.dir_table + file_no), blk));
    const Dir dir = DirFromBytes(blk);
    if (dir.company_code == 0 || dir.game_code == 0) return kInvalid;
    if (dir.start_page < (uint16_t)pfs.inode_start_page) return kInconsistent;
    int pages = 0;
    uint8_t start_page = (uint8_t)(dir.start_page & 0xFF);
    uint8_t bank = (uint8_t)(dir.start_page >> 8);
    uint16_t next_page = 0;
    Inode inode;
    while (bank < pfs.banks) {
        RET_IF(RWInode(pfs, &inode, kRead, bank));
        next_page = inode.e[start_page];
        pages++;
        while (next_page >= (uint16_t)pfs.inode_start_page) {
            pages++;
            next_page = inode.e[next_page & 0xFF];
            if ((next_page >> 8) != bank) {
                bank = (uint8_t)(next_page >> 8);
                start_page = (uint8_t)(next_page & 0xFF);
                break;
            }
        }
        if (next_page == 1) break;
    }
    if (next_page != 1) return kInconsistent;
    state->file_size = (uint32_t)pages << 8;
    state->company_code = dir.company_code;
    state->game_code = dir.game_code;
    std::memcpy(state->game_name, dir.game_name, 16);
    std::memcpy(state->ext_name, dir.ext_name, 4);
    return 0;
}

int Core::FindFile(Pfs& pfs, uint16_t company, uint32_t game_code, const uint8_t* game_name, const uint8_t* ext_name, int* file_no) {
    uint8_t blk[32];
    PFS_CHECK_ID;
    for (int j = 0; j < pfs.dir_size; j++) {
        RET_IF(pak_.read_block((uint16_t)(pfs.dir_table + j), blk));
        const Dir dir = DirFromBytes(blk);
        if (dir.company_code == company && dir.game_code == game_code) {
            bool fail = false;
            if (game_name && std::memcmp(dir.game_name, game_name, 16) != 0) fail = true;
            if (ext_name && !fail && std::memcmp(dir.ext_name, ext_name, 4) != 0) fail = true;
            if (!fail) { *file_no = j; return 0; }
        }
    }
    *file_no = -1;
    return kInvalid;
}

int Core::ClearPage(Pfs& pfs, int page_no, uint8_t bank) {
    uint8_t zeros[32] = {};
    pfs.activebank = bank;
    RET_IF(SelectBank(pfs));
    int ret = 0;
    for (int i = 0; i < kOnePage; i++) {
        ret = pak_.write_block((uint16_t)(page_no * kOnePage + i), zeros);
        if (ret != 0) break;
    }
    pfs.activebank = 0;
    ret = SelectBank(pfs);
    return ret;
}

int Core::DeclearPage(Pfs& pfs, Inode* inode, int pages, int* first_page, uint8_t bank, int* decleared, int* last_page) {
    const int offset = bank > 0 ? 1 : pfs.inode_start_page;
    int j;
    for (j = offset; j < 128; j++) if (inode->e[j] == 3) break;
    if (j == 128) { *first_page = -1; return 0; }
    const int spage = j;
    *decleared = 1;
    int old_page = j++;
    while (pages > *decleared && j < 128) {
        if (inode->e[j] == 3) {
            inode->e[old_page] = (uint16_t)((bank << 8) | j);
            RET_IF(ClearPage(pfs, old_page, bank));
            old_page = j;
            (*decleared)++;
        }
        j++;
    }
    *first_page = spage;
    if (j == 128 && pages > *decleared) { *last_page = old_page; return 0; }
    inode->e[old_page] = 1;
    const int ret = ClearPage(pfs, old_page, bank);
    *last_page = 0;
    return ret;
}

int Core::FreeBlocks(Pfs& pfs, int* bytes_not_used) {
    Inode inode;
    int pages = 0;
    PFS_CHECK_STATUS;
    PFS_CHECK_ID;
    for (uint8_t bank = 0; bank < pfs.banks; bank++) {
        RET_IF(RWInode(pfs, &inode, kRead, bank));
        const int offset = bank > 0 ? 1 : pfs.inode_start_page;
        for (int j = offset; j < 128; j++) if (inode.e[j] == 3) pages++;
    }
    *bytes_not_used = pages * kOnePage * kBlockSize;
    return 0;
}

int Core::NumFiles(Pfs& pfs, int* max_files, int* files_used) {
    uint8_t blk[32];
    int files = 0;
    PFS_CHECK_STATUS;
    PFS_CHECK_ID;
    RET_IF(ActiveBankZero(pfs));
    for (int j = 0; j < pfs.dir_size; j++) {
        RET_IF(pak_.read_block((uint16_t)(pfs.dir_table + j), blk));
        const Dir dir = DirFromBytes(blk);
        if (dir.company_code != 0 && dir.game_code != 0) files++;
    }
    *files_used = files;
    *max_files = pfs.dir_size;
    return 0;
}

int Core::AllocateFile(Pfs& pfs, uint16_t company, uint32_t game_code, const uint8_t* game_name, const uint8_t* ext_name, int size_bytes, int* file_no) {
    Inode inode, backup;
    Dir dir{};
    int start_page = 0, decleared = 0, last_page = 0, old_last_page = 0, firsttime = 0, bytes = 0;
    uint8_t bank = 0, old_bank = 0;
    uint16_t fpage = 0;
    if (company == 0 || game_code == 0) return kInvalid;
    int pages = (size_bytes + 255) / (kBlockSize * kOnePage);
    if ((pfs.status & kInitialized) == 0) return kInvalid;
    PFS_CHECK_ID;
    int ret = FindFile(pfs, company, game_code, game_name, ext_name, file_no);
    if (ret != 0 && ret != kInvalid) return ret;
    if (*file_no != -1) return kExist;
    ret = FreeBlocks(pfs, &bytes);
    if (size_bytes > bytes) return kDataFull;
    if (pages == 0) return kInvalid;

    ret = FindFile(pfs, 0, 0, nullptr, nullptr, file_no);
    if (ret != 0 && ret != kInvalid) return ret;
    if (*file_no == -1) return kDirFull;

    std::memset(&backup, 0, sizeof backup);
    for (bank = 0; bank < pfs.banks; bank++) {
        RET_IF(RWInode(pfs, &inode, kRead, bank));
        RET_IF(DeclearPage(pfs, &inode, pages, &start_page, bank, &decleared, &last_page));
        if (start_page != -1) {
            if (firsttime == 0) {
                fpage = (uint16_t)((bank << 8) | (start_page & 0xFF));
            } else {
                backup.e[old_last_page] = (uint16_t)((bank << 8) | (start_page & 0xFF));
                RET_IF(RWInode(pfs, &backup, kWrite, old_bank));
            }
            for (int j = 0; j < 128; j++) backup.e[j] = inode.e[j];
            old_last_page = last_page;
            old_bank = bank;
            firsttime++;
            if (pages > decleared) {
                pages -= decleared;
            } else {
                pages = 0;
                break;
            }
        }
    }
    if (pages > 0 || start_page == -1) return kInconsistent;
    backup.e[old_last_page] = (uint16_t)((bank << 8) | (start_page & 0xFF));
    RET_IF(RWInode(pfs, &backup, kWrite, old_bank));
    dir.start_page = fpage;
    dir.company_code = company;
    dir.game_code = game_code;
    dir.data_sum = 0;
    dir.status = 0;
    dir.reserved = 0;
    std::memcpy(dir.game_name, game_name, 16);
    std::memcpy(dir.ext_name, ext_name, 4);
    uint8_t blk[32];
    DirToBytes(dir, blk);
    RET_IF(pak_.write_block((uint16_t)(*file_no + pfs.dir_table), blk));
    return 0;
}

int Core::BlockSum(Pfs& pfs, uint8_t page_no, uint16_t* sum, uint8_t bank) {
    uint8_t data[32];
    pfs.activebank = bank;
    RET_IF(SelectBank(pfs));
    for (int i = 0; i < kOnePage; i++) {
        const int ret = pak_.read_block((uint16_t)(page_no * kOnePage + i), data);
        if (ret != 0) { pfs.activebank = 0; SelectBank(pfs); return ret; }
        *sum = (uint16_t)(*sum + SumCalc(data, 32));
    }
    pfs.activebank = 0;
    return SelectBank(pfs);
}

int Core::ReleasePages(Pfs& pfs, Inode* inode, uint8_t start_page, uint16_t* sum, uint8_t bank, uint16_t* last_page, int flag) {
    uint16_t next_page = inode->e[start_page];
    int offset;
    if (next_page != 1) offset = (next_page >> 8) > 0 ? 1 : pfs.inode_start_page;
    else offset = bank > 0 ? 1 : pfs.inode_start_page;
    if ((next_page & 0xFF) < offset && next_page != 1) return kInconsistent;
    *last_page = next_page;
    if (flag == 1) inode->e[start_page] = 3;
    RET_IF(BlockSum(pfs, start_page, sum, bank));
    if (next_page == 1) return 0;
    while (next_page >= (uint16_t)pfs.inode_start_page) {
        const uint16_t old_page = next_page;
        next_page = inode->e[next_page & 0xFF];
        inode->e[old_page & 0xFF] = 3;
        RET_IF(BlockSum(pfs, (uint8_t)(old_page & 0xFF), sum, bank));
        if ((next_page >> 8) != bank) break;
    }
    if (next_page >= (uint16_t)pfs.inode_start_page && (next_page >> 8) == bank) inode->e[next_page & 0xFF] = 3;
    *last_page = next_page;
    return 0;
}

int Core::DeleteNote(Pfs& pfs, uint16_t company, uint32_t game_code, const uint8_t* game_name, const uint8_t* ext_name) {
    int file_no = 0;
    uint16_t sum = 0, last_page = 0;
    Inode inode;
    uint8_t blk[32];
    if (company == 0 || game_code == 0) return kInvalid;
    PFS_CHECK_STATUS;
    PFS_CHECK_ID;
    RET_IF(ActiveBankZero(pfs));
    RET_IF(FindFile(pfs, company, game_code, game_name, ext_name, &file_no));
    if (file_no == -1) return kInvalid;
    RET_IF(pak_.read_block((uint16_t)(pfs.dir_table + file_no), blk));
    Dir dir = DirFromBytes(blk);
    uint8_t startpage = (uint8_t)(dir.start_page & 0xFF);
    uint8_t bank;
    for (bank = (uint8_t)(dir.start_page >> 8); bank < pfs.banks;) {
        RET_IF(RWInode(pfs, &inode, kRead, bank));
        RET_IF(ReleasePages(pfs, &inode, startpage, &sum, bank, &last_page, 1));
        RET_IF(RWInode(pfs, &inode, kWrite, bank));
        if (last_page == 1) break;
        bank = (uint8_t)(last_page >> 8);
        startpage = (uint8_t)(last_page & 0xFF);
    }
    if (bank >= pfs.banks) return kInconsistent;
    dir.game_code = 0;
    dir.company_code = 0;
    dir.start_page = 0;
    dir.data_sum = 0;
    std::memset(dir.game_name, 0, 16);
    std::memset(dir.ext_name, 0, 4);
    dir.status = 0;
    DirToBytes(dir, blk);
    return pak_.write_block((uint16_t)(pfs.dir_table + file_no), blk);
}

int Core::GetNextPage(Pfs& pfs, uint8_t* bank, Inode* inode, uint16_t* page) {
    if (*bank != (*page >> 8)) {
        *bank = (uint8_t)(*page >> 8);
        RET_IF(RWInode(pfs, inode, kRead, *bank));
    }
    *page = inode->e[*page & 0xFF];
    if (*page < (uint16_t)pfs.inode_start_page || (*page >> 8) >= pfs.banks || (*page & 0xFF) == 0 || (*page & 0xFF) >= 128) {
        if (*page == 1) return kInvalid;
        return kInconsistent;
    }
    return 0;
}

int Core::ReadWriteFile(Pfs& pfs, int file_no, uint8_t flag, int offset, int size_bytes, uint8_t* data) {
    uint8_t blk[32];
    Inode inode;
    if (file_no >= pfs.dir_size || file_no < 0) return kInvalid;
    if (size_bytes <= 0 || (size_bytes & (kBlockSize - 1)) != 0) return kInvalid;
    if (offset < 0 || (offset & (kBlockSize - 1)) != 0) return kInvalid;
    PFS_CHECK_STATUS;
    PFS_CHECK_ID;
    RET_IF(ActiveBankZero(pfs));
    RET_IF(pak_.read_block((uint16_t)(pfs.dir_table + file_no), blk));
    Dir dir = DirFromBytes(blk);
    if (dir.company_code == 0 || dir.game_code == 0) return kInvalid;
    if (dir.start_page < (uint16_t)pfs.inode_start_page || (dir.start_page >> 8) >= pfs.banks || (dir.start_page & 0xFF) == 0 ||
        (dir.start_page & 0xFF) >= 128) {
        if (dir.start_page == 1) return kInvalid;
        return kInconsistent;
    }
    if (flag == kRead && (dir.status & 2) == 0) return kBadData;
    uint8_t bank = 0xFF;
    int cur_block = offset / kBlockSize;
    uint16_t cur_page = dir.start_page;
    while (cur_block >= kOnePage) {
        RET_IF(GetNextPage(pfs, &bank, &inode, &cur_page));
        cur_block -= kOnePage;
    }
    int siz_block = size_bytes / kBlockSize;
    uint8_t* buffer = data;
    while (siz_block > 0) {
        if (cur_block == kOnePage) {
            RET_IF(GetNextPage(pfs, &bank, &inode, &cur_page));
            cur_block = 0;
        }
        if (pfs.activebank != (cur_page >> 8)) {
            pfs.activebank = (uint8_t)(cur_page >> 8);
            RET_IF(SelectBank(pfs));
        }
        const uint16_t blockno = (uint16_t)((cur_page & 0xFF) * kOnePage + cur_block);
        const int ret = flag == kRead ? pak_.read_block(blockno, buffer) : pak_.write_block(blockno, buffer);
        if (ret != 0) return ret;
        buffer += kBlockSize;
        cur_block++;
        siz_block--;
    }
    if (flag == kWrite && (dir.status & 2) == 0) {
        dir.status |= 2;
        pfs.activebank = 0;
        RET_IF(SelectBank(pfs));
        DirToBytes(dir, blk);
        RET_IF(pak_.write_block((uint16_t)(pfs.dir_table + file_no), blk));
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Verificador (osPfsChecker): descarta arquivos com cadeia inconsistente e refaz a tabela de inodes so com as paginas alcancaveis
// ---------------------------------------------------------------------------------------------
int Core::CorruptedInit(Pfs& pfs, Cache* cache) {
    Inode tmp;
    std::memset(cache->map, 0, sizeof cache->map);
    cache->bank = 0xFF;
    for (uint8_t bank = 0; bank < pfs.banks; bank++) {
        const int offset = bank > 0 ? 1 : pfs.inode_start_page;
        const int ret = RWInode(pfs, &tmp, kRead, bank);
        if (ret != 0 && ret != kInconsistent) return ret;
        for (int i = offset; i < 128; i++) {
            const uint16_t tpage = tmp.e[i];
            if (tpage >= (uint16_t)pfs.inode_start_page && (tpage >> 8) != bank) {
                const int n = ((tpage & 0xFF) / 4) + (((tpage >> 8) % kOnePage) * kBlockSize);
                cache->map[n] |= (uint8_t)(1 << (bank % kOnePage));
            }
        }
    }
    return 0;
}

int Core::Corrupted(Pfs& pfs, uint16_t fpage, Cache* cache) {
    int hit = 0;
    const int n = ((fpage & 0xFF) / 4) + (((fpage >> 8) % 8) * kBlockSize);
    for (uint8_t bank = 0; bank < pfs.banks; bank++) {
        const int offset = bank > 0 ? 1 : pfs.inode_start_page;
        if (bank == (fpage >> 8) || (cache->map[n] & (1 << (bank % 8)))) {
            if (bank != cache->bank) {
                const int ret = RWInode(pfs, &cache->inode, kRead, bank);
                if (ret != 0 && ret != kInconsistent) return ret;
                cache->bank = bank;
            }
            for (int j = offset; hit < 2 && j < 128; j++) if (cache->inode.e[j] == fpage) hit++;
            if (1 < hit) return kNewPack;
        }
    }
    return hit;
}

int Core::Checker(Pfs& pfs) {
    uint8_t blk[32];
    Inode checked, tmp_inode;
    uint16_t file_next_node[16];
    Cache cache;
    int fixed = 0;
    int ret = CheckId(pfs);
    if (ret == kNewPack) ret = GetId(pfs);
    if (ret != 0) return ret;
    RET_IF(CorruptedInit(pfs, &cache));
    for (int j = 0; j < pfs.dir_size; j++) {
        RET_IF(pak_.read_block((uint16_t)(pfs.dir_table + j), blk));
        Dir tmp_dir = DirFromBytes(blk);
        if (tmp_dir.company_code != 0 && tmp_dir.game_code != 0) {
            uint16_t next_page = tmp_dir.start_page;
            int cc = 0, cl = 0;
            uint8_t bank = 255;
            while (next_page >= (uint16_t)pfs.inode_start_page && (next_page >> 8) < pfs.banks && (next_page & 0xFF) > 0 && (next_page & 0xFF) < 128) {
                if (bank != (next_page >> 8)) {
                    bank = (uint8_t)(next_page >> 8);
                    ret = RWInode(pfs, &tmp_inode, kRead, bank);
                    if (ret != 0 && ret != kInconsistent) return ret;
                }
                cc = Corrupted(pfs, next_page, &cache) - cl;
                if (cc != 0) break;
                cl = 1;
                next_page = tmp_inode.e[next_page & 0xFF];
            }
            if (cc == 0 && next_page == 1) continue;
        } else {
            if (tmp_dir.company_code == 0 && tmp_dir.game_code == 0) continue;
        }
        tmp_dir.company_code = 0;
        tmp_dir.game_code = 0;
        tmp_dir.start_page = 0;
        tmp_dir.status = 0;
        tmp_dir.data_sum = 0;
        RET_IF(ActiveBankZero(pfs));
        DirToBytes(tmp_dir, blk);
        RET_IF(pak_.write_block((uint16_t)(pfs.dir_table + j), blk));
        fixed++;
    }
    for (int j = 0; j < pfs.dir_size; j++) {
        RET_IF(pak_.read_block((uint16_t)(pfs.dir_table + j), blk));
        const Dir d = DirFromBytes(blk);
        file_next_node[j] = (d.company_code != 0 && d.game_code != 0 && d.start_page >= (uint16_t)pfs.inode_start_page) ? d.start_page : 0;
    }
    for (uint8_t bank = 0; bank < pfs.banks; bank++) {
        ret = RWInode(pfs, &tmp_inode, kRead, bank);
        if (ret != 0 && ret != kInconsistent) return ret;
        const int offset = bank > 0 ? 1 : pfs.inode_start_page;
        int j;
        for (j = 0; j < offset; j++) checked.e[j] = tmp_inode.e[j];
        for (; j < 128; j++) checked.e[j] = 3;
        for (j = 0; j < pfs.dir_size; j++) {
            while ((file_next_node[j] >> 8) == bank && file_next_node[j] >= (uint16_t)pfs.inode_start_page) {
                const uint8_t pp = (uint8_t)(file_next_node[j] & 0xFF);
                checked.e[pp] = tmp_inode.e[pp];
                file_next_node[j] = tmp_inode.e[pp];
            }
        }
        RET_IF(RWInode(pfs, &checked, kWrite, bank));
    }
    if (fixed) pfs.status |= kCorrupted;
    else pfs.status &= ~kCorrupted;
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Formatacao (osPfsReFormat)
// ---------------------------------------------------------------------------------------------
int Core::ReFormat(Pfs& pfs, int channel) {
    uint8_t tmp[32] = {};
    uint8_t buf[256];
    Inode inode;
    if (!pak_.plugged) return kNoPack;
    pfs.status ^= kInitialized;
    pfs.channel = channel;
    RET_IF(GetId(pfs));
    RET_IF(ActiveBankZero(pfs));
    for (int j = 0; j < pfs.dir_size; j++) RET_IF(pak_.write_block((uint16_t)(pfs.dir_table + j), tmp));
    for (int j = 0; j < pfs.inode_start_page; j++) inode.e[j] = 0;
    for (int j = pfs.inode_start_page; j < 128; j++) inode.e[j] = 3;
    {
        uint8_t b[256];
        InodeToBytes(inode, b);
        inode.e[0] = SumCalc(b + 2 * pfs.inode_start_page, 256 - 2 * pfs.inode_start_page);
    }
    InodeToBytes(inode, buf);
    for (int j = 0; j < kOnePage; j++) {
        RET_IF(pak_.write_block((uint16_t)(pfs.inode_table + j), buf + j * 32));
        RET_IF(pak_.write_block((uint16_t)(pfs.minode_table + j), buf + j * 32));
    }
    for (int i = 1; i < pfs.banks; i++) {
        for (int j = 1; j < 128; j++) inode.e[j] = 3;
        uint8_t b[256];
        InodeToBytes(inode, b);
        inode.e[0] = SumCalc(b + 2, 254);
        InodeToBytes(inode, buf);
        for (int j = 0; j < kOnePage; j++) {
            RET_IF(pak_.write_block((uint16_t)(pfs.inode_table + i * kOnePage + j), buf + j * 32));
            RET_IF(pak_.write_block((uint16_t)(pfs.minode_table + i * kOnePage + j), buf + j * 32));
        }
    }
    return 0;
}

void FormatPak(Pak& pak) {
    pak.mem.fill(0);
    pak.plugged = true;
    Pfs pfs;
    Core core(pak);
    core.ReFormat(pfs, 0);
    pak.dirty = true;
}

}  // namespace wdcpfs
