#include "wdc_rom.h"

#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>

namespace {
const wchar_t* kTitle = L"World Driver Championship - Leo Build";
}

bool wdc_rom_explain() {
    const int r = MessageBoxW(nullptr,
        L"Para jogar é preciso o arquivo da ROM do World Driver Championship (versão dos EUA, \"USA\"), que você mesmo precisa ter.\n\n"
        L"O programa não inclui a ROM. Clique em OK e escolha o arquivo (.z64, .v64 ou .n64). Isso só é pedido uma vez: o programa guarda uma cópia.",
        kTitle, MB_OKCANCEL | MB_ICONINFORMATION);
    return r == IDOK;
}

std::filesystem::path wdc_rom_pick_dialog() {
    wchar_t buf[MAX_PATH * 2] = {};
    OPENFILENAMEW o{};
    o.lStructSize = sizeof(o);
    o.lpstrTitle = L"Escolha a ROM do World Driver Championship (USA)";
    o.lpstrFilter = L"ROM de Nintendo 64 (*.z64;*.v64;*.n64;*.rom)\0*.z64;*.v64;*.n64;*.rom\0Todos os arquivos (*.*)\0*.*\0";
    o.lpstrFile = buf;
    o.nMaxFile = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (!GetOpenFileNameW(&o)) return {};
    return std::filesystem::path(buf);
}

void wdc_rom_show_error(int e, const std::filesystem::path& file) {
    // valores de recomp::RomValidationError: 0 Good, 1 FailedToOpen, 2 NotARom, 3 IncorrectRom, 4 NotYet, 5 IncorrectVersion, 6 OtherError
    std::wstring m;
    switch (e) {
    case 1: m = L"Não foi possível abrir o arquivo."; break;
    case 2: m = L"Este arquivo não parece ser uma ROM de Nintendo 64."; break;
    case 3: m = L"Este arquivo é uma ROM de outro jogo. Escolha a ROM do World Driver Championship."; break;
    case 5: m = L"Esta é uma versão diferente do World Driver Championship. Este programa só funciona com a versão dos EUA (\"USA\")."; break;
    default: m = L"O arquivo não foi aceito como a ROM do World Driver Championship (USA)."; break;
    }
    m += L"\n\n" + file.wstring() + L"\n\nTente de novo com outro arquivo.";
    MessageBoxW(nullptr, m.c_str(), kTitle, MB_OK | MB_ICONWARNING);
}
