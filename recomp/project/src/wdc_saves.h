// Saves (Controller Pak virtual) numa pasta FIXA, independente da build e da pasta do executavel, para sobreviverem a atualizacoes e a pastas novas.
// Regra do projeto: NUNCA apagar nem sobrescrever um save do jogador; migrar so copiando e guardar backups com data.
#pragma once
#include <filesystem>

// %LOCALAPPDATA%/WorldDriverChampionship/saves (criada se preciso). Vazio se nao foi possivel criar/gravar.
std::filesystem::path wdc_saves_dir();
// Copia (nunca move nem apaga) o save antigo `old_file` para `new_file`, so se `new_file` ainda nao existe. true = copiou.
bool wdc_saves_migrate(const std::filesystem::path& old_file, const std::filesystem::path& new_file);
// Guarda uma copia datada de `file` em <pasta do arquivo>/backups (nao repete se for igual a copia mais recente) e mantem as `keep` mais recentes por save.
void wdc_saves_backup(const std::filesystem::path& file, int keep = 10);
