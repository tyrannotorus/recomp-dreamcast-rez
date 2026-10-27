#include "dreamcomp/settings.h"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

namespace dreamcomp {

namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

std::filesystem::path default_config_dir(const std::string& folder) {
    std::filesystem::path base;
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"))
        base = appdata;
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        base = std::filesystem::path(home) / "Library" / "Application Support";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char* home = std::getenv("HOME"))
        base = std::filesystem::path(home) / ".config";
#endif
    if (base.empty())
        base = std::filesystem::current_path();
    return base / folder;
}

namespace {
// One lock for every Settings object: the in-game menu edits them on its own thread while the
// game thread reads them (a rare, short critical section either way).
std::recursive_mutex& settings_mutex() {
    static std::recursive_mutex m;
    return m;
}
}  // namespace

void Settings::load(const std::filesystem::path& file) {
    std::lock_guard<std::recursive_mutex> lock(settings_mutex());
    file_ = file;
    values_.clear();
    dirty_ = false;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        values_[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
}

bool Settings::save() const {
    std::lock_guard<std::recursive_mutex> lock(settings_mutex());
    if (file_.empty())
        return false;
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    const auto tmp = std::filesystem::path(file_.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return false;
        out << "# Rez Recompiled settings. Edited by the game's menus; safe to edit by hand while the\n"
               "# game is closed. Delete the file to return to the defaults.\n";
        for (const auto& [k, v] : values_) out << k << " = " << v << "\n";
        if (!out)
            return false;
    }
    std::filesystem::rename(tmp, file_, ec);
    return !ec;
}

std::string Settings::get(const std::string& key, const std::string& def) const {
    std::lock_guard<std::recursive_mutex> lock(settings_mutex());
    const auto it = values_.find(key);
    return it == values_.end() ? def : it->second;
}

bool Settings::get_bool(const std::string& key, bool def) const {
    const std::string v = get(key);
    if (v == "true" || v == "1" || v == "on" || v == "yes")
        return true;
    if (v == "false" || v == "0" || v == "off" || v == "no")
        return false;
    return def;
}

int Settings::get_int(const std::string& key, int def) const {
    const std::string v = get(key);
    if (v.empty())
        return def;
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 0);
    return end && *end == '\0' ? static_cast<int>(n) : def;
}

float Settings::get_float(const std::string& key, float def) const {
    const std::string v = get(key);
    if (v.empty())
        return def;
    char* end = nullptr;
    const float f = std::strtof(v.c_str(), &end);
    return end && *end == '\0' ? f : def;
}

void Settings::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lock(settings_mutex());
    auto& slot = values_[key];
    if (slot != value) {
        slot = value;
        dirty_ = true;
    }
}

}  // namespace dreamcomp
