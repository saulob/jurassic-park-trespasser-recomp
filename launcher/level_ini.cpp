#include "level_ini.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace jpt {

std::string LowerAscii(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return c < 128 ? char(std::tolower(c)) : char(c); });
    return lower;
}

bool SameText(std::string_view a, std::string_view b) { return a.size() == b.size() && LowerAscii(a) == LowerAscii(b); }

std::string TrimText(std::string_view text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}

const IniEntry* IniFile::Find(std::string_view section, std::string_view key) const {
    for (const IniEntry& entry : entries)
        if (!entry.additive && SameText(entry.section, section) && SameText(entry.key, key)) return &entry;
    return nullptr;
}

IniFile ParseIni(const std::string& text) {
    IniFile ini;
    std::string section;
    size_t start = 0;
    for (int number = 1; start <= text.size(); ++number) {
        const size_t end = std::min(text.find('\n', start), text.size());
        const std::string line = TrimText(std::string_view(text).substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            const size_t close = line.find(']');
            if (close == std::string::npos) ++ini.malformed;
            else section = TrimText(std::string_view(line).substr(1, close - 1));
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos || equals == 0) {
            ++ini.malformed;
            continue;
        }
        IniEntry entry;
        entry.section = section;
        entry.line = number;
        entry.additive = line[equals - 1] == '+';
        entry.key = TrimText(std::string_view(line).substr(0, equals - (entry.additive ? 1 : 0)));
        entry.value = TrimText(std::string_view(line).substr(equals + 1));
        if (entry.value.size() >= 2 && entry.value.front() == '"' && entry.value.back() == '"')
            entry.value = entry.value.substr(1, entry.value.size() - 2);
        if (entry.key.empty()) ++ini.malformed;
        else ini.entries.push_back(std::move(entry));
    }
    return ini;
}

bool ParseIniBool(const std::string& text, bool& value) {
    const std::string lower = LowerAscii(TrimText(text));
    if (lower == "true" || lower == "yes" || lower == "on") return value = true, true;
    if (lower == "false" || lower == "no" || lower == "off") return value = false, true;
    long number = 0;
    if (!ParseIniInt(lower, number)) return false;
    value = number != 0;
    return true;
}

bool ParseIniInt(const std::string& text, long& value) {
    const std::string trimmed = TrimText(text);
    char* end = nullptr;
    if (trimmed.empty()) return false;
    value = std::strtol(trimmed.c_str(), &end, 0);
    return *end == 0;
}

} // namespace jpt
