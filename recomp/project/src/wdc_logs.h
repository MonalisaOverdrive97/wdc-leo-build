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
// Desempenho (para achar gargalos em PCs fracos): CPU do processo em segundos (usuario + kernel) e relatorio de CPU por thread no encerramento.
double wdc_proc_cpu_seconds();
void wdc_set_thread_name(const wchar_t* name);   // aparece no relatorio de CPU por thread
void wdc_logs_cpu_report();
// Prioridades (PCs fracos / carregados): processo acima do normal, para que um navegador/antivirus em segundo plano nao atrase as threads que seguram o ritmo do jogo
// (as threads em si: VI no maximo e RSP/graficos acima do normal no runtime, jogo em main.cpp, RDP em wdc_rdp_vk.cpp, comandos do ParaLLEl-RDP). WDC_NO_PRIO=1 desliga.
void wdc_perf_tune_start();
void wdc_thread_above_normal();   // a thread chamadora passa a prioridade acima do normal (salvo WDC_NO_PRIO=1)
// Abre a pasta dos logs no Explorer (cria se preciso). true = abriu.
bool wdc_logs_open_folder();
