#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#include "jpt_config.h"
#include "portable.h"
#include "jpt_user_data.h"

namespace jpt {

LauncherSettings::LauncherSettings(std::filesystem::path file) : file_(std::move(file)) {
    true_value = "1";
    false_value = "0";
    std::ifstream in(file_);
    std::string line;
    char key[128], value[1024];
    while (std::getline(in, line))
        if (jpt_ini_parse_line(line.c_str(), 0, key, sizeof key, value, sizeof value)) saved_[key] = value;
}

std::string LauncherSettings::Get(std::string_view key, std::string_view def) const {
    const std::string k(key);
    if (const auto p = pending_.find(k); p != pending_.end()) return p->second;
    const char* port_default = jpt_default_value(k.c_str());
    if (const auto s = saved_.find(k); s != saved_.end() && !s->second.empty()) {
        if (!port_default || jpt_config_valid(k.c_str(), s->second.c_str())) return s->second;
    }
    if (port_default) return port_default;
    return std::string(def);
}

bool LauncherSettings::Has(std::string_view key) const {
    const std::string k(key);
    if (const auto p = pending_.find(k); p != pending_.end()) return !p->second.empty();
    const auto s = saved_.find(k);
    return s != saved_.end() && !s->second.empty();
}

void LauncherSettings::Set(std::string_view key, std::string value) {
    const std::string k(key);
    const auto s = saved_.find(k);
    if (s != saved_.end() && s->second == value) pending_.erase(k);
    else pending_[k] = std::move(value);
}

bool LauncherSettings::Write(const Values& values) {
    if (values.empty()) return true;
    std::vector<const char*> keys, texts;
    for (const auto& [k, v] : values) {
        keys.push_back(k.c_str());
        texts.push_back(v.c_str());
    }
    const std::string path = file_.string();
    jpt_make_dirs(file_.parent_path().string().c_str());
    if (!jpt_ini_set(path.c_str(), keys.data(), texts.data(), static_cast<int>(keys.size()))) return false;
    for (const auto& [k, v] : values) saved_[k] = v;
    return true;
}

bool LauncherSettings::Save() {
    if (!Write(pending_)) return false;
    pending_.clear();
    return true;
}

bool LauncherSettings::SaveNow(const Values& values) {
    if (!Write(values)) return false;
    for (const auto& [k, v] : values) pending_.erase(k);
    return true;
}

void LauncherSettings::ResetToDefaults() {
    for (int i = 0; i < JPT_SETTING_COUNT; i++) Set(jpt_settings[i].key, jpt_settings[i].default_value);
}

bool LauncherWanted(bool game_ready, bool show_setting, bool shift_held, bool skip_once) {
    if (!game_ready) return true;
    return !skip_once && (shift_held || show_setting);
}

namespace {

std::vector<std::string> ReadLines(const std::filesystem::path& file) {
    std::vector<std::string> lines;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

bool NameMatches(const std::string& line, const std::string& name) {
    return line.size() > name.size() && line[name.size()] == '=' && _strnicmp(line.c_str(), name.c_str(), name.size()) == 0;
}

}  // namespace

bool GameRegistry::GetDword(const std::string& name, unsigned long& value) const {
    for (const std::string& line : ReadLines(file_)) {
        if (!NameMatches(line, name)) continue;
        const std::string rest = line.substr(name.size() + 1);
        if (rest.rfind("dword:", 0) != 0) return false;
        value = std::strtoul(rest.c_str() + 6, nullptr, 16);
        return true;
    }
    return false;
}

bool GameRegistry::GetBinary(const std::string& name, std::vector<uint8_t>& value) const {
    for (const std::string& line : ReadLines(file_)) {
        if (!NameMatches(line, name)) continue;
        const std::string rest = line.substr(name.size() + 1);
        const size_t colon = rest.find(':');
        if (rest.rfind("hex", 0) != 0 || colon == std::string::npos || (rest.size() - colon - 1) % 2) return false;
        value.clear();
        for (size_t i = colon + 1; i + 1 < rest.size(); i += 2) {
            const std::string pair = rest.substr(i, 2);
            char* end = nullptr;
            const unsigned long byte = std::strtoul(pair.c_str(), &end, 16);
            if (end != pair.c_str() + 2) return false;
            value.push_back(static_cast<uint8_t>(byte));
        }
        return true;
    }
    return false;
}

bool GameRegistry::SetDword(const std::string& name, unsigned long value) {
    std::vector<std::string> lines = ReadLines(file_);
    char text[160];
    std::snprintf(text, sizeof text, "%s=dword:%08lX", name.c_str(), value);
    bool replaced = false;
    for (std::string& line : lines)
        if (NameMatches(line, name)) {
            line = text;
            replaced = true;
        }
    if (!replaced) lines.push_back(text);
    std::filesystem::path tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        for (const std::string& line : lines) out << line << '\n';
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file_, ec);
    return !ec;
}

}  // namespace jpt
