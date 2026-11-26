// The INI files of fan missions (fm.ini, tp_mod.ini, trespasser.ini, tpass.ini): every entry in file order, with "+=" kept
// apart from "=".
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace jpt {

struct IniEntry {
    std::string section;
    std::string key;
    std::string value;
    bool additive = false; // key+=value
    int line = 0;
};

struct IniFile {
    std::vector<IniEntry> entries;
    int malformed = 0;

    // The first "key=" of the section, as Windows reads profile files; null when it is absent.
    const IniEntry* Find(std::string_view section, std::string_view key) const;
};

IniFile ParseIni(const std::string& text);

std::string LowerAscii(std::string_view text);
bool SameText(std::string_view a, std::string_view b);
std::string TrimText(std::string_view text);
bool ParseIniBool(const std::string& text, bool& value);
bool ParseIniInt(const std::string& text, long& value);

} // namespace jpt
