// Per-user settings for a port: one `key = value` text file in the port's folder in the platform's
// config directory (Windows %APPDATA%\<folder>\, macOS ~/Library/Application Support/<folder>/,
// Linux $XDG_CONFIG_HOME/<folder>/ or ~/.config/<folder>/, overridable with --settings FILE).
// Never inside the install directory, never in the repository.
#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace dreamcomp {

class Settings {
public:
    // Loads `file` (missing is fine: everything falls back to defaults).
    void load(const std::filesystem::path& file);
    // Writes back atomically (temp file + rename). False if the directory cannot be written.
    bool save() const;

    std::string get(const std::string& key, const std::string& def = {}) const;
    bool get_bool(const std::string& key, bool def) const;
    int get_int(const std::string& key, int def) const;
    float get_float(const std::string& key, float def) const;
    void set(const std::string& key, const std::string& value);
    void set_bool(const std::string& key, bool v) { set(key, v ? "true" : "false"); }
    void set_int(const std::string& key, int v) { set(key, std::to_string(v)); }

    const std::filesystem::path& file() const noexcept { return file_; }
    bool dirty() const noexcept { return dirty_; }

private:
    std::filesystem::path file_;
    std::map<std::string, std::string> values_;
    bool dirty_ = false;
};

// The port's folder in the platform config directory (created on demand by save()).
std::filesystem::path default_config_dir(const std::string& folder);

}  // namespace dreamcomp
