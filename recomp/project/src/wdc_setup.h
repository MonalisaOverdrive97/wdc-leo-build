// Setup (launcher): janela inicial para escolher o modo (Legacy / Enhanced) e o esquema de controles (Classico / Moderno).
// Aparece ao abrir o executavel sem argumentos ou com --setup. As escolhas ficam em <config>/wdc_config.toml (mode e [controls] scheme).
#pragma once
#include <filesystem>

// Mostra o setup (bloqueia ate o usuario escolher). Devolve true se o usuario apertou JOGAR (as escolhas ja foram gravadas em wdc_config.toml),
// false se fechou/apertou Sair. `shot_path` (opcional, depuracao): grava uma captura da janela nesse PNG e fecha sozinho.
bool wdc_setup_run(const std::filesystem::path& config_dir, const char* shot_path = nullptr);
