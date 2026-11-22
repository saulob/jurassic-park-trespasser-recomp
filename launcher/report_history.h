#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace jpt {

inline constexpr size_t kMostTrackedReports = 50;
inline constexpr char kReportHistoryFile[] = "bug_reports.json";

inline constexpr char kAwaitingPlayerTest[] = "awaiting_player_test";
inline constexpr char kTestFixed[] = "fixed";
inline constexpr char kTestNotFixed[] = "not_fixed";

struct ReportStatus {
    std::string ticket;
    std::string status;
    int64_t updated_at = 0;
    std::string fixed_version;
    std::string player_message;
    std::string test_feedback;
};

struct TrackedReport {
    std::string ticket;
    std::string receipt;
    int64_t submitted_at = 0;
    std::string description;
    std::string log_file;
    std::string launcher_version;
    std::string game_build;
    std::string operating_system;
    std::string status = "new";
    int64_t status_updated_at = 0;
    std::string fixed_version;
    std::string player_message;
    std::string test_feedback;
    int64_t test_feedback_revision = 0;
    bool unseen = false;
    bool message_unseen = false;
    bool unavailable = false;
};

enum class PlayerTest { kNone, kAwaitingAnswer, kConfirmedFixed, kStillHappening };
PlayerTest PlayerTestOf(const TrackedReport& report);

const char* ReportStatusText(std::string_view status);
std::string ReportStatusLine(const TrackedReport& report);
std::string ReportPreview(std::string_view description);

class ReportHistory {
public:
    explicit ReportHistory(std::filesystem::path file) : file_(std::move(file)) {}

    void Load();
    bool Save() const;

    const std::vector<TrackedReport>& reports() const { return reports_; }
    bool HasUnseen() const;
    bool HasUnseenMessage() const;
    std::string NextUnseen() const;

    bool Add(TrackedReport report);
    bool Apply(const std::vector<ReportStatus>& statuses, const std::vector<std::string>& asked = {});
    bool MarkSeen(std::string_view ticket);
    bool RecordTestFeedback(std::string_view ticket, int64_t revision, std::string_view result);
    bool Remove(std::string_view ticket);

private:
    std::filesystem::path file_;
    std::vector<TrackedReport> reports_;
};

} // namespace jpt
