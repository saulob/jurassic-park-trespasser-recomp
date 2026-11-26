// The Custom Levels list: each package's listing record, which spares analysing it again while nothing it was read from
// changed, and the job that makes the list off the UI thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "background_job.h"
#include "custom_levels.h"

namespace jpt {

// Raise whenever the analysis or the READY rule changes: every record made before is then read as stale.
inline constexpr int kListingVersion = 6;

// What the analysis reads of the installed game: its version folder, every file by name, size and time.
std::string GameListingKey(const std::filesystem::path& data_dir);
// The package's own files (its listing and world caches aside), the game's key and what the analysis is.
std::string PackageListingKey(const std::filesystem::path& package_dir, const std::string& game_key);

bool ReadListingRecord(const std::filesystem::path& package_dir, const std::string& key, LevelPackage& package);
bool WriteListingRecord(const std::filesystem::path& package_dir, const std::string& key, const LevelPackage& package);
std::string ListingRecordText(const std::string& key, const LevelPackage& package);
bool ParseListingRecord(const std::string& text, const std::string& key, LevelPackage& package);

struct ListingRequest {
    std::filesystem::path user_dir, data_dir;
    bool operator==(const ListingRequest& other) const { return user_dir == other.user_dir && data_dir == other.data_dir; }
};

struct Listing {
    std::vector<LevelPackage> packages;
    ListingStats stats;
    double total_ms = 0;
};

Listing ScanLevels(const ListingRequest& request, const std::atomic<bool>& cancelled);

// When the page shows the list, the loading view or a failure, and when its job starts. Used by the UI thread alone.
class LevelListing {
public:
    using Work = std::function<Listing(const ListingRequest&, const std::atomic<bool>&)>;
    enum class View { kLoading, kList, kFailed };
    struct Frame {
        View view = View::kLoading;
        bool adopted = false; // a new list this frame
        bool failed = false;  // the job failed this frame
    };

    explicit LevelListing(Work work = ScanLevels) : work_(std::move(work)) {}

    // Another list is needed; blank: the current one is no longer shown meanwhile.
    void Invalidate(bool blank);
    // The page was not drawn for a while: its next loading view is a first one again.
    void Hidden() { loading_shown_ = false; }
    // A package removed meanwhile leaves the list shown at once.
    void Forget(const std::string& id);
    // Once per frame the page is drawn. Work starts only once a frame has shown the loading view; a list still shown
    // stays until the new one is ready.
    Frame Advance(const ListingRequest& request);

    const Listing& list() const { return list_; }
    const std::string& error() const { return error_; }
    bool busy() const { return job_.busy(); }
    uint64_t jobs_started() const { return started_; }

private:
    Work work_;
    BackgroundJob<Listing> job_;
    ListingRequest request_;
    bool has_request_ = false;
    Listing list_;
    std::string error_;
    uint64_t generation_ = 1, listed_ = 0, failed_ = 0, job_generation_ = 0, job_serial_ = 0, started_ = 0;
    bool blank_ = true, loading_shown_ = false;
};

} // namespace jpt
