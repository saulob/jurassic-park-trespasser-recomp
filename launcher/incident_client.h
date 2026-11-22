#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "http_post.h"
#include "incident_report.h"
#include "report_history.h"

namespace jpt {

inline constexpr char kBugReportUrlVariable[] = "JPT_BUG_REPORT_URL";

struct ServiceEndpoint {
    std::string url;
    std::string problem;
    bool configured() const { return !url.empty(); }
};

ServiceEndpoint ResolveEndpoint(std::string_view built_in, std::string_view override_url);

struct ServiceConfig {
    ServiceEndpoint endpoint;
    PostJson post = PostJsonOverHttps;
    PostBinary post_binary = PostBinaryOverHttps;
    HttpLimits limits;
    std::function<void(const std::string&)> log;
};

enum class IncidentOutcome { kSent, kRecentLimit, kDailyLimit, kUnreachable, kRefused, kServiceError, kNotConfigured };
enum class CrashUpload { kNotRequested, kUploaded, kFailed };
enum class FeedbackOutcome { kSaved, kStale, kUnreachable, kFailed };

IncidentOutcome IncidentOutcomeOf(int status, std::string_view error_code, HttpFailure failure = HttpFailure::kNone);
const char* IncidentLimitTitle(IncidentOutcome outcome);

struct IncidentReceipt {
    IncidentOutcome outcome = IncidentOutcome::kServiceError;
    std::string ticket;
    std::string credential;
    int64_t submitted_at = 0;
    CrashUpload crash = CrashUpload::kNotRequested;
};

struct FeedbackAnswer {
    FeedbackOutcome outcome = FeedbackOutcome::kFailed;
    std::string stage;
    std::string code;
    std::string detail;
};

FeedbackOutcome FeedbackOutcomeOf(int status, std::string_view code, HttpFailure failure = HttpFailure::kNone);

TrackedReport TrackedReportOf(const IncidentPackage& package, const IncidentReceipt& receipt);

std::string LoadOrCreateClientId(const std::filesystem::path& user_dir);

IncidentReceipt FileIncident(const ServiceConfig& config, const std::filesystem::path& user_dir, const IncidentPackage& package);
CrashUpload UploadCrashDump(const ServiceConfig& config, const std::string& ticket, const std::string& receipt, const CrashDump& dump);

std::optional<std::vector<ReportStatus>> FetchReportStatuses(const ServiceConfig& config, const std::vector<TrackedReport>& reports);
std::optional<IncidentPackage> FetchSentReport(const ServiceConfig& config, const TrackedReport& report);
FeedbackAnswer SendTestFeedback(const ServiceConfig& config, const TrackedReport& report, std::string_view result, const ReportFacts& facts);

} // namespace jpt
