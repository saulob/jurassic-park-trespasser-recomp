#include "incident_client.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>
#include <thread>

#include "json_lite.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace jpt {
namespace fs = std::filesystem;
namespace {

constexpr int kDumpRetryPauseMs = 250;
constexpr size_t kReceiptHexDigits = 64;
constexpr char kClientIdFile[] = "bug_report_client_id";

constexpr std::string_view kFieldLabels[][2] = {
    {"description", "Description"},
    {"launcher_version", "Launcher version"},
    {"game_build", "Game build"},
    {"game_version", "Game version"},
    {"operating_system", "Operating system"},
    {"log_file", "Selected log"},
    {"log_clipped", "Log shortened to its end"},
    {"log_text", "Log after privacy filtering"},
};

bool IsHex64(std::string_view text) {
    return text.size() == kReceiptHexDigits &&
           std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string ClaimJson(std::string_view ticket, std::string_view receipt) {
    return "{\"ticket\":" + json::Quote(ticket) + ",\"receipt\":" + json::Quote(receipt) + "}";
}

std::string StatusRequestJson(const std::vector<TrackedReport>& reports) {
    std::string out = "{\"reports\":[";
    const size_t count = std::min(reports.size(), kMostTrackedReports);
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) out += ',';
        out += ClaimJson(reports[i].ticket, reports[i].receipt);
    }
    out += "]}";
    return out;
}

std::string FeedbackRequestJson(const TrackedReport& report, std::string_view result, const ReportFacts& facts) {
    return "{\"ticket\":" + json::Quote(report.ticket) +
           ",\"receipt\":" + json::Quote(report.receipt) +
           ",\"revision\":" + std::to_string(report.status_updated_at) +
           ",\"result\":" + json::Quote(result) +
           ",\"launcher_version\":" + json::Quote(facts.launcher_version) +
           ",\"game_build\":" + json::Quote(facts.game_build) + "}";
}

bool GenerateRandomBytes(uint8_t* out, size_t count) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, out, ULONG(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    std::ifstream in("/dev/urandom", std::ios::binary);
    in.read(reinterpret_cast<char*>(out), std::streamsize(count));
    return size_t(in.gcount()) == count;
#endif
}

std::string ToHex(const uint8_t* bytes, size_t count) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

std::string Trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
    return std::string(text);
}

enum class DumpStep { kDone, kRetry, kStop };

DumpStep SendDumpOnce(const ServiceConfig& config, const std::string& ticket, const std::string& receipt, const CrashDump& dump) {
    std::error_code ec;
    if (fs::file_size(dump.path, ec) != dump.bytes || ec) return DumpStep::kStop;
    const std::string claim = ClaimJson(ticket, receipt);
    const HttpReply begun = config.post(config.endpoint.url + "/v1/incidents/dump/start", claim, config.limits);
    if (begun.failure != HttpFailure::kNone) return DumpStep::kRetry;
    if (begun.status == 400 || begun.status == 404 || begun.status == 413) return DumpStep::kStop;
    if (begun.status != 200) return DumpStep::kRetry;
    const std::optional<json::Value> start = json::Parse(begun.body);
    if (!start || !start->IsObject()) return DumpStep::kRetry;
    if (uint64_t(start->NumberOr("bytes")) != dump.bytes) return DumpStep::kStop;
    if (start->BoolOr("complete")) return DumpStep::kDone;
    uint64_t at = uint64_t(std::max<int64_t>(0, start->NumberOr("stored")));
    if (at > dump.bytes) return DumpStep::kStop;

    std::ifstream in(dump.path, std::ios::binary);
    if (!in) return DumpStep::kStop;
    in.seekg(std::streamoff(at));
    std::vector<uint8_t> buffer(kDumpChunkBytes);
    while (at < dump.bytes) {
        const size_t want = size_t(std::min<uint64_t>(kDumpChunkBytes, dump.bytes - at));
        in.read(reinterpret_cast<char*>(buffer.data()), std::streamsize(want));
        if (size_t(in.gcount()) != want) return DumpStep::kStop;
        const std::vector<uint8_t> frame = BuildDumpFrame(buffer.data(), want);
        const std::vector<HttpHeader> headers = {
            {"X-JPT-Ticket", ticket},
            {"X-JPT-Receipt", receipt},
            {"X-JPT-Dump-Offset", std::to_string(at)},
        };
        const HttpReply chunk = config.post_binary(config.endpoint.url + "/v1/incidents/dump/chunk", frame.data(), frame.size(), headers, config.limits);
        if (chunk.failure != HttpFailure::kNone) return DumpStep::kRetry;
        if (chunk.status == 400 || chunk.status == 404 || chunk.status == 413) return DumpStep::kStop;
        if (chunk.status != 200) return DumpStep::kRetry;
        at += want;
    }

    const HttpReply ended = config.post(config.endpoint.url + "/v1/incidents/dump/end", claim, config.limits);
    if (ended.failure != HttpFailure::kNone) return DumpStep::kRetry;
    if (ended.status == 200) return DumpStep::kDone;
    return (ended.status == 400 || ended.status == 404) ? DumpStep::kStop : DumpStep::kRetry;
}

} // namespace

ServiceEndpoint ResolveEndpoint(std::string_view built_in, std::string_view override_url) {
    std::string chosen = Trimmed(override_url.empty() ? built_in : override_url);
    while (!chosen.empty() && chosen.back() == '/') chosen.pop_back();
    if (chosen.empty()) return {{}, "The bug report service is not configured."};
    ServiceAddress parsed;
    if (!ParseServiceAddress(chosen, parsed) || parsed.path != "/") {
        return {{}, "The bug report service address is not valid."};
    }
    return {chosen, {}};
}

IncidentOutcome IncidentOutcomeOf(int status, std::string_view error_code, HttpFailure failure) {
    if (failure != HttpFailure::kNone) return IncidentOutcome::kUnreachable;
    if (status == 200 || status == 201) return IncidentOutcome::kSent;
    if (error_code == "incident_hourly_limit" || error_code == "limit") return IncidentOutcome::kRecentLimit;
    if (error_code == "incident_daily_limit") return IncidentOutcome::kDailyLimit;
    if (status == 429) return IncidentOutcome::kRecentLimit;
    if (status == 400 || status == 413 || error_code == "bad_request" || error_code == "too_large") return IncidentOutcome::kRefused;
    return IncidentOutcome::kServiceError;
}

const char* IncidentLimitTitle(IncidentOutcome outcome) {
    if (outcome == IncidentOutcome::kRecentLimit) return "Too many reports were sent recently";
    if (outcome == IncidentOutcome::kDailyLimit) return "Daily report limit reached";
    return nullptr;
}

FeedbackOutcome FeedbackOutcomeOf(int status, std::string_view code, HttpFailure failure) {
    if (failure != HttpFailure::kNone) return FeedbackOutcome::kUnreachable;
    if (status == 200) return FeedbackOutcome::kSaved;
    if (code == "stale_request" || code == "already_answered" || code == "not_found" || status == 404 || status == 409) {
        return FeedbackOutcome::kStale;
    }
    return FeedbackOutcome::kFailed;
}

TrackedReport TrackedReportOf(const IncidentPackage& package, const IncidentReceipt& receipt) {
    TrackedReport report;
    report.ticket = receipt.ticket;
    report.receipt = receipt.credential;
    report.submitted_at = receipt.submitted_at;
    report.status_updated_at = receipt.submitted_at;
    report.description = package.Value("description");
    report.log_file = package.Value("log_file");
    report.launcher_version = package.Value("launcher_version");
    report.game_build = package.Value("game_build");
    report.operating_system = package.Value("operating_system");
    return report;
}

std::string LoadOrCreateClientId(const fs::path& user_dir) {
    if (user_dir.empty()) return {};
    const fs::path file = user_dir / kClientIdFile;
    std::error_code ec;
    if (fs::is_regular_file(file, ec)) {
        std::ifstream in(file, std::ios::binary);
        std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        existing = Trimmed(existing);
        if (IsHex64(existing)) return existing;
    }
    uint8_t bytes[32]{};
    if (!GenerateRandomBytes(bytes, sizeof(bytes))) return {};
    const std::string created = ToHex(bytes, sizeof(bytes));
    fs::create_directories(user_dir, ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (out) {
        out << created;
        out.flush();
    }
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, ec);
    return created;
}

CrashUpload UploadCrashDump(const ServiceConfig& config, const std::string& ticket, const std::string& receipt, const CrashDump& dump) {
    if (!config.endpoint.configured() || !config.post || !config.post_binary) return CrashUpload::kFailed;
    for (int attempt = 0; attempt < kDumpUploadAttempts; ++attempt) {
        if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(kDumpRetryPauseMs * attempt));
        const DumpStep step = SendDumpOnce(config, ticket, receipt, dump);
        if (step == DumpStep::kDone) {
            if (config.log) config.log("Bug report: crash report " + dump.name + " uploaded for " + ticket);
            return CrashUpload::kUploaded;
        }
        if (step == DumpStep::kStop) break;
    }
    if (config.log) config.log("Bug report: crash report " + dump.name + " was not uploaded for " + ticket);
    return CrashUpload::kFailed;
}

IncidentReceipt FileIncident(const ServiceConfig& config, const fs::path& user_dir, const IncidentPackage& package) {
    IncidentReceipt result;
    if (!config.endpoint.configured() || !config.post) {
        result.outcome = IncidentOutcome::kNotConfigured;
        return result;
    }
    const std::string client_id = LoadOrCreateClientId(user_dir);
    const HttpReply reply = config.post(config.endpoint.url + "/v1/incidents", IncidentRequestJson(package, client_id), config.limits);
    const std::optional<json::Value> parsed = reply.failure == HttpFailure::kNone ? json::Parse(reply.body) : std::nullopt;
    const std::string error_code = parsed && parsed->IsObject() ? parsed->StringOr("error") : std::string();
    result.outcome = IncidentOutcomeOf(reply.status, error_code, reply.failure);
    if (result.outcome != IncidentOutcome::kSent) return result;
    if (!parsed || !parsed->IsObject()) {
        result.outcome = IncidentOutcome::kServiceError;
        return result;
    }
    result.ticket = parsed->StringOr("ticket");
    if (result.ticket.empty()) {
        result.outcome = IncidentOutcome::kServiceError;
        return result;
    }
    const std::string credential = parsed->StringOr("receipt");
    if (IsHex64(credential)) result.credential = credential;
    result.submitted_at = parsed->NumberOr("submitted_at");
    if (config.log) config.log("Bug report: sent as " + result.ticket);
    if (package.crash_dump) {
        result.crash = result.credential.empty() || parsed->StringOr("dump") != "expected"
                           ? CrashUpload::kFailed
                           : UploadCrashDump(config, result.ticket, result.credential, *package.crash_dump);
    } else if (package.crash_dump_requested) {
        result.crash = CrashUpload::kFailed;
    }
    return result;
}

std::optional<std::vector<ReportStatus>> FetchReportStatuses(const ServiceConfig& config, const std::vector<TrackedReport>& reports) {
    if (!config.endpoint.configured() || !config.post) return std::nullopt;
    const HttpReply reply = config.post(config.endpoint.url + "/v1/incidents/status", StatusRequestJson(reports), config.limits);
    if (reply.failure != HttpFailure::kNone || reply.status != 200) return std::nullopt;
    const std::optional<json::Value> parsed = json::Parse(reply.body);
    const json::Value* list = parsed && parsed->IsObject() ? parsed->Find("reports") : nullptr;
    if (!list || !list->IsArray()) return std::nullopt;
    std::vector<ReportStatus> statuses;
    for (const json::Value& entry : list->array) {
        if (!entry.IsObject()) continue;
        const std::string feedback = entry.StringOr("test_feedback");
        statuses.push_back({entry.StringOr("ticket"), entry.StringOr("status"), entry.NumberOr("status_updated_at"),
                            entry.StringOr("fixed_version"), entry.StringOr("player_message"),
                            (feedback == kTestFixed || feedback == kTestNotFixed) ? feedback : std::string()});
    }
    return statuses;
}

std::optional<IncidentPackage> FetchSentReport(const ServiceConfig& config, const TrackedReport& report) {
    if (!config.endpoint.configured() || !config.post) return std::nullopt;
    const HttpReply reply = config.post(config.endpoint.url + "/v1/incidents/read", ClaimJson(report.ticket, report.receipt), config.limits);
    if (reply.failure != HttpFailure::kNone || reply.status != 200) return std::nullopt;
    const std::optional<json::Value> parsed = json::Parse(reply.body);
    const json::Value* body = parsed && parsed->IsObject() ? parsed->Find("report") : nullptr;
    if (!body || !body->IsObject()) return std::nullopt;
    IncidentPackage package;
    for (const auto& pair : kFieldLabels) package.fields.push_back({pair[0], pair[1], body->StringOr(pair[0])});
    return package;
}

FeedbackAnswer SendTestFeedback(const ServiceConfig& config, const TrackedReport& report, std::string_view result, const ReportFacts& facts) {
    FeedbackAnswer answer;
    if (!config.endpoint.configured() || !config.post) {
        answer.stage = "connect";
        answer.code = "not_configured";
        answer.outcome = FeedbackOutcome::kUnreachable;
        return answer;
    }
    const HttpReply reply = config.post(config.endpoint.url + "/v1/incidents/feedback", FeedbackRequestJson(report, result, facts), config.limits);
    if (reply.failure != HttpFailure::kNone) {
        answer.stage = "connect";
        answer.code = "unreachable";
        answer.outcome = FeedbackOutcome::kUnreachable;
        return answer;
    }
    const std::optional<json::Value> parsed = json::Parse(reply.body);
    answer.stage = "call";
    answer.code = parsed && parsed->IsObject() ? parsed->StringOr("error") : std::string();
    answer.detail = parsed && parsed->IsObject() ? parsed->StringOr("detail").substr(0, 80) : std::string();
    answer.outcome = FeedbackOutcomeOf(reply.status, answer.code, reply.failure);
    if (answer.outcome == FeedbackOutcome::kSaved) {
        if (!parsed || !parsed->IsObject() || parsed->StringOr("test_feedback") != result) {
            answer.outcome = FeedbackOutcome::kFailed;
            answer.code = "malformed_reply";
            answer.detail = "test_feedback";
        }
    }
    return answer;
}

} // namespace jpt
