#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "log_scrubber.h"

namespace jpt {

inline constexpr size_t kMostGameLogs = 5;
inline constexpr size_t kFewestDescriptionCharacters = 10;
inline constexpr size_t kMostDescriptionCharacters = 500;
inline constexpr size_t kMostLogBytes = 64 * 1024;
inline constexpr size_t kMostDumpBytes = 8 * 1024 * 1024;
inline constexpr size_t kFewestDumpBytes = 32;
inline constexpr size_t kDumpChunkBytes = 64 * 1024;
inline constexpr int kDumpPairingSeconds = 120;
inline constexpr int kDumpUploadAttempts = 3;
inline constexpr char kTicketPrefix[] = "JPT-";

enum class LogActivity { kLauncher, kGameStarted, kGameplay };
enum class LogRole { kNone, kCurrentLauncher, kCurrentGameplay, kLastGameplay, kOtherGameplay, kOtherLauncher };
enum class DumpProblem { kNone, kMissing, kNotThisLog, kNotAFile, kEmpty, kTooLarge, kNotADump };

struct CrashDump {
    std::filesystem::path path;
    std::string name;
    uintmax_t bytes = 0;
};

struct GameLog {
    std::filesystem::path path;
    std::string name;
    std::time_t written = 0;
    uintmax_t bytes = 0;
    LogActivity activity = LogActivity::kLauncher;
    LogRole role = LogRole::kNone;
    std::optional<CrashDump> crash;
    DumpProblem crash_problem = DumpProblem::kMissing;

    bool Crashed() const { return crash.has_value(); }
    bool CanAttachCrash() const { return crash.has_value() && crash_problem == DumpProblem::kNone; }
};

std::vector<GameLog> FindGameLogs(const std::filesystem::path& folder, const std::filesystem::path& running_log,
                                  size_t most = kMostGameLogs);

std::string DumpNameOf(std::string_view log_name);
DumpProblem CheckCrashDump(const GameLog& log, const std::filesystem::path& dump);
bool DefaultAttachCrashReport(const GameLog& log);

size_t DefaultGameLog(const std::vector<GameLog>& logs);
const char* LogRoleText(LogRole role);
const char* PreviewButtonText(bool preview_open);
std::string LogDateText(std::time_t written, bool portuguese);
std::string LogSizeText(uintmax_t bytes);

size_t BytesOfCharacters(std::string_view text, size_t most);
size_t CountCharacters(std::string_view text);

enum class DraftProblem { kNone, kDescriptionTooShort, kNoLog };
DraftProblem CheckDraft(std::string_view description, bool has_log);

struct ReportFacts {
    std::string launcher_version;
    std::string game_build;
    std::string game_version;
    std::string operating_system;
};

std::string FormatLinuxOperatingSystem(std::string_view distribution, std::string_view dmi_vendor,
                                       std::string_view dmi_product);
std::string OperatingSystemText();

struct IncidentField {
    std::string_view key;
    std::string_view label;
    std::string value;
};

struct IncidentPackage {
    std::vector<IncidentField> fields;
    std::optional<CrashDump> crash_dump;
    bool crash_dump_requested = false;
    const std::string& Value(std::string_view key) const;
};

std::string LastBytes(std::string_view text, size_t most);

std::optional<IncidentPackage> BuildIncidentPackage(std::string_view description, const GameLog& log, const ReportFacts& facts,
                                                    const PrivateFacts& private_facts, bool attach_crash_dump = false);

std::string IncidentPreviewText(const IncidentPackage& package);
std::string IncidentRequestJson(const IncidentPackage& package, std::string_view client_id = {});

// Builds one binary dump frame: "DI", version 1, chunk type 1, followed by payload bytes.
std::vector<uint8_t> BuildDumpFrame(const uint8_t* payload, size_t size);

} // namespace jpt
