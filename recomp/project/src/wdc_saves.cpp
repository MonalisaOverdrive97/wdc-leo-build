#include "wdc_saves.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
bool writable(const fs::path& d) {
    std::error_code ec;
    fs::create_directories(d, ec);
    const fs::path t = d / ".write_test";
    std::ofstream f(t);
    const bool ok = (bool)f;
    f.close();
    fs::remove(t, ec);
    return ok;
}
std::vector<char> read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
}  // namespace

fs::path wdc_saves_dir() {
    const char* la = std::getenv("LOCALAPPDATA");
    if (!la || !*la) return {};
    const fs::path d = fs::path(la) / "WorldDriverChampionship" / "saves";
    return writable(d) ? d : fs::path{};
}

bool wdc_saves_migrate(const fs::path& old_file, const fs::path& new_file) {
    std::error_code ec;
    if (!fs::exists(old_file, ec) || fs::exists(new_file, ec)) return false;
    fs::create_directories(new_file.parent_path(), ec);
    const bool ok = fs::copy_file(old_file, new_file, fs::copy_options::none, ec) && !ec;
    std::printf("[saves] %s: %s -> %s\n", ok ? "save antigo copiado" : "FALHOU ao copiar o save antigo", old_file.string().c_str(), new_file.string().c_str());
    return ok;
}

void wdc_saves_backup(const fs::path& file, int keep) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return;
    const fs::path dir = file.parent_path() / "backups";
    fs::create_directories(dir, ec);
    const std::string stem = file.stem().string();
    std::vector<fs::path> mine;   // backups deste save, do mais antigo ao mais novo (o nome tem a data)
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind(stem + "-", 0) == 0 && e.path().extension() == ".mpk") mine.push_back(e.path());
    }
    std::sort(mine.begin(), mine.end());
    const std::vector<char> cur = read_all(file);
    if (!mine.empty() && read_all(mine.back()) == cur) return;   // igual ao ultimo backup: nao repete
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tmv);
    const fs::path dst = dir / (stem + "-" + ts + ".mpk");
    if (fs::copy_file(file, dst, fs::copy_options::skip_existing, ec) && !ec) {
        mine.push_back(dst);
        std::sort(mine.begin(), mine.end());
        std::printf("[saves] backup: %s\n", dst.string().c_str());
    }
    while ((int)mine.size() > keep) {   // so apaga BACKUPS antigos, nunca o save
        fs::remove(mine.front(), ec);
        mine.erase(mine.begin());
    }
}
