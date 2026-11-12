// Localizacao da ROM do jogo para a instalacao do usuario final: a ROM NAO vem com o programa; o jogador aponta o arquivo uma vez (qualquer ordem de bytes:
// .z64, .v64 ou .n64) e o runtime guarda uma copia convertida em <config>/wdc.z64. Este modulo so cuida das janelas (seletor de arquivo e mensagens).
#pragma once
#include <filesystem>

// Caixa de dialogo "Abrir arquivo" para escolher a ROM. Devolve o caminho ou vazio se o usuario cancelou.
std::filesystem::path wdc_rom_pick_dialog();
// Mensagem de PT-BR para um erro de validacao da ROM (valor de recomp::RomValidationError, passado como int para nao puxar o cabecalho do runtime).
void wdc_rom_show_error(int validation_error, const std::filesystem::path& file);
// Aviso inicial antes do seletor ("falta a ROM"). Devolve false se o usuario escolheu cancelar.
bool wdc_rom_explain();
