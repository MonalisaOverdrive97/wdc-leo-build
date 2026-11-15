// Arquivo de log para diagnostico em outros PCs (build de distribuicao): a saida do programa vai para
// %LOCALAPPDATA%\WorldDriverChampionship\logs\wdc_log_AAAAMMDD-HHMMSS.txt (os 10 mais recentes), com um cabecalho do sistema
// (Windows, CPU, memoria, placas de video, monitores/DPI, audio, pastas). Para mandar um log: abrir a pasta pelo botao do setup.
#pragma once
#include <filesystem>

// Pasta dos logs (criada se preciso); vazia se nao foi possivel criar/gravar em %LOCALAPPDATA%.
std::filesystem::path wdc_logs_dir();
// Redireciona stdout/stderr para um log novo (sem buffer) e apaga os mais antigos alem de 10. `fallback` = pasta alternativa se %LOCALAPPDATA% nao serve.
// Devolve o caminho do arquivo (vazio se falhou; a saida segue onde estava).
std::filesystem::path wdc_logs_start(const std::filesystem::path& fallback);
// Imprime o cabecalho do sistema no log (versao, Windows, CPU, memoria, GPUs, monitores, audio, pastas).
void wdc_logs_system_info(const std::filesystem::path& exe_dir, const std::filesystem::path& config_dir);
// Abre a pasta dos logs no Explorer (cria se preciso). true = abriu.
bool wdc_logs_open_folder();
