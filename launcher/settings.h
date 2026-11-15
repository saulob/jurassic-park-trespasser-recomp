// The launcher's view of user_data\launcher\trespasser.ini (src/common/jpt_config.h): edits stay pending until Save or Play.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "trg/settings.h"

namespace jpt {

using Values = std::map<std::string, std::string, std::less<>>;

inline constexpr char kLauncherXKey[] = "launcher_x";
inline constexpr char kLauncherYKey[] = "launcher_y";
inline constexpr char kLauncherWidthKey[] = "launcher_width";
inline constexpr char kLauncherHeightKey[] = "launcher_height";
inline constexpr char kLauncherMaximizedKey[] = "launcher_maximized";
inline constexpr char kLauncherFullscreenKey[] = "launcher_fullscreen";

// The launcher opens when the game files are not ready, or when the setting is On or Shift is held (--launcher acts
// like Shift), unless it was skipped once. A game that is not ready always shows it: it is where the files are installed.
bool LauncherWanted(bool game_ready, bool show_setting, bool shift_held, bool skip_once);

class LauncherSettings final : public trg::SettingsStore {
public:
    explicit LauncherSettings(std::filesystem::path file);

    // A missing or invalid value reads as the port's default for the key (the game reads it the same way).
    std::string Get(std::string_view key, std::string_view def = {}) const override;
    void Set(std::string_view key, std::string value) override;
    bool Save() override;
    void ResetToDefaults() override;
    bool SaveNow(const Values& values);
    bool Dirty() const { return !pending_.empty(); }

    const std::filesystem::path& file() const { return file_; }

private:
    bool Write(const Values& values);

    std::filesystem::path file_;
    Values saved_;
    Values pending_;
};

// The game's own settings in its registry store (user_data\saves\recomp-registry.txt): REG_DWORD values only.
class GameRegistry {
public:
    explicit GameRegistry(std::filesystem::path file) : file_(std::move(file)) {}
    bool GetDword(const std::string& name, unsigned long& value) const;
    // A hex(N) value's bytes, as the game's KeyMap is stored.
    bool GetBinary(const std::string& name, std::vector<uint8_t>& value) const;
    bool SetDword(const std::string& name, unsigned long value);

private:
    std::filesystem::path file_;
};

}  // namespace jpt
