// Sistema de arquivos do Controller Pak (PFS) do N64, independente do runtime: opera sobre uma imagem de 32 KB em blocos de 32 bytes.
//
// A API espelha a da libultra (osPfsInitPak, osPfsAllocateFile, ...) com o MESMO formato em disco e os mesmos codigos de retorno,
// escrita a partir da leitura do codigo-fonte de referencia thirdparty/libreultra/src/io/pfs*.c e contpfs.c (versao 2.0I). A imagem e o
// "raw" de 32 KB (o mesmo que os arquivos .mpk dos emuladores), campos multibyte em big-endian:
//   bloco 0 = cabecalho (sondagem de bancos); blocos 1,3,4,6 = copias do ID do pak; bloco 7 = rotulo; blocos 8..15 = tabela de inodes
//   (1 pagina); 16..23 = copia da tabela de inodes; 24..39 = 16 entradas de diretorio (nota) de 32 bytes; paginas 5..127 = dados (123 paginas
//   de 256 bytes). Entrada de inode = u16 (banco<<8 | pagina): 1 = ultima pagina do arquivo, 3 = pagina livre; a entrada 0 leva a soma de
//   verificacao. Detalhes das estruturas (__OSPackId, __OSDir, __OSInodeUnit) sao conhecimento EXTERNO ao manual "Intro to the N64".
#pragma once
#include <array>
#include <cstdint>

namespace wdcpfs {

constexpr int kBlockSize = 32;
constexpr int kOnePage = 8;         // blocos por pagina
constexpr int kPakBytes = 32768;
constexpr int kMaxBanks = 62;

enum Err {
    kOk = 0, kNoPack = 1, kNewPack = 2, kInconsistent = 3, kContrFail = 4, kInvalid = 5, kBadData = 6, kDataFull = 7, kDirFull = 8,
    kExist = 9, kIdFatal = 10, kDevice = 11,
};
constexpr int kInitialized = 1, kCorrupted = 2;
constexpr int kRead = 0, kWrite = 1;

// Dispositivo: a RAM do pak (um banco de 32 KB; o seletor de banco e aceito e ignorado, como num pak de 32 KB).
struct Pak {
    bool plugged = false;
    std::array<uint8_t, kPakBytes> mem{};
    bool dirty = false;
    uint32_t count = 0x1000;           // fonte de "osGetCount" para o campo `random` do ID
    int read_block(uint16_t addr, uint8_t* out32);
    int write_block(uint16_t addr, const uint8_t* in32);
};

// Estado do OSPfs que a libultra mantem na memoria do jogo.
struct Pfs {
    int status = 0;
    int channel = 0;
    uint8_t id[32] = {};
    uint8_t label[32] = {};
    int version = 0;
    int dir_size = 0;
    int inode_table = 0, minode_table = 0, dir_table = 0, inode_start_page = 0;
    uint8_t banks = 0, activebank = 0;
};

struct FileState {
    uint32_t file_size = 0, game_code = 0;
    uint16_t company_code = 0;
    char ext_name[4] = {};
    char game_name[16] = {};
};

class Core {
public:
    explicit Core(Pak& pak) : pak_(pak) {}
    int InitPak(Pfs& pfs, int channel);
    int RepairId(Pfs& pfs);
    int FileState(Pfs& pfs, int file_no, wdcpfs::FileState* state);
    int FindFile(Pfs& pfs, uint16_t company, uint32_t game_code, const uint8_t* game_name, const uint8_t* ext_name, int* file_no);
    int AllocateFile(Pfs& pfs, uint16_t company, uint32_t game_code, const uint8_t* game_name, const uint8_t* ext_name, int size_bytes, int* file_no);
    int DeleteNote(Pfs& pfs, uint16_t company, uint32_t game_code, const uint8_t* game_name, const uint8_t* ext_name);
    int ReadWriteFile(Pfs& pfs, int file_no, uint8_t flag, int offset, int size_bytes, uint8_t* data);
    int FreeBlocks(Pfs& pfs, int* bytes_not_used);
    int NumFiles(Pfs& pfs, int* max_files, int* files_used);
    int Checker(Pfs& pfs);
    int ReFormat(Pfs& pfs, int channel);

private:
    struct Inode { uint16_t e[128]; };            // valores como no N64 (banco<<8 | pagina)
    struct Dir {
        uint32_t game_code; uint16_t company_code; uint16_t start_page; uint8_t status; int8_t reserved; uint16_t data_sum;
        uint8_t ext_name[4]; uint8_t game_name[16];
    };
    struct PackId {
        uint32_t repaired, random; uint64_t serial_mid, serial_low; uint16_t deviceid; uint8_t banks, version; uint16_t checksum, inverted;
    };
    struct Cache { Inode inode; uint8_t bank; uint8_t map[256]; };

    static uint16_t SumCalc(const uint8_t* p, int len);
    static void IdCheckSum(const uint8_t* id32, uint16_t* sum, uint16_t* isum);
    static void PackIdToBytes(const PackId& id, uint8_t out[32]);
    static PackId PackIdFromBytes(const uint8_t in[32]);
    static void DirToBytes(const Dir& d, uint8_t out[32]);
    static Dir DirFromBytes(const uint8_t in[32]);
    static void InodeToBytes(const Inode& i, uint8_t out[256]);
    static void InodeFromBytes(const uint8_t in[256], Inode* i);

    int SelectBank(Pfs& pfs);
    int ActiveBankZero(Pfs& pfs);
    int RepairPackId(Pfs& pfs, const uint8_t bad32[32], uint8_t new32[32]);
    int CheckPackId(Pfs& pfs, uint8_t id32[32]);   // devolve em id32 a copia valida do ID
    int GetId(Pfs& pfs);
    int CheckId(Pfs& pfs);
    int RWInode(Pfs& pfs, Inode* inode, uint8_t flag, uint8_t bank);
    int DeclearPage(Pfs& pfs, Inode* inode, int pages, int* first_page, uint8_t bank, int* decleared, int* last_page);
    int ClearPage(Pfs& pfs, int page_no, uint8_t bank);
    int ReleasePages(Pfs& pfs, Inode* inode, uint8_t start_page, uint16_t* sum, uint8_t bank, uint16_t* last_page, int flag);
    int BlockSum(Pfs& pfs, uint8_t page_no, uint16_t* sum, uint8_t bank);
    int GetNextPage(Pfs& pfs, uint8_t* bank, Inode* inode, uint16_t* page);
    int CorruptedInit(Pfs& pfs, Cache* cache);
    int Corrupted(Pfs& pfs, uint16_t fpage, Cache* cache);
    void FillPfsFromId(Pfs& pfs, const uint8_t id32[32]);

    Pak& pak_;
};

// Pak "de fabrica": zera a imagem, cria o ID (4 copias) e as tabelas de inode livres com diretorio vazio.
void FormatPak(Pak& pak);

}  // namespace wdcpfs
