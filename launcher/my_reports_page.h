#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "incident_client.h"
#include "incident_report.h"
#include "report_history.h"
#include "trg/launcher.h"

namespace jpt {

class MyReports {
public:
    MyReports(std::filesystem::path history_file, ServiceConfig service);

    void Draw(trg::Ui& ui, const ReportFacts& facts);
    void Refresh(bool asked_by_player = false);
    bool Remember(const IncidentPackage& package, const IncidentReceipt& receipt);
    bool HasUpdates();
    bool HasNewReply();
    void ShowUpdate();
    void BringIntoView() { bring_into_view_ = true; }
    bool TakeNewReportRequest() { return std::exchange(new_report_requested_, false); }

private:
    struct StatusCheck {
        std::atomic<bool> done{false};
        std::optional<std::vector<ReportStatus>> statuses;
        std::vector<std::string> asked;
    };
    struct FeedbackSend {
        std::atomic<bool> done{false};
        std::string ticket;
        int64_t revision = 0;
        std::string result;
        FeedbackAnswer answer;
    };
    struct SentReportRead {
        std::atomic<bool> done{false};
        std::string ticket;
        std::optional<IncidentPackage> package;
    };

    void Collect();
    void SaveHistory();
    void Open(const std::string& ticket);
    void Collapse();
    void ForgetSentReport();
    void CollectFeedback();
    void SendFeedback(const TrackedReport& report, std::string_view result);
    void ReadSentReport(const TrackedReport& report);
    void DrawHeader(trg::Ui& ui);
    void DrawReports(trg::Ui& ui);
    void DrawRow(trg::Ui& ui, const TrackedReport& report, size_t index, std::string& toggled);
    void DrawExpanded(trg::Ui& ui, const TrackedReport& report, size_t index, std::string& toggled, std::string& removed);
    void DrawPlayerTest(trg::Ui& ui, const TrackedReport& report);
    void DrawActions(trg::Ui& ui, const TrackedReport& report, std::string& removed);
    void DrawSentReport(trg::Ui& ui);
    bool ConfirmRemoval(trg::Ui& ui);

    ReportHistory history_;
    ServiceConfig service_;
    std::shared_ptr<StatusCheck> check_;
    bool asked_by_player_ = false;
    bool check_failed_ = false;
    bool bring_into_view_ = false;
    bool bring_detail_into_view_ = false;
    std::string open_ticket_;
    std::string fresh_reply_;
    ReportFacts facts_;
    std::shared_ptr<FeedbackSend> feedback_;
    std::string feedback_error_ticket_;
    FeedbackOutcome feedback_error_ = FeedbackOutcome::kFailed;
    bool refresh_wanted_ = false;
    bool new_report_requested_ = false;
    std::shared_ptr<SentReportRead> reading_;
    std::string sent_ticket_;
    std::string sent_text_;
    bool sent_unavailable_ = false;
};

} // namespace jpt
