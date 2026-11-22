#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "incident_client.h"
#include "incident_report.h"
#include "json_lite.h"
#include "report_history.h"

namespace fs = std::filesystem;
using namespace jpt;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

void WriteBytes(const fs::path& file, const std::string& content) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(content.data(), std::streamsize(content.size()));
}

std::string MakeMinidump(size_t size) {
    std::string out(size, '\x42');
    if (size >= 4) {
        out[0] = 'M';
        out[1] = 'D';
        out[2] = 'M';
        out[3] = 'P';
    }
    return out;
}

fs::path MakeTempDir(const char* tag) {
    const fs::path dir = fs::temp_directory_path() / (std::string("jpt_bug_report_test_") + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void TestLogDiscoveryAndCrashValidation() {
    const fs::path dir = MakeTempDir("logs");
    const fs::path running = dir / "trespasser_recomp_005.log";
    const fs::path gameplay = dir / "trespasser_recomp_004.log";
    const fs::path launcher_closed = dir / "trespasser_recomp_003.log";
    const fs::path older_gameplay = dir / "trespasser_recomp_002.log";

    WriteBytes(older_gameplay, "[guest   ] entering the CRT entry point\n");
    WriteBytes(launcher_closed, "[launcher] closed\n");
    WriteBytes(gameplay, "[launcher] closed, starting the game\n[guest   ] entering the CRT entry point\n");
    WriteBytes(running, "[launcher] shown\n");

    const fs::path dmp = dir / "trespasser_recomp_004.dmp";
    WriteBytes(dmp, MakeMinidump(128));

    const auto logs = FindGameLogs(dir, running);
    Check(logs.size() == 3, "closed launcher-only session omitted");
    Check(logs[0].name == "trespasser_recomp_005.log" && logs[0].role == LogRole::kCurrentLauncher, "running log first");
    Check(logs[1].name == "trespasser_recomp_004.log" && logs[1].role == LogRole::kLastGameplay, "last gameplay marked");
    Check(logs[1].Crashed() && logs[1].CanAttachCrash() && DefaultAttachCrashReport(logs[1]), "crash detected and attachable");
    Check(DefaultGameLog(logs) == 1, "default log picks last gameplay");
    Check(logs[2].role == LogRole::kOtherGameplay, "older gameplay role");

    // Local DMP validation edge cases.
    Check(CheckCrashDump(logs[1], dmp) == DumpProblem::kNone, "valid MDMP passes");
    Check(CheckCrashDump(logs[1], dir / "other.dmp") == DumpProblem::kNotThisLog, "wrong filename rejected");
    Check(CheckCrashDump(logs[1], dir / "sub" / "trespasser_recomp_004.dmp") == DumpProblem::kNotThisLog, "wrong directory rejected");

    const fs::path empty_dmp = dir / "trespasser_recomp_002.dmp";
    WriteBytes(empty_dmp, "");
    Check(CheckCrashDump(logs[2], empty_dmp) == DumpProblem::kEmpty, "empty dump rejected");

    WriteBytes(empty_dmp, std::string(64, 'X'));
    Check(CheckCrashDump(logs[2], empty_dmp) == DumpProblem::kNotADump, "non-MDMP rejected");

    WriteBytes(empty_dmp, MakeMinidump(16));
    Check(CheckCrashDump(logs[2], empty_dmp) == DumpProblem::kNotADump, "too-small dump rejected");

    // Too old timestamp rejected.
    WriteBytes(empty_dmp, MakeMinidump(64));
    std::error_code ec;
    fs::last_write_time(empty_dmp, fs::last_write_time(older_gameplay, ec) - std::chrono::seconds(kDumpPairingSeconds + 60), ec);
    Check(CheckCrashDump(logs[2], empty_dmp) == DumpProblem::kNotThisLog, "stale timestamp rejected");

    // Preview shows binary dump name/size/notice without raw bytes.
    const ReportFacts facts{"0.1.0", "English (Retail 116)", "116", "Windows 11"};
    const auto pkg = BuildIncidentPackage("Game crashed at the gate.", logs[1], facts, PrivateFacts{}, true);
    Check(pkg.has_value() && pkg->crash_dump.has_value(), "package attaches dump");
    const std::string preview = IncidentPreviewText(*pkg);
    Check(preview.find("trespasser_recomp_004.dmp") != std::string::npos, "preview names dump");
    Check(preview.find("MDMP") == std::string::npos, "preview does not dump raw bytes");

    const auto frame = BuildDumpFrame(reinterpret_cast<const uint8_t*>("MDMP"), 4);
    Check(frame.size() == 8 && frame[0] == 0x44 && frame[1] == 0x49 && frame[2] == 1 && frame[3] == 1, "DI v1 chunk 1 frame header");

    fs::remove_all(dir, ec);
}

void TestHistoryStatusAndFeedback() {
    const fs::path dir = MakeTempDir("history");
    const fs::path file = dir / kReportHistoryFile;
    ReportHistory history(file);

    TrackedReport r;
    r.ticket = "JPT-234567";
    r.receipt = std::string(64, 'a');
    r.submitted_at = 1000;
    r.status_updated_at = 1000;
    r.description = "Crash near lab";
    Check(history.Add(r), "history add and save");

    // Status update with developer reply marks unseen + message_unseen.
    Check(history.Apply({{"JPT-234567", "awaiting_player_test", 2000, "", "Please retest in 0.1.1", ""}}, {"JPT-234567"}), "apply awaiting_player_test");
    Check(history.HasUnseen() && history.HasUnseenMessage(), "unseen and new reply flags");
    Check(history.NextUnseen() == "JPT-234567", "next unseen picks reply");
    Check(PlayerTestOf(history.reports()[0]) == PlayerTest::kAwaitingAnswer, "awaiting answer");

    // Wrong revision rejected, matching revision recorded.
    Check(!history.RecordTestFeedback("JPT-234567", 1999, kTestFixed), "wrong revision rejected");
    Check(history.RecordTestFeedback("JPT-234567", 2000, kTestFixed), "matching revision recorded");
    Check(PlayerTestOf(history.reports()[0]) == PlayerTest::kConfirmedFixed, "confirmed fixed");

    // MarkSeen clears unseen flags; reload persists.
    Check(history.MarkSeen("JPT-234567") && !history.HasUnseen() && !history.HasUnseenMessage(), "mark seen clears");
    Check(history.Save(), "save after feedback");

    ReportHistory reloaded(file);
    reloaded.Load();
    Check(reloaded.reports().size() == 1 && PlayerTestOf(reloaded.reports()[0]) == PlayerTest::kConfirmedFixed, "reload keeps feedback");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

void TestClientChunkedUploadAndRetry() {
    const fs::path dir = MakeTempDir("client");
    const fs::path log_file = dir / "trespasser_recomp_010.log";
    const fs::path dmp_file = dir / "trespasser_recomp_010.dmp";
    WriteBytes(log_file, "[guest   ] entering the CRT entry point\n");
    WriteBytes(dmp_file, MakeMinidump(100));

    const auto logs = FindGameLogs(dir, {});
    const ReportFacts facts{"0.1.0", "English (Retail 116)", "116", "Windows 11"};
    const auto pkg = BuildIncidentPackage("Test crash upload retry", logs[0], facts, PrivateFacts{}, true);
    Check(pkg.has_value() && pkg->crash_dump.has_value(), "client test package built");

    int chunk_calls = 0;
    bool end_called = false;
    ServiceConfig cfg;
    cfg.endpoint = ResolveEndpoint("https://reports.example", "");
    cfg.post = [&](const std::string& url, const std::string&, const HttpLimits&) -> HttpReply {
        if (url.ends_with("/v1/incidents")) {
            return {HttpFailure::kNone, 201,
                    "{\"ticket\":\"JPT-ABCDEF\",\"receipt\":\"" + std::string(64, 'b') +
                        "\",\"submitted_at\":1700000000000,\"dump\":\"expected\",\"dump_chunk_bytes\":65536}"};
        }
        if (url.ends_with("/v1/incidents/dump/start")) {
            return {HttpFailure::kNone, 200, "{\"stored\":0,\"bytes\":100,\"complete\":false}"};
        }
        if (url.ends_with("/v1/incidents/dump/end")) {
            end_called = true;
            return {HttpFailure::kNone, 200, "{\"stored\":100,\"bytes\":100,\"complete\":true}"};
        }
        if (url.ends_with("/v1/incidents/feedback")) {
            return {HttpFailure::kNone, 200, "{\"ticket\":\"JPT-ABCDEF\",\"revision\":2000,\"test_feedback\":\"fixed\"}"};
        }
        return {HttpFailure::kNone, 404, "{}"};
    };
    cfg.post_binary = [&](const std::string&, const uint8_t* data, size_t size, const std::vector<HttpHeader>&, const HttpLimits&) -> HttpReply {
        ++chunk_calls;
        if (chunk_calls == 1) return {HttpFailure::kUnreachable, 0, ""}; // Force retry on first attempt
        Check(size == 104 && data[0] == 0x44 && data[1] == 0x49 && data[2] == 1 && data[3] == 1, "chunk frame valid on retry");
        return {HttpFailure::kNone, 200, "{\"stored\":100,\"bytes\":100}"};
    };

    const IncidentReceipt receipt = FileIncident(cfg, dir, *pkg);
    Check(receipt.outcome == IncidentOutcome::kSent, "report sent");
    Check(receipt.ticket == "JPT-ABCDEF", "ticket parsed");
    Check(receipt.crash == CrashUpload::kUploaded && chunk_calls == 2 && end_called, "dump retried and uploaded");

    TrackedReport tracked = TrackedReportOf(*pkg, receipt);
    tracked.status = kAwaitingPlayerTest;
    tracked.status_updated_at = 2000;
    const FeedbackAnswer fb = SendTestFeedback(cfg, tracked, kTestFixed, facts);
    Check(fb.outcome == FeedbackOutcome::kSaved, "feedback sent and confirmed");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

} // namespace

int main() {
    TestLogDiscoveryAndCrashValidation();
    TestHistoryStatusAndFeedback();
    TestClientChunkedUploadAndRetry();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("bug_report_tests: all passed\n");
    return 0;
}
