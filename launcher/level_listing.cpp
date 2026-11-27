#include "level_listing.h"

#include "level_guide.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

#include "jpt_custom_level.h"
#include "jpt_groff.h"
#include "jpt_hash.h"
#include "jpt_level_compat.h"
#include "jpt_level_features.h"
#include "jpt_scene.h"
#include "jpt_script_plan.h"
#include "level_ini.h"
#include "level_manifest.h"
#include "utf8.h"

namespace fs = std::filesystem;

namespace jpt {
namespace {

constexpr char kRecordFile[] = "listing.txt";
constexpr char kRecordHead[] = "custom-level-listing";
constexpr uint64_t kRecordLimit = 16ull << 20;

std::string Hex(const std::string& text) {
    char hex[65];
    jpt_sha256_buffer_hex(text.data(), text.size(), hex);
    return hex;
}

// Every file below root by path, size and time; links are neither followed nor counted.
std::vector<std::string> FileLines(const fs::path& root, const std::function<bool(const std::string&)>& skip) {
    std::vector<std::string> lines;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        const fs::file_status status = it->symlink_status(type_ec);
        if (type_ec || fs::is_symlink(status)) {
            if (fs::is_directory(it->status(type_ec))) it.disable_recursion_pending();
            continue;
        }
        if (!fs::is_regular_file(status)) continue;
        const std::string relative = Utf8(it->path().lexically_relative(root).generic_string<char8_t>());
        if (skip && skip(LowerAscii(relative))) continue;
        std::error_code size_ec, time_ec;
        const uintmax_t size = it->file_size(size_ec);
        const auto time = it->last_write_time(time_ec);
        lines.push_back(relative + "|" + std::to_string(size_ec ? 0 : size) + "|" + std::to_string(time_ec ? 0 : time.time_since_epoch().count()));
    }
    if (ec) lines.push_back("unlisted|" + std::to_string(ec.value()));
    std::sort(lines.begin(), lines.end());
    return lines;
}

std::string Escape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '\\') out += "\\\\";
        else if (c == '|') out += "\\p";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

std::string Unescape(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 == text.size()) {
            out += text[i];
            continue;
        }
        const char c = text[++i];
        out += c == 'p' ? '|' : c == 'n' ? '\n' : c == 'r' ? '\r' : c;
    }
    return out;
}

std::vector<std::string> Fields(const std::string& text) {
    std::vector<std::string> fields;
    std::string field;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            field += text[i];
            field += text[++i];
        } else if (text[i] == '|') {
            fields.push_back(Unescape(field));
            field.clear();
        } else {
            field += text[i];
        }
    }
    fields.push_back(Unescape(field));
    return fields;
}

std::string Line(const char* kind, std::initializer_list<std::string> fields) {
    std::string line = kind;
    char separator = ' ';
    for (const std::string& field : fields) {
        line += separator + Escape(field);
        separator = '|';
    }
    return line + "\n";
}

bool Number(const std::string& text, long long low, long long high, long long& value) {
    char* end = nullptr;
    value = std::strtoll(text.c_str(), &end, 10);
    return !text.empty() && end && !*end && value >= low && value <= high;
}

bool Flag(const std::string& text, bool& value) {
    if (text != "0" && text != "1") return false;
    value = text == "1";
    return true;
}

template <typename Enum>
bool EnumOf(const std::string& text, Enum last, Enum& value) {
    long long number = 0;
    if (!Number(text, 0, static_cast<long long>(last), number)) return false;
    value = static_cast<Enum>(number);
    return true;
}

std::string Str(long long value) { return std::to_string(value); }

} // namespace

std::string GameListingKey(const fs::path& data_dir) {
    if (data_dir.empty()) return "no game";
    const fs::path version = data_dir.parent_path();
    std::string text = Utf8(data_dir.lexically_normal()) + "\n";
    for (const std::string& line : FileLines(version, {})) text += line + "\n";
    return Hex(text);
}

// A linked package's own files: the parts of its folder its link names, the launcher's names aside.
static std::vector<std::string> LinkedLines(const PackageLink& link) {
    const auto generated = [](const std::string& path) { return IsGeneratedPath(path); };
    if (link.scope.empty()) return FileLines(link.root, generated);
    std::vector<std::string> lines;
    for (const std::string& part : link.scope) {
        const fs::path path = link.root / PathFromUtf8(part);
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(path, ec);
        if (fs::is_regular_file(status)) {
            std::error_code size_ec, time_ec;
            const uintmax_t size = fs::file_size(path, size_ec);
            const auto time = fs::last_write_time(path, time_ec);
            lines.push_back(part + "|" + std::to_string(size_ec ? 0 : size) + "|" + std::to_string(time_ec ? 0 : time.time_since_epoch().count()));
        } else if (fs::is_directory(status)) {
            for (const std::string& line : FileLines(path, {})) lines.push_back(part + "/" + line);
        } else {
            lines.push_back(part + "|absent");
        }
    }
    return lines;
}

std::string PackageListingKey(const fs::path& package_dir, const std::string& game_key, const PackageLink* link) {
    const std::string generated = LowerAscii(std::string(JPT_CUSTOM_LEVEL_GENERATED_DIR) + "/");
    const auto skip = [&generated](const std::string& path) {
        return path == generated + kRecordFile || path == generated + "worlds.txt" || path.rfind(generated + "guide/", 0) == 0;
    };
    std::string text = std::string(kRecordHead) + " " + Str(kListingVersion) + "|scene " + Str(JPT_SCENE_VERSION_MAX) + "|actions " + Str(JPT_GROFF_ACTION_OWN_LAST) +
                       "|runtime " + Str(JPT_CUSTOM_LEVEL_RUNTIME_VERSION) + "|manifest " + Str(kManifestSchema) + "|guide " + Str(kLevelGuideVersion) + "|game " + game_key + "|features ";
    for (int i = 0; const jpt_level_feature_t* feature = jpt_level_feature_at(i); ++i)
        text += Str(int(feature->status)) + Str(int(jpt_level_issue_kind("UNSUPPORTED_FEATURE", feature->name, 0)));
    text += "|script api";
    for (const jpt::script::ApiEntry& e : jpt::script::Api()) text += std::string(" ") + e.owner + "." + e.name + ":" + Str(int(e.scopes));
    text += "\n";
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(package_dir, ec))) return {};
    for (const std::string& line : FileLines(package_dir, skip)) text += line + "\n";
    if (link) {
        text += "link\n" + PackageLinkText(*link);
        for (const std::string& line : LinkedLines(*link)) text += line + "\n";
    }
    return Hex(text);
}

std::string ListingRecordText(const std::string& key, const LevelPackage& p) {
    std::string text = std::string(kRecordHead) + " " + Str(kListingVersion) + "\n" + Line("key", {key});
    text += Line("package", {Str(p.imported_at), p.layout, p.readme, Str(int(p.ce)), Str(int(p.atx)), Str(p.config_warnings), Str(p.recorded)});
    if (!p.origin.empty()) text += Line("origin", {p.origin});
    for (const std::string& line : p.log) text += Line("log", {line});
    for (const LevelScene& s : p.scenes) {
        text += Line("scene", {s.path, s.file, Str(s.valid), Str(s.start), Str(s.auxiliary), Str(s.version), Str(int(s.state)), s.note});
        for (const std::string& name : s.missing) text += Line("missing", {name});
        for (const std::string& use : s.uses) text += Line("use", {use});
        for (const std::string& part : s.left_out) text += Line("left", {part});
        for (const LevelResource& r : s.resources) text += Line("resource", {r.type, r.name, r.place, r.note});
        for (const LevelIssue& i : s.issues) text += Line("issue", {i.code, i.subject, i.detail, Str(int(i.policy))});
        for (const std::string& file : s.led_to_by) text += Line("led", {file});
        if (s.scripted) text += Line("scripted", {"1"});
        for (const std::string& file : s.script_files) text += Line("script_file", {file});
        for (const std::string& name : s.script_required) text += Line("script_required", {name});
        for (const auto& [world, facts] : s.worlds) text += Line("world", {world, WorldFactsText(facts)});
    }
    return text + "end\n";
}

bool ParseListingRecord(const std::string& text, const std::string& key, LevelPackage& package) {
    if (key.empty()) return false;
    LevelPackage p;
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != std::string(kRecordHead) + " " + Str(kListingVersion)) return false;
    bool keyed = false, described = false, ended = false;
    while (std::getline(in, line)) {
        if (line == "end") {
            ended = true;
            break;
        }
        const size_t space = line.find(' ');
        if (space == std::string::npos) return false;
        const std::string kind = line.substr(0, space);
        const std::vector<std::string> f = Fields(line.substr(space + 1));
        LevelScene* scene = p.scenes.empty() ? nullptr : &p.scenes.back();
        long long number = 0;
        if (kind == "key") {
            if (f.size() != 1 || f[0] != key) return false;
            keyed = true;
        } else if (kind == "package" && f.size() == 7) {
            if (!Number(f[0], std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max(), number) || !EnumOf(f[3], Need::kRequired, p.ce) || !EnumOf(f[4], Need::kRequired, p.atx) ||
                !Flag(f[5], p.config_warnings) || !Flag(f[6], p.recorded))
                return false;
            p.imported_at = number;
            p.layout = f[1];
            p.readme = f[2];
            described = true;
        } else if (kind == "log" && f.size() == 1) {
            p.log.push_back(f[0]);
        } else if (kind == "origin" && f.size() == 1) {
            p.origin = f[0];
        } else if (kind == "scene" && f.size() == 8) {
            LevelScene s;
            s.path = f[0];
            s.file = f[1];
            s.note = f[7];
            if (!Flag(f[2], s.valid) || !Flag(f[3], s.start) || !Flag(f[4], s.auxiliary) || !Number(f[5], 0, 1 << 20, number) ||
                !EnumOf(f[6], LevelState::kLimited, s.state))
                return false;
            s.version = int(number);
            p.scenes.push_back(std::move(s));
        } else if (!scene) {
            return false;
        } else if (kind == "missing" && f.size() == 1) {
            scene->missing.push_back(f[0]);
        } else if (kind == "use" && f.size() == 1) {
            scene->uses.push_back(f[0]);
        } else if (kind == "scripted" && f.size() == 1 && f[0] == "1") {
            scene->scripted = true;
        } else if (kind == "script_file" && f.size() == 1) {
            scene->script_files.push_back(f[0]);
        } else if (kind == "script_required" && f.size() == 1) {
            scene->script_required.push_back(f[0]);
        } else if (kind == "left" && f.size() == 1) {
            scene->left_out.push_back(f[0]);
        } else if (kind == "resource" && f.size() == 4) {
            scene->resources.push_back({f[0], f[1], f[2], f[3]});
        } else if (kind == "issue" && f.size() == 4) {
            LevelIssue issue{f[0], f[1], f[2], IssuePolicy::kInfo};
            if (!EnumOf(f[3], IssuePolicy::kMalformed, issue.policy)) return false;
            scene->issues.push_back(std::move(issue));
        } else if (kind == "led" && f.size() == 1) {
            scene->led_to_by.push_back(f[0]);
        } else if (kind == "world" && f.size() == 2) {
            WorldFacts facts;
            if (!ReadWorldFacts(f[1], facts)) return false;
            scene->worlds.push_back({f[0], std::move(facts)});
        } else {
            return false;
        }
    }
    if (!keyed || !described || !ended) return false;
    package = std::move(p);
    return true;
}

bool ReadListingRecord(const fs::path& package_dir, const std::string& key, LevelPackage& package) {
    const fs::path file = package_dir / JPT_CUSTOM_LEVEL_GENERATED_DIR / kRecordFile;
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec || size > kRecordLimit || !fs::is_regular_file(fs::symlink_status(file, ec))) return false;
    std::ifstream in(file, std::ios::binary);
    std::string text(size_t(size), '\0');
    if (!in || (size && !in.read(text.data(), std::streamsize(size)))) return false;
    return ParseListingRecord(text, key, package);
}

bool ReadLastListingRecord(const fs::path& package_dir, LevelPackage& package) {
    const fs::path file = package_dir / JPT_CUSTOM_LEVEL_GENERATED_DIR / kRecordFile;
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec || size > kRecordLimit || !fs::is_regular_file(fs::symlink_status(file, ec))) return false;
    std::ifstream in(file, std::ios::binary);
    std::string text(size_t(size), '\0');
    if (!in || (size && !in.read(text.data(), std::streamsize(size)))) return false;
    // Shown only while the folder is away, never played: the record of the version before has the same lines.
    const std::string previous = std::string(kRecordHead) + " " + Str(kListingVersion - 1) + "\n";
    if (text.rfind(previous, 0) == 0) text.replace(0, previous.size(), std::string(kRecordHead) + " " + Str(kListingVersion) + "\n");
    const size_t at = text.find("\nkey ");
    if (at == std::string::npos) return false;
    const size_t end = text.find('\n', at + 1);
    const std::vector<std::string> key = Fields(text.substr(at + 5, end == std::string::npos ? std::string::npos : end - at - 5));
    return key.size() == 1 && ParseListingRecord(text, key[0], package);
}

bool WriteListingRecord(const fs::path& package_dir, const std::string& key, const LevelPackage& package) {
    if (key.empty()) return false;
    const fs::path folder = package_dir / JPT_CUSTOM_LEVEL_GENERATED_DIR, file = folder / kRecordFile;
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(folder, ec)) || fs::is_symlink(fs::symlink_status(file, ec))) return false;
    fs::create_directories(folder, ec);
    fs::path temporary = file;
    temporary += ".new";
    {
        const std::string text = ListingRecordText(key, package);
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(text.data(), std::streamsize(text.size()));
        if (!out) return false;
    }
    fs::rename(temporary, file, ec);
    if (ec) fs::remove(temporary, ec);
    return !ec;
}

Listing ScanLevels(const ListingRequest& request, const std::atomic<bool>& cancelled) {
    const auto start = std::chrono::steady_clock::now();
    Listing listing;
    listing.packages = ListLevelPackages(request.user_dir, request.data_dir, &listing.stats, &cancelled);
    listing.total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return listing;
}

void LevelListing::Forget(const std::string& id) {
    auto& packages = list_.packages;
    packages.erase(std::remove_if(packages.begin(), packages.end(), [&id](const LevelPackage& p) { return p.id == id; }), packages.end());
}

void LevelListing::Invalidate(bool blank) {
    ++generation_;
    blank_ = blank_ || blank;
    job_.Cancel();
}

LevelListing::Frame LevelListing::Advance(const ListingRequest& request) {
    Frame frame;
    if (!has_request_ || !(request == request_)) {
        if (has_request_) Invalidate(true);
        request_ = request;
        has_request_ = true;
    }
    if (std::optional<BackgroundJob<Listing>::Done> done = job_.Collect()) {
        const bool current = done->serial == job_serial_ && job_generation_ == generation_;
        if (current && done->result && !done->result->stats.cancelled) {
            list_ = std::move(*done->result);
            listed_ = generation_;
            blank_ = false;
            error_.clear();
            frame.adopted = true;
        } else if (current && !done->result) {
            failed_ = generation_;
            error_ = done->error;
            frame.failed = true;
        }
    }
    if (failed_ == generation_) {
        loading_shown_ = false;
        frame.view = View::kFailed;
        return frame;
    }
    const bool current = listed_ == generation_;
    const bool loading = !current && (blank_ || listed_ == 0);
    if (!current && !job_.busy()) {
        if (loading && !loading_shown_) {
            loading_shown_ = true;
            frame.view = View::kLoading;
            return frame;
        }
        job_generation_ = generation_;
        job_serial_ = job_.Start([work = work_, request = request_](const std::atomic<bool>& cancelled) { return work(request, cancelled); });
        if (job_serial_) ++started_;
    }
    loading_shown_ = loading;
    frame.view = loading ? View::kLoading : View::kList;
    return frame;
}

} // namespace jpt
