#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "incident_client.h"
#include "incident_report.h"
#include "my_reports_page.h"
#include "trg/launcher.h"

namespace jpt {

class IncidentPage {
public:
    struct Places {
        std::filesystem::path logs_dir;
        std::filesystem::path running_log;
        std::filesystem::path user_dir;
        std::vector<std::filesystem::path> install_folders;
        std::function<void(const std::string&)> log;
    };

    explicit IncidentPage(Places places);

    void Draw(trg::Ui& ui, const ReportFacts& facts);
    void CheckReports() { my_reports_.Refresh(); }
    bool HasReportUpdates() { return my_reports_.HasUpdates(); }
    bool Loading() const { return !logs_found_; }

private:
    struct Delivery {
        std::atomic<bool> done{false};
        IncidentReceipt receipt;
        std::shared_ptr<const IncidentPackage> package;
    };

    struct LogScan {
        std::atomic<bool> done{false};
        std::vector<GameLog> logs;
    };

    struct DumpRetry {
        std::atomic<bool> done{false};
        CrashUpload outcome = CrashUpload::kFailed;
    };

    void FindLogs();
    void StartLogScan();
    void CollectLogScan();
    void CollectDelivery();
    void CollectDumpRetry();
    void RetryDump();
    void SelectLog(size_t index);
    void ForgetPreview();
    bool AcceptDraft();
    std::shared_ptr<const IncidentPackage> BuildPackage(const ReportFacts& facts);
    void TogglePreview(const ReportFacts& facts);
    void Send(const ReportFacts& facts);
    void DrawDescription(trg::Ui& ui, bool sending);
    void DrawLogs(trg::Ui& ui, bool sending);
    void DrawCrashReport(trg::Ui& ui, bool sending);
    void DrawPreview(trg::Ui& ui, const ReportFacts& facts, bool sending);
    void DrawSend(trg::Ui& ui, const ReportFacts& facts, bool sending);
    void DrawResult(trg::Ui& ui);

    Places places_;
    ServiceConfig service_;
    char description_[kMostDescriptionCharacters * 4 + 1] = {};
    std::vector<GameLog> logs_;
    bool logs_found_ = false;
    size_t selected_ = 0;
    DraftProblem problem_ = DraftProblem::kNone;
    double problem_time_ = 0.0;
    bool bring_problem_into_view_ = false;
    std::shared_ptr<const IncidentPackage> previewed_;
    std::string preview_text_;
    bool log_failed_ = false;
    std::shared_ptr<Delivery> delivery_;
    std::optional<IncidentReceipt> result_;
    std::shared_ptr<DumpRetry> dump_retry_;
    std::optional<CrashDump> sent_dump_;
    bool attach_crash_report_ = false;
    bool bring_result_into_view_ = false;
    bool sent_report_remembered_ = true;
    MyReports my_reports_;
    int last_drawn_frame_ = -2;
    std::shared_ptr<LogScan> scan_;
    float form_top_ = 0.0f;
};

} // namespace jpt
