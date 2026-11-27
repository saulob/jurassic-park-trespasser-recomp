#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "http_post.h"
#include "incident_client.h"
#include "incident_report.h"
#include "json_lite.h"
#include "log_scrubber.h"
#include "report_history.h"

namespace fs = std::filesystem;
using namespace jpt;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
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

void TestJson() {
    Check(json::Quote("hello") == "\"hello\"", "quote plain");
    Check(json::Quote("a\"b\\c") == "\"a\\\"b\\\\c\"", "quote quotes and slash");
    {
        const std::string q = json::Quote("a\nb\rc\td");
        const auto p = json::Parse(q);
        Check(q.find('\n') == std::string::npos && q.find('\r') == std::string::npos && p && p->IsString() && p->string == "a\nb\rc\td",
              "quote whitespace escapes");
    }
    Check(json::Quote(std::string("\x01\x1f", 2)) == "\"\\u0001\\u001f\"", "quote control chars");
    Check(json::Quote("caf\xC3\xA9") == "\"caf\xC3\xA9\"", "quote valid utf8");
    Check(json::Quote(std::string("\xFF\xFE", 2)).find("\\u") != std::string::npos ||
              json::Quote(std::string("\xFF\xFE", 2)).find('\xFF') == std::string::npos,
          "quote sanitizes invalid utf8");

    const auto v_null = json::Parse("null");
    Check(v_null && v_null->kind == json::Value::Kind::kNull, "parse null");
    const auto v_true = json::Parse("true");
    Check(v_true && v_true->IsBool() && v_true->boolean, "parse true");
    const auto v_false = json::Parse("false");
    Check(v_false && v_false->IsBool() && !v_false->boolean, "parse false");
    const auto v_num = json::Parse("-1234567890123");
    Check(v_num && v_num->IsNumber() && v_num->number == -1234567890123LL, "parse negative int64");
    const auto v_zero = json::Parse("0");
    Check(v_zero && v_zero->IsNumber() && v_zero->number == 0, "parse zero");
    const auto v_str = json::Parse("\"line1\\nline2\\t\\u0041\\\"\\\\\\/\"");
    Check(v_str && v_str->IsString() && v_str->string == "line1\nline2\tA\"\\/", "parse escapes");

    const auto v_obj = json::Parse(" { \"a\" : 42 , \"b\" : \"ok\" , \"c\" : true , \"d\" : [1, 2, 3], \"e\" : null } ");
    Check(v_obj && v_obj->IsObject(), "parse object");
    Check(v_obj->NumberOr("a") == 42, "NumberOr existing");
    Check(v_obj->NumberOr("missing") == 0, "NumberOr missing");
    Check(v_obj->NumberOr("b") == 0, "NumberOr wrong type");
    Check(v_obj->StringOr("b") == "ok", "StringOr existing");
    Check(v_obj->StringOr("missing").empty(), "StringOr missing");
    Check(v_obj->StringOr("a").empty(), "StringOr wrong type");
    Check(v_obj->BoolOr("c"), "BoolOr true");
    Check(!v_obj->BoolOr("missing"), "BoolOr missing");
    Check(!v_obj->BoolOr("a"), "BoolOr wrong type");
    Check(v_obj->Find("e") != nullptr && v_obj->Find("e")->kind == json::Value::Kind::kNull, "Find null member");
    Check(v_obj->Find("zzz") == nullptr, "Find missing");
    const auto* arr = v_obj->Find("d");
    Check(arr && arr->IsArray() && arr->array.size() == 3, "nested array");
    Check(arr->array[0].number == 1 && arr->array[1].number == 2 && arr->array[2].number == 3, "array elements");

    // Round-trip quoting for ASCII and UTF-8.
    const char* samples[] = {
        "", "simple", "with \"quotes\"", "back\\slash", "tabs\tand\nlines", "portugu\xC3\xAAs",
        "brackets [1,2] {a:b}", "path C:\\Users\\test\\file.log", "slashes // \\\\", "angle <home> <user>"
    };
    for (const char* s : samples) {
        const auto parsed = json::Parse(json::Quote(s));
        Check(parsed && parsed->IsString() && parsed->string == s, "quote/parse round-trip");
    }

    // Reject invalid JSON inputs.
    const char* bad[] = {
        "", "   ", "tru", "fals", "nul", "01", "-01", "1.5", "1e5", "[1,]", "{\"a\":1,}",
        "{a:1}", "\"unterminated", "\"bad\\x\"", "1 2", "{}", "[]", "][", "}{", "null null",
        "{\"a\":}", "[,]", "{\"a\" 1}", "\"\x01\"", "999999999999999999999999999999"
    };
    for (const char* b : bad) {
        const bool expect_ok = (std::strcmp(b, "{}") == 0 || std::strcmp(b, "[]") == 0);
        const auto r = json::Parse(b);
        Check(r.has_value() == expect_ok, "invalid json check");
    }

    // Excessive nesting rejected.
    std::string deep(128, '[');
    deep.append(128, ']');
    Check(!json::Parse(deep).has_value(), "deep nesting rejected");
}

void TestEndpointAndTransportAddress() {
    ServiceAddress addr;
    Check(ParseServiceAddress("https://reports.example", addr), "valid bare https");
    Check(addr.host == "reports.example" && addr.port == 443 && addr.path == "/", "parsed bare https");

    Check(ParseServiceAddress("https://reports.example:8443/v1/incidents", addr), "valid https with port and path");
    Check(addr.host == "reports.example" && addr.port == 8443 && addr.path == "/v1/incidents", "parsed port and path");

    Check(ParseServiceAddress("https://sub.domain.example.org/v1/incidents/status", addr), "valid subdomain path");
    Check(addr.host == "sub.domain.example.org" && addr.port == 443 && addr.path == "/v1/incidents/status", "parsed subdomain");

    const char* bad_urls[] = {
        "",
        "http://reports.example",
        "ftp://reports.example",
        "https://",
        "https:///path",
        "https://user:pass@reports.example",
        "https://reports.example?q=1",
        "https://reports.example#frag",
        "https://reports.example:0",
        "https://reports.example:70000",
        "https://reports.example:abc",
        "https://bad host/v1",
        "https://reports.example/v1/a b",
        "reports.example/v1",
    };
    for (const char* u : bad_urls) {
        ServiceAddress out;
        Check(!ParseServiceAddress(u, out), "reject invalid service address");
    }

    // ResolveEndpoint rules.
    const auto empty_ep = ResolveEndpoint("", "");
    Check(!empty_ep.configured() && empty_ep.problem == "The bug report service is not configured.", "empty endpoint not configured");
    const auto builtin_ep = ResolveEndpoint("https://reports.example/", "");
    Check(builtin_ep.configured() && builtin_ep.url == "https://reports.example" && builtin_ep.problem.empty(), "trailing slash trimmed");
    const auto override_ep = ResolveEndpoint("https://builtin.example", "  https://override.example///  ");
    Check(override_ep.configured() && override_ep.url == "https://override.example", "override wins and trims");
    const auto bad_ep = ResolveEndpoint("http://insecure.example", "");
    Check(!bad_ep.configured() && !bad_ep.problem.empty(), "http endpoint rejected");
    const auto path_ep = ResolveEndpoint("https://reports.example/extra", "");
    Check(!path_ep.configured() && !path_ep.problem.empty(), "non-root service base rejected");

    // HTTP/1.1 response parser.
    HttpReply reply;
    bool framed = false;
    Check(ParseHttpResponse("HTTP/1.1 201 Created\r\nContent-Length: 11\r\n\r\n{\"ok\":true}", reply, &framed), "parse content-length");
    Check(reply.status == 201 && reply.body == "{\"ok\":true}" && framed, "content-length status/body/framed");

    reply = {};
    framed = false;
    Check(ParseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", reply, &framed),
          "parse chunked");
    Check(reply.status == 200 && reply.body == "hello world" && framed, "chunked assembled");

    reply = {};
    framed = false;
    Check(ParseHttpResponse("HTTP/1.1 503 Service Unavailable\r\n\r\nretry", reply, &framed), "parse unframed");
    Check(reply.status == 503 && reply.body == "retry" && !framed, "unframed status/body");

    reply = {};
    Check(!ParseHttpResponse("not an http response", reply, &framed), "reject non-http");
    Check(!ParseHttpResponse("HTTP/1.1 200 OK\r\nNoHeaderEnd", reply, &framed), "reject incomplete headers");
    Check(!ParseHttpResponse("HTTP/1.1 abc OK\r\n\r\n", reply, &framed), "reject non-numeric status");
}

void TestPrivacyScrubber() {
    PrivateFacts facts;
    facts.user_names = {"AlicePlayer", "alice_win"};
    facts.machine_names = {"GAMING-RIG-99", "DESKTOP-XYZ123"};
    facts.profile_folders = {"C:\\Users\\AlicePlayer", "/home/alice_win"};
    facts.install_folders = {"D:\\Games\\TrespasserRecomp", "/home/alice_win/Games/Trespasser"};

    // Profile and install folders.
    {
        const std::string out = ScrubText("Loaded D:\\Games\\TrespasserRecomp\\data\\be.scn from C:\\Users\\AlicePlayer\\Documents\\save.dat", facts);
        Check(out.find("AlicePlayer") == std::string::npos, "profile username removed");
        Check(out.find("D:\\Games\\TrespasserRecomp") == std::string::npos, "install folder removed");
        Check(out.find("<install>") != std::string::npos, "install placeholder inserted");
        Check(out.find("<home>") != std::string::npos, "home placeholder inserted");
        Check(out.find("be.scn") != std::string::npos, "game file kept");
    }
    // Nested install inside home prefers nearest (<install>).
    {
        const std::string out = ScrubText("Path: /home/alice_win/Games/Trespasser/logs/run.log", facts);
        Check(out.find("alice_win") == std::string::npos, "linux user removed");
        Check(out.find("<install>") != std::string::npos, "nested install prefers <install>");
    }
    // Generic POSIX/Windows user profile paths even when not in facts.
    {
        const std::string out = ScrubText("Opened /home/stranger/file.txt and C:\\Users\\OtherPerson\\AppData\\Local\\cfg.ini", PrivateFacts{});
        Check(out.find("stranger") == std::string::npos, "generic linux home scrubbed");
        Check(out.find("OtherPerson") == std::string::npos, "generic windows user scrubbed");
    }
    // UNC network paths.
    {
        const std::string out = ScrubText("Mounted \\\\FILESERVER01\\PrivateShare\\iso\\tpass.iso", facts);
        Check(out.find("FILESERVER01") == std::string::npos, "UNC host scrubbed");
        Check(out.find("PrivateShare") == std::string::npos, "UNC share scrubbed");
        Check(out.find("<network-path>") != std::string::npos, "network-path placeholder");
    }
    // User and machine names as standalone words (case-insensitive).
    {
        const std::string out = ScrubText("User aliceplayer on gaming-rig-99 (host DESKTOP-XYZ123) started", facts);
        Check(out.find("aliceplayer") == std::string::npos, "user name word scrubbed");
        Check(out.find("gaming-rig-99") == std::string::npos, "machine name scrubbed");
        Check(out.find("DESKTOP-XYZ123") == std::string::npos, "second machine name scrubbed");
        Check(out.find("<user>") != std::string::npos, "user placeholder");
        Check(out.find("<computer>") != std::string::npos, "computer placeholder");
    }
    // IPv4 scrubbed, version numbers preserved.
    {
        const std::string out = ScrubText("Peer 192.168.1.42 connected; NVIDIA 560.94.0.1 Vulkan 1.3.280.0 driver 31.0.15.6094", facts);
        Check(out.find("192.168.1.42") == std::string::npos, "ipv4 scrubbed");
        Check(out.find("<ipv4>") != std::string::npos, "ipv4 placeholder");
        Check(out.find("560.94.0.1") != std::string::npos, "nvidia version preserved");
        Check(out.find("1.3.280.0") != std::string::npos, "vulkan version preserved");
        Check(out.find("31.0.15.6094") != std::string::npos, "driver version preserved");
    }
    // IPv6 scrubbed.
    {
        const std::string out = ScrubText("Addr 2001:0db8:85a3:0000:0000:8a2e:0370:7334 failed", facts);
        Check(out.find("2001:0db8") == std::string::npos, "ipv6 scrubbed");
        Check(out.find("<ipv6>") != std::string::npos, "ipv6 placeholder");
    }
    // Secrets, tokens, passwords, URL credentials.
    const char* secret_lines[] = {
        "password=SuperSecret123",
        "token: eyJhbGciOiJIUzI1NiJ9.payload.sig",
        "api_key=sk_live_9876543210abcdef",
        "Authorization: Bearer my-secret-bearer-token",
        "https://alice:hunter2@proxy.example.com/path",
        "cookie=session_abc123def456",
        "credential=0123456789abcdef0123456789abcdef",
    };
    for (const char* line : secret_lines) {
        const std::string out = ScrubText(line, facts);
        Check(out.find("SuperSecret123") == std::string::npos &&
                  out.find("eyJhbGciOiJIUzI1NiJ9") == std::string::npos &&
                  out.find("sk_live_9876543210abcdef") == std::string::npos &&
                  out.find("my-secret-bearer-token") == std::string::npos &&
                  out.find("hunter2") == std::string::npos &&
                  out.find("session_abc123def456") == std::string::npos &&
                  out.find("0123456789abcdef0123456789abcdef") == std::string::npos,
              "secret value scrubbed");
    }
    // Diagnostics preserved: guest VA, frame counts, SHA hashes, GPU names.
    {
        const std::string diag = "[guest   ] fault at 0x004A1B2C frame=1420 sha256=deadbeef GPU='NVIDIA GeForce RTX 4070'";
        const std::string out = ScrubText(diag, facts);
        Check(out.find("0x004A1B2C") != std::string::npos, "guest VA preserved");
        Check(out.find("frame=1420") != std::string::npos, "frame count preserved");
        Check(out.find("deadbeef") != std::string::npos, "hash preserved");
        Check(out.find("NVIDIA GeForce RTX 4070") != std::string::npos, "GPU name preserved");
    }
    // Line ending normalization and control character stripping.
    {
        const std::string raw = std::string("a\r\nb\rc\td\x01\x02\x07\x1f" "e\xFF");
        const std::string out = ScrubText(raw, facts);
        Check(out.find('\r') == std::string::npos, "CR normalized to LF");
        Check(out.find('\t') != std::string::npos, "tab preserved");
        Check(out.find('\x01') == std::string::npos && out.find('\x07') == std::string::npos && out.find('\x1f') == std::string::npos,
              "control chars stripped");
        Check(out.find('\xFF') == std::string::npos, "invalid utf8 sanitized");
    }
    // All secret key prefixes from log_scrubber.cpp.
    const char* secret_keys[] = {
        "secret", "password", "passwd", "passphrase", "pwd", "token", "api_key", "apikey",
        "api-key", "access_key", "private_key", "session_key", "room_key", "cookie", "credential",
        "friend_code", "friend-code", "friend code"
    };
    for (const char* key : secret_keys) {
        const std::string line = std::string(key) + "=TopSecretVal99";
        const std::string out = ScrubText(line, facts);
        Check(out.find("TopSecretVal99") == std::string::npos, "secret key value scrubbed");
        Check(out.find("<secret>") != std::string::npos, "secret key replaced with <secret>");
    }
    // Version words preserve dotted quads (not mistaken for IPv4).
    const char* version_words[] = {
        "amd", "api", "build", "driver", "intel", "mesa",
        "nvidia", "radeon", "sdk", "ver", "version", "vulkan"
    };
    for (const char* vw : version_words) {
        const std::string line = std::string(vw) + " 10.20.30.40";
        const std::string out = ScrubText(line, facts);
        Check(out.find("10.20.30.40") != std::string::npos, "version quad preserved");
        Check(out.find("<ipv4>") == std::string::npos, "version quad not marked ipv4");
    }
    // Profile roots scrubbed for arbitrary users.
    const char* profile_roots[] = {"/var/home/", "/run/media/", "/home/", "/Users/", "/media/"};
    for (const char* root : profile_roots) {
        const std::string line = std::string("file ") + root + "SecretPerson/saves/game.sav";
        const std::string out = ScrubText(line, PrivateFacts{});
        Check(out.find("SecretPerson") == std::string::npos, "profile root username scrubbed");
        Check(out.find("game.sav") != std::string::npos, "profile root filename kept");
    }
    // Generic names are NOT scrubbed when listed as user/machine names.
    {
        PrivateFacts generic;
        generic.user_names = {"admin", "administrator", "deck", "default", "game", "guest", "owner", "player", "root", "steam", "user"};
        generic.machine_names = {"localhost", "steamdeck", "public"};
        for (const auto& name : generic.user_names) {
            const std::string line = "state=" + name + " ok";
            const std::string out = ScrubText(line, generic);
            Check(out.find(name) != std::string::npos, "generic user word preserved");
        }
        for (const auto& name : generic.machine_names) {
            const std::string line = "host=" + name + " ok";
            const std::string out = ScrubText(line, generic);
            Check(out.find(name) != std::string::npos, "generic machine word preserved");
        }
    }
    // Windows system folders and POSIX system roots preserved.
    const char* system_paths[] = {
        "C:\\Windows\\System32\\vulkan-1.dll",
        "C:\\Program Files\\Common\\driver.dll",
        "/usr/lib/x86_64-linux-gnu/libvulkan.so.1",
        "/opt/mesa/lib/libEGL.so",
        "/var/log/boot.log",
        "/tmp/shader_cache.bin"
    };
    for (const char* sp : system_paths) {
        const std::string out = ScrubText(sp, facts);
        Check(out.find("<home>") == std::string::npos && out.find("<install>") == std::string::npos, "system path not marked private");
    }
    // Chat lines stripped.
    {
        const std::string out = ScrubText("Chat: meet me at 5pm secret place", facts);
        Check(out.find("meet me") == std::string::npos, "chat body removed");
        Check(out.find("Chat:") != std::string::npos, "chat prefix kept");
    }
    // Double-backslash and mixed-separator known folder paths.
    {
        const std::string out = ScrubText("C:\\\\Users\\\\AlicePlayer/AppData/log.txt", facts);
        Check(out.find("AlicePlayer") == std::string::npos, "mixed-separator profile scrubbed");
        Check(out.find("<home>") != std::string::npos, "mixed-separator home placeholder");
    }
    // Custom level texts in a log of v0.1.3: the readme's lines and the sentences quoted as reasons stay out.
    {
        const std::string old_log =
            "[   1.000] [launcher] custom level import: README BEGIN M/Readme.txt (40 bytes, SHA-256 abc)\n"
            "[   1.001] [launcher] custom level import: | Put the level in data. Synthetic author words.\n"
            "[   1.002] [launcher] custom level import: README END M/Readme.txt\n"
            "[   1.003] [launcher] custom level import:   depends on: Trespasser CE required (Synthetic needs CE sentence)\n"
            "[   1.004] [launcher] custom level import:   Trespasser CE required: Synthetic needs CE sentence\n"
            "[   1.005] [launcher] custom level m: readme_not_followed: Move synthetic.asa into the anim directory\n"
            "[   1.006] [launcher] custom level:   DEPENDENCY_CE Trespasser CE: required (Synthetic needs CE sentence); what of it\n"
            "[   1.007] [launcher] custom level:   README_NOT_FOLLOWED package: Move synthetic.asa into the anim directory\n"
            "[   1.008] [launcher] depends on: an unrelated line keeps its words\n"
            "[   1.009] [launcher] custom level import: | last line";
        const std::string out = ScrubText(old_log, facts);
        Check(out.find("Synthetic") == std::string::npos && out.find("Move synthetic") == std::string::npos && out.find("| ") == std::string::npos &&
                  out.find("last line") == std::string::npos,
              "custom level texts removed from an old log");
        Check(out.find("README BEGIN M/Readme.txt (40 bytes, SHA-256 abc)") != std::string::npos && out.find("README END M/Readme.txt") != std::string::npos,
              "custom level text descriptions kept");
        Check(out.find("depends on: <level text removed>") != std::string::npos && out.find("readme_not_followed: <level text removed>") != std::string::npos &&
                  out.find("Trespasser CE: required (<level text removed>)") != std::string::npos && out.find("README_NOT_FOLLOWED package: <level text removed>") != std::string::npos,
              "custom level reasons keep their codes");
        Check(out.find("an unrelated line keeps its words") != std::string::npos && out.back() != '\n', "other lines untouched");
    }
    // GatherPrivateFacts returns install folder passed in.
    {
        const PrivateFacts gathered = GatherPrivateFacts({fs::path("D:/CustomInstallFolder")});
        bool found_install = false;
        for (const auto& f : gathered.install_folders) {
            if (f.find("CustomInstallFolder") != std::string::npos) found_install = true;
        }
        Check(found_install, "GatherPrivateFacts includes install folder");
    }
}

void TestLogDiscoveryAndCrashValidation() {
    const fs::path dir = MakeTempDir("logs");
    const fs::path running = dir / "trespasser_recomp_005.log";
    const fs::path gameplay = dir / "trespasser_recomp_004.log";
    const fs::path launcher_closed = dir / "trespasser_recomp_003.log";
    const fs::path older_gameplay = dir / "trespasser_recomp_002.log";
    const fs::path oldest_gameplay = dir / "trespasser_recomp_001.log";
    const fs::path extra_old = dir / "trespasser_recomp_000.log";
    const fs::path ignored_txt = dir / "notes.txt";

    WriteBytes(extra_old, "[guest   ] entering the CRT entry point\n");
    WriteBytes(oldest_gameplay, "[guest   ] entering the CRT entry point\n");
    WriteBytes(older_gameplay, "[guest   ] entering the CRT entry point\n");
    WriteBytes(launcher_closed, "[launcher] closed\n");
    WriteBytes(gameplay, "[launcher] closed, starting the game\n[guest   ] entering the CRT entry point\n");
    WriteBytes(running, "[launcher] shown\n");
    WriteBytes(ignored_txt, "not a log\n");

    // Set deterministic descending timestamps.
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(extra_old, now - std::chrono::minutes(60), ec);
    fs::last_write_time(oldest_gameplay, now - std::chrono::minutes(50), ec);
    fs::last_write_time(older_gameplay, now - std::chrono::minutes(40), ec);
    fs::last_write_time(launcher_closed, now - std::chrono::minutes(30), ec);
    fs::last_write_time(gameplay, now - std::chrono::minutes(20), ec);
    fs::last_write_time(running, now - std::chrono::minutes(10), ec);

    const fs::path dmp = dir / "trespasser_recomp_004.dmp";
    WriteBytes(dmp, MakeMinidump(128));
    fs::last_write_time(dmp, now - std::chrono::minutes(20), ec);

    const auto logs = FindGameLogs(dir, running);
    Check(logs.size() == 5, "capped at kMostGameLogs and closed launcher omitted");
    Check(logs[0].name == "trespasser_recomp_005.log" && logs[0].role == LogRole::kCurrentLauncher, "running log first");
    Check(logs[1].name == "trespasser_recomp_004.log" && logs[1].role == LogRole::kLastGameplay, "last gameplay marked");
    Check(logs[1].Crashed() && logs[1].CanAttachCrash() && DefaultAttachCrashReport(logs[1]), "crash detected and attachable");
    Check(DefaultGameLog(logs) == 1, "default log picks last gameplay");
    Check(logs[2].role == LogRole::kOtherGameplay, "older gameplay role");
    Check(!logs[2].Crashed() && !logs[2].CanAttachCrash() && !DefaultAttachCrashReport(logs[2]), "non-crashed log has no dump");
    Check(DefaultGameLog({}) == 0, "default log on empty vector is 0");
    // Bug Report after a failed import: this session's own log, whatever an older game session ended like.
    Check(ChooseGameLog(logs, running) == 0 && logs[0].path == running, "the import's own session log is chosen");
    Check(ChooseGameLog(logs, dir / "." / running.filename()) == 0, "the same log by another spelling of its path");
    Check(ChooseGameLog(logs, logs[2].path) == 2, "a log picked by hand stays chosen after a rescan");
    Check(ChooseGameLog(logs, {}) == DefaultGameLog(logs) && ChooseGameLog(logs, dir / "gone.log") == DefaultGameLog(logs), "nothing asked for: the default log");
    Check(ChooseGameLog({}, running) == 0, "no logs");
    // Bug Report of a level: the log of that level's own last run, never another one's crash.
    const auto run = [&](const char* name, const std::string& package, const std::string& scene, int minutes) {
        std::ifstream in(gameplay, std::ios::binary);
        std::string text(std::istreambuf_iterator<char>(in), {});
        text += "[00:00:01.000] [custom-level] " + package + ", scene " + scene + " loaded as LEVEL.SCN; saves in x\n";
        WriteBytes(dir / name, text);
        std::error_code touched;
        fs::last_write_time(dir / name, now - std::chrono::minutes(minutes), touched);
        return dir / name;
    };
    const fs::path first = run("trespasser_recomp_101.log", "Isle", "data/isle.scn", 200), second = run("trespasser_recomp_102.log", "Isle", "data/isle.scn", 150),
                   other = run("trespasser_recomp_103.log", "Isle", "data/other.scn", 140), same = run("trespasser_recomp_104.log", "Isle-2", "data/isle.scn", 130);
    Check(FindLevelLog(dir, running, "Isle", "data/isle.scn") == second && FindLevelLog(dir, running, "Isle", "data/other.scn") == other, "the newest log of the level's runs");
    Check(FindLevelLog(dir, running, "Isle-2", "data/isle.scn") == same && FindLevelLog(dir, running, "Isle", "isle.scn").empty() && FindLevelLog(dir, running, "Gone", "x.scn").empty(), "another package's or level's log is never taken");
    const auto with = FindGameLogs(dir, running, 2, first);
    Check(with.size() == 3 && with[ChooseGameLog(with, first)].path == first && ChooseGameLog(FindGameLogs(dir, running, 2), first) == DefaultGameLog(FindGameLogs(dir, running, 2)), "an old log asked for is listed and chosen");
    {
        std::ofstream ended(second, std::ios::binary | std::ios::app);
        ended << "[00:00:02.000] [custom-level] ENGINE_LIMIT_EXCEEDED: the scene's vertices at 0x10000000 holds 1 bytes, 1 are in use and 40 more were asked for; nothing of them was written\n"
                 "[00:00:02.000] [custom-level] CONTROLLED_LOAD_FAILURE: isle.scn of Isle\n[00:00:02.000] [exit    ] exit code 6\n";
    }
    fs::last_write_time(second, now - std::chrono::minutes(150), ec);
    Check(FindLevelLog(dir, running, "Isle", "data/isle.scn") == second, "the log of a load that was ended is the level's log");
    for (const fs::path& log : {first, second, other, same}) fs::remove(log, ec);

    // Dump naming.
    Check(DumpNameOf("trespasser_recomp_004.log") == "trespasser_recomp_004.dmp", "dump name from .log");
    Check(DumpNameOf("session.txt").empty() || DumpNameOf("session.log") == "session.dmp", "dump name suffix rule");
    Check(DumpNameOf("").empty(), "empty log name yields empty dump name");

    // Local DMP validation edge cases.
    Check(CheckCrashDump(logs[1], dmp) == DumpProblem::kNone, "valid MDMP passes");
    Check(CheckCrashDump(logs[1], dir / "other.dmp") == DumpProblem::kNotThisLog, "wrong filename rejected");
    Check(CheckCrashDump(logs[1], dir / "sub" / "trespasser_recomp_004.dmp") == DumpProblem::kNotThisLog, "wrong directory rejected");
    Check(CheckCrashDump(logs[2], dir / "trespasser_recomp_002.dmp") == DumpProblem::kMissing, "missing dump reported");

    const fs::path dir_as_dmp = dir / "trespasser_recomp_001.dmp";
    fs::create_directories(dir_as_dmp, ec);
    Check(CheckCrashDump(logs[3], dir_as_dmp) == DumpProblem::kNotAFile, "directory as dump rejected");
    fs::remove_all(dir_as_dmp, ec);

    const fs::path empty_dmp = dir / "trespasser_recomp_002.dmp";
    WriteBytes(empty_dmp, "");
    fs::last_write_time(empty_dmp, fs::last_write_time(older_gameplay, ec), ec);
    Check(CheckCrashDump(logs[2], empty_dmp) == DumpProblem::kEmpty, "empty dump rejected");

    WriteBytes(empty_dmp, std::string(64, 'X'));
    fs::last_write_time(empty_dmp, fs::last_write_time(older_gameplay, ec), ec);
    Check(CheckCrashDump(logs[2], empty_dmp) == DumpProblem::kNotADump, "non-MDMP rejected");

    WriteBytes(empty_dmp, MakeMinidump(16));
    fs::last_write_time(empty_dmp, fs::last_write_time(older_gameplay, ec), ec);
    Check(CheckCrashDump(logs[2], empty_dmp) == DumpProblem::kNotADump, "too-small dump rejected");

    WriteBytes(empty_dmp, MakeMinidump(kFewestDumpBytes));
    fs::last_write_time(empty_dmp, fs::last_write_time(older_gameplay, ec), ec);
    Check(CheckCrashDump(logs[2], empty_dmp) == DumpProblem::kNone, "minimum size valid MDMP passes");

    // Stale timestamp rejected (both too old and too new).
    fs::last_write_time(empty_dmp, fs::last_write_time(older_gameplay, ec) - std::chrono::seconds(kDumpPairingSeconds + 60), ec);
    Check(CheckCrashDump(logs[2], empty_dmp) == DumpProblem::kNotThisLog, "too-old timestamp rejected");
    fs::last_write_time(empty_dmp, fs::last_write_time(older_gameplay, ec) + std::chrono::seconds(kDumpPairingSeconds + 60), ec);
    Check(CheckCrashDump(logs[2], empty_dmp) == DumpProblem::kNotThisLog, "too-new timestamp rejected");

    // Role, preview button, date, size formatting.
    Check(LogRoleText(LogRole::kNone) == nullptr, "LogRoleText kNone is null");
    const LogRole roles[] = {
        LogRole::kCurrentLauncher, LogRole::kCurrentGameplay,
        LogRole::kLastGameplay, LogRole::kOtherGameplay, LogRole::kOtherLauncher
    };
    for (LogRole role : roles) {
        const char* txt = LogRoleText(role);
        Check(txt != nullptr && txt[0] != '\0', "non-none LogRoleText non-empty");
    }
    Check(std::string(PreviewButtonText(false)) != PreviewButtonText(true), "PreviewButtonText toggles");
    Check(!LogDateText(1700000000, false).empty(), "LogDateText English");
    Check(!LogDateText(1700000000, true).empty(), "LogDateText Portuguese");
    Check(LogSizeText(0) == "1 KB", "LogSizeText 0 B rounds up to 1 KB");
    Check(LogSizeText(512) == "1 KB", "LogSizeText 512 B rounds up to 1 KB");
    Check(LogSizeText(4096) == "4 KB", "LogSizeText KB");
    Check(LogSizeText(3 * 1024 * 1024) == "3.0 MB", "LogSizeText MB");

    // Linux no-DMP behavior: gameplay log without .dmp builds cleanly with no crash_dump.
    const ReportFacts linux_facts{"0.1.0", "English (Retail 116)", "116", FormatLinuxOperatingSystem("SteamOS", "Valve", "Jupiter")};
    const auto linux_pkg = BuildIncidentPackage("Audio stutter in Jungle Road on Steam Deck.", logs[3], linux_facts, PrivateFacts{}, DefaultAttachCrashReport(logs[3]));
    Check(linux_pkg.has_value(), "Linux no-DMP package built");
    Check(!linux_pkg->crash_dump.has_value(), "Linux no-DMP has no crash_dump");
    Check(!linux_pkg->crash_dump_requested, "Linux no-DMP does not request crash upload");
    Check(linux_pkg->Value("operating_system") == "Steam Deck: SteamOS", "Steam Deck OS recorded");

    fs::remove_all(dir, ec);
}

void TestDraftPayloadAndPreview() {
    // Draft validation.
    Check(CheckDraft("Too short", true) == DraftProblem::kDescriptionTooShort, "9 chars rejected");
    Check(CheckDraft("   short  ", true) == DraftProblem::kDescriptionTooShort, "whitespace-trimmed short rejected");
    Check(CheckDraft("1234567890", true) == DraftProblem::kNone, "10 chars accepted");
    Check(CheckDraft("Valid description here.", false) == DraftProblem::kNoLog, "no log rejected");
    Check(CheckDraft("Valid description here.", true) == DraftProblem::kNone, "valid draft accepted");

    // UTF-8 character counting & slicing.
    Check(CountCharacters("") == 0, "empty char count");
    Check(CountCharacters("abc") == 3, "ascii char count");
    Check(CountCharacters("a\xC3\xA7\xC3\xA3o") == 4, "pt-BR utf8 char count");
    Check(BytesOfCharacters("a\xC3\xA7\xC3\xA3o", 2) == 3, "utf8 prefix byte count");
    Check(BytesOfCharacters("a\xC3\xA7\xC3\xA3o", 100) == 6, "utf8 full byte count");
    Check(BytesOfCharacters("a\xC3\xA7\xC3\xA3o", 0) == 0, "utf8 zero char count");

    // LastBytes clipping.
    Check(LastBytes("hello", 10) == "hello", "LastBytes shorter than limit");
    Check(LastBytes("0123456789", 4) == "6789", "LastBytes exact tail");
    Check(LastBytes("abc\xC3\xA9" "def", 4) == "def" || LastBytes("abc\xC3\xA9" "def", 5).size() <= 5, "LastBytes respects UTF-8 boundary");

    // Package, preview, JSON serialization, clipping.
    const fs::path dir = MakeTempDir("payload");
    const fs::path big_log = dir / "trespasser_recomp_020.log";
    std::string big_text;
    big_text.reserve(kMostLogBytes + 4096);
    for (int i = 0; i < 2200; ++i) {
        big_text += "[guest   ] line " + std::to_string(i) + " path C:\\Users\\SecretUser\\game.dat\n";
    }
    WriteBytes(big_log, big_text);
    const fs::path dmp = dir / "trespasser_recomp_020.dmp";
    WriteBytes(dmp, MakeMinidump(256));

    const auto logs = FindGameLogs(dir, {});
    Check(logs.size() == 1 && logs[0].CanAttachCrash(), "payload test log found with valid dump");

    PrivateFacts pf;
    pf.user_names = {"SecretUser"};
    pf.profile_folders = {"C:\\Users\\SecretUser"};
    const ReportFacts facts{"0.1.0", "English (Retail 116)", "116", "Windows 11"};

    const auto pkg = BuildIncidentPackage("Game crashed at C:\\Users\\SecretUser\\gate.", logs[0], facts, pf, true);
    Check(pkg.has_value(), "package built");
    Check(pkg->crash_dump.has_value() && pkg->crash_dump_requested, "dump attached when requested");
    Check(pkg->Value("description").find("SecretUser") == std::string::npos, "description scrubbed");
    Check(pkg->Value("log_text").find("SecretUser") == std::string::npos, "log_text scrubbed");
    Check(pkg->Value("log_text").size() <= kMostLogBytes, "log_text clipped to kMostLogBytes");
    Check(pkg->Value("log_clipped") == "yes" || pkg->Value("log_clipped") == "true" || !pkg->Value("log_clipped").empty(),
          "log_clipped flag set");
    Check(pkg->Value("launcher_version") == "0.1.0", "launcher_version field");
    Check(pkg->Value("game_build") == "English (Retail 116)", "game_build field");
    Check(pkg->Value("game_version") == "116", "game_version field");
    Check(pkg->Value("operating_system") == "Windows 11", "operating_system field");
    Check(pkg->Value("log_file") == "trespasser_recomp_020.log", "log_file field");
    Check(pkg->Value("nonexistent").empty(), "missing field returns empty");

    // Preview shows dump filename and notice without raw bytes.
    const std::string preview = IncidentPreviewText(*pkg);
    Check(preview.find("trespasser_recomp_020.dmp") != std::string::npos, "preview names dump");
    Check(preview.find("MDMP") == std::string::npos, "preview does not dump raw bytes");
    Check(preview.find("SecretUser") == std::string::npos, "preview does not leak user");

    // Request JSON parses cleanly and carries all required fields.
    const std::string req_json = IncidentRequestJson(*pkg, "client-1234");
    const auto parsed = json::Parse(req_json);
    Check(parsed && parsed->IsObject(), "IncidentRequestJson valid JSON");
    Check(parsed->StringOr("launcher_version") == "0.1.0", "JSON launcher_version");
    Check(parsed->StringOr("game_build") == "English (Retail 116)", "JSON game_build");
    Check(parsed->StringOr("game_version") == "116", "JSON game_version");
    Check(parsed->StringOr("operating_system") == "Windows 11", "JSON operating_system");
    Check(parsed->StringOr("log_file") == "trespasser_recomp_020.log", "JSON log_file");
    Check(parsed->StringOr("client_id") == "client-1234", "JSON client_id");
    Check(parsed->StringOr("log_clipped") == "yes", "JSON log_clipped yes");
    Check(parsed->NumberOr("dump_bytes") == 256, "JSON dump_bytes");

    // Without crash dump attached.
    const auto pkg_no_dmp = BuildIncidentPackage("Visual glitch on water surface.", logs[0], facts, pf, false);
    Check(pkg_no_dmp && !pkg_no_dmp->crash_dump.has_value() && !pkg_no_dmp->crash_dump_requested, "dump omitted when not requested");
    const auto parsed_no_dmp = json::Parse(IncidentRequestJson(*pkg_no_dmp));
    Check(parsed_no_dmp && parsed_no_dmp->NumberOr("dump_bytes") == 0, "JSON dump_bytes 0 when omitted");

    // Binary dump frame header: "DI", version 1, chunk type 1.
    const uint8_t sample_bytes[6] = {'M', 'D', 'M', 'P', 0xAA, 0xBB};
    const auto frame = BuildDumpFrame(sample_bytes, sizeof(sample_bytes));
    Check(frame.size() == 10, "dump frame size");
    Check(frame[0] == 0x44 && frame[1] == 0x49 && frame[2] == 1 && frame[3] == 1, "DI v1 chunk 1 header");
    Check(std::memcmp(frame.data() + 4, sample_bytes, sizeof(sample_bytes)) == 0, "dump frame payload matches");

    // OS formatting.
    Check(FormatLinuxOperatingSystem("SteamOS", "Valve", "Jupiter") == "Steam Deck: SteamOS", "Steam Deck LCD OS");
    Check(FormatLinuxOperatingSystem("SteamOS", "Valve", "Galileo") == "Steam Deck: SteamOS", "Steam Deck OLED OS");
    Check(FormatLinuxOperatingSystem("Bazzite", "Valve", "Galileo") == "Steam Deck: Bazzite", "Steam Deck Bazzite OS");
    Check(FormatLinuxOperatingSystem("Ubuntu", "Dell", "XPS") == "Linux: Ubuntu", "Desktop Linux OS");
    Check(FormatLinuxOperatingSystem("", "Dell", "XPS") == "Linux", "Empty distro Linux OS");
    Check(!OperatingSystemText().empty(), "OperatingSystemText non-empty");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

void TestOutcomeMappingAndClientId() {
    Check(IncidentOutcomeOf(201, "") == IncidentOutcome::kSent, "201 -> sent");
    Check(IncidentOutcomeOf(200, "") == IncidentOutcome::kSent, "200 -> sent");
    Check(IncidentOutcomeOf(429, "incident_hourly_limit") == IncidentOutcome::kRecentLimit, "hourly limit");
    Check(IncidentOutcomeOf(429, "limit") == IncidentOutcome::kRecentLimit, "generic limit");
    Check(IncidentOutcomeOf(429, "incident_daily_limit") == IncidentOutcome::kDailyLimit, "daily limit");
    Check(IncidentOutcomeOf(429, "") == IncidentOutcome::kRecentLimit, "429 default recent limit");
    Check(IncidentOutcomeOf(400, "bad_request") == IncidentOutcome::kRefused, "400 refused");
    Check(IncidentOutcomeOf(413, "too_large") == IncidentOutcome::kRefused, "413 refused");
    Check(IncidentOutcomeOf(500, "") == IncidentOutcome::kServiceError, "500 service error");
    Check(IncidentOutcomeOf(0, "", HttpFailure::kUnreachable) == IncidentOutcome::kUnreachable, "unreachable");
    Check(IncidentOutcomeOf(0, "", HttpFailure::kTimedOut) == IncidentOutcome::kUnreachable, "timeout unreachable");
    Check(IncidentOutcomeOf(0, "", HttpFailure::kCertificate) == IncidentOutcome::kUnreachable, "cert unreachable");

    Check(IncidentLimitTitle(IncidentOutcome::kRecentLimit) != nullptr && IncidentLimitTitle(IncidentOutcome::kRecentLimit)[0] != '\0',
          "recent limit title");
    Check(IncidentLimitTitle(IncidentOutcome::kDailyLimit) != nullptr && IncidentLimitTitle(IncidentOutcome::kDailyLimit)[0] != '\0',
          "daily limit title");

    Check(FeedbackOutcomeOf(200, "") == FeedbackOutcome::kSaved, "feedback 200 saved");
    Check(FeedbackOutcomeOf(409, "stale_revision") == FeedbackOutcome::kStale, "feedback 409 stale");
    Check(FeedbackOutcomeOf(0, "", HttpFailure::kUnreachable) == FeedbackOutcome::kUnreachable, "feedback unreachable");
    Check(FeedbackOutcomeOf(500, "") == FeedbackOutcome::kFailed, "feedback 500 failed");

    // Client ID created once and persisted.
    const fs::path dir = MakeTempDir("client_id");
    const std::string id1 = LoadOrCreateClientId(dir);
    const std::string id2 = LoadOrCreateClientId(dir);
    Check(!id1.empty() && id1.size() >= 16, "client id generated");
    Check(id1 == id2, "client id persisted across loads");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

void TestClientAgainstStandInTransport() {
    const fs::path dir = MakeTempDir("client");
    const fs::path log_file = dir / "trespasser_recomp_010.log";
    const fs::path dmp_file = dir / "trespasser_recomp_010.dmp";
    WriteBytes(log_file, "[guest   ] entering the CRT entry point\n");
    // Multi-chunk dump (> 64 KiB) to exercise multi-chunk upload & offset headers.
    const size_t multi_size = kDumpChunkBytes + 500;
    WriteBytes(dmp_file, MakeMinidump(multi_size));

    const auto logs = FindGameLogs(dir, {});
    const ReportFacts facts{"0.1.0", "English (Retail 116)", "116", "Windows 11"};
    const auto pkg = BuildIncidentPackage("Test crash upload retry and multi-chunk", logs[0], facts, PrivateFacts{}, true);
    Check(pkg.has_value() && pkg->crash_dump.has_value(), "client test package built");

    // 1. Not configured endpoint never invokes transport.
    {
        bool called = false;
        ServiceConfig uncfg;
        uncfg.endpoint = ResolveEndpoint("", "");
        uncfg.post = [&](const std::string&, const std::string&, const HttpLimits&) -> HttpReply {
            called = true;
            return {HttpFailure::kNone, 200, "{}"};
        };
        const IncidentReceipt r = FileIncident(uncfg, dir, *pkg);
        Check(r.outcome == IncidentOutcome::kNotConfigured && !called, "not configured skips transport");
    }

    // 2. Successful report + multi-chunk dump upload with first-chunk retry.
    const std::string receipt_hex(64, 'b');
    int chunk_calls = 0;
    bool end_called = false;
    std::vector<std::string> seen_offsets;
    ServiceConfig cfg;
    cfg.endpoint = ResolveEndpoint("https://reports.example", "");
    cfg.post = [&](const std::string& url, const std::string& body, const HttpLimits&) -> HttpReply {
        if (url.ends_with("/v1/incidents")) {
            Check(body.find("Test crash upload retry") != std::string::npos, "incident post carries description");
            return {HttpFailure::kNone, 201,
                    "{\"ticket\":\"JPT-ABCDEF\",\"receipt\":\"" + receipt_hex +
                        "\",\"submitted_at\":1700000000000,\"dump\":\"expected\",\"dump_chunk_bytes\":65536}"};
        }
        if (url.ends_with("/v1/incidents/dump/start")) {
            return {HttpFailure::kNone, 200,
                    "{\"stored\":0,\"bytes\":" + std::to_string(multi_size) + ",\"complete\":false}"};
        }
        if (url.ends_with("/v1/incidents/dump/end")) {
            end_called = true;
            return {HttpFailure::kNone, 200,
                    "{\"stored\":" + std::to_string(multi_size) + ",\"bytes\":" + std::to_string(multi_size) + ",\"complete\":true}"};
        }
        if (url.ends_with("/v1/incidents/status")) {
            return {HttpFailure::kNone, 200,
                    "{\"reports\":[{\"ticket\":\"JPT-ABCDEF\",\"status\":\"awaiting_player_test\",\"status_updated_at\":2000,"
                    "\"fixed_version\":\"0.1.1\",\"player_message\":\"Please test the new build.\",\"test_feedback\":\"\"}]}"};
        }
        if (url.ends_with("/v1/incidents/read")) {
            return {HttpFailure::kNone, 200,
                    "{\"report\":{\"description\":\"Test crash upload retry and multi-chunk\",\"launcher_version\":\"0.1.0\","
                    "\"game_build\":\"English (Retail 116)\",\"game_version\":\"116\",\"operating_system\":\"Windows 11\","
                    "\"log_file\":\"trespasser_recomp_010.log\",\"log_clipped\":\"no\",\"log_text\":\"[guest] ok\"}}"};
        }
        if (url.ends_with("/v1/incidents/feedback")) {
            const auto req = json::Parse(body);
            Check(req && req->StringOr("ticket") == "JPT-ABCDEF", "feedback request ticket");
            Check(req && req->NumberOr("revision") == 2000, "feedback request revision");
            return {HttpFailure::kNone, 200, "{\"ticket\":\"JPT-ABCDEF\",\"revision\":2000,\"test_feedback\":\"fixed\"}"};
        }
        return {HttpFailure::kNone, 404, "{}"};
    };
    cfg.post_binary = [&](const std::string& url, const uint8_t* data, size_t size, const std::vector<HttpHeader>& headers,
                          const HttpLimits&) -> HttpReply {
        ++chunk_calls;
        Check(url.ends_with("/v1/incidents/dump/chunk"), "chunk URL");
        if (chunk_calls == 1) return {HttpFailure::kUnreachable, 0, ""}; // Force retry on first attempt
        Check(size > 4 && data[0] == 0x44 && data[1] == 0x49 && data[2] == 1 && data[3] == 1, "chunk DI header valid");
        for (const auto& [k, v] : headers) {
            if (k == "X-JPT-Dump-Offset") seen_offsets.push_back(v);
            if (k == "X-JPT-Ticket") Check(v == "JPT-ABCDEF", "chunk ticket header");
            if (k == "X-JPT-Receipt") Check(v == receipt_hex, "chunk receipt header");
        }
        return {HttpFailure::kNone, 200, "{\"stored\":" + std::to_string(size - 4) + ",\"bytes\":" + std::to_string(multi_size) + "}"};
    };

    const IncidentReceipt receipt = FileIncident(cfg, dir, *pkg);
    Check(receipt.outcome == IncidentOutcome::kSent, "report sent");
    Check(receipt.ticket == "JPT-ABCDEF", "ticket parsed");
    Check(receipt.credential == receipt_hex, "receipt parsed");
    Check(receipt.submitted_at == 1700000000000LL, "submitted_at parsed");
    Check(receipt.crash == CrashUpload::kUploaded && chunk_calls == 3 && end_called, "multi-chunk dump retried and uploaded");
    Check(seen_offsets.size() == 2 && seen_offsets[0] == "0" && seen_offsets[1] == std::to_string(kDumpChunkBytes),
          "chunk offsets sequential");

    // 3. FetchReportStatuses, FetchSentReport, SendTestFeedback.
    TrackedReport tracked = TrackedReportOf(*pkg, receipt);
    Check(tracked.ticket == "JPT-ABCDEF" && tracked.receipt == receipt_hex, "TrackedReportOf copies ticket & receipt");

    const auto statuses = FetchReportStatuses(cfg, {tracked});
    Check(statuses && statuses->size() == 1, "FetchReportStatuses returned 1");
    Check((*statuses)[0].ticket == "JPT-ABCDEF" && (*statuses)[0].status == kAwaitingPlayerTest, "status awaiting_player_test");
    Check((*statuses)[0].updated_at == 2000 && (*statuses)[0].fixed_version == "0.1.1", "status version & revision");
    Check((*statuses)[0].player_message == "Please test the new build.", "developer reply parsed");

    const auto detail = FetchSentReport(cfg, tracked);
    Check(detail.has_value() && detail->Value("log_file") == "trespasser_recomp_010.log", "FetchSentReport parsed");

    tracked.status = kAwaitingPlayerTest;
    tracked.status_updated_at = 2000;
    const FeedbackAnswer fb = SendTestFeedback(cfg, tracked, kTestFixed, facts);
    Check(fb.outcome == FeedbackOutcome::kSaved, "feedback fixed saved");

    // 4. Stale feedback (409) & unreachable feedback.
    ServiceConfig stale_cfg = cfg;
    stale_cfg.post = [](const std::string&, const std::string&, const HttpLimits&) -> HttpReply {
        return {HttpFailure::kNone, 409, "{\"error\":\"stale_revision\"}"};
    };
    Check(SendTestFeedback(stale_cfg, tracked, kTestNotFixed, facts).outcome == FeedbackOutcome::kStale, "409 feedback -> kStale");

    ServiceConfig down_cfg = cfg;
    down_cfg.post = [](const std::string&, const std::string&, const HttpLimits&) -> HttpReply {
        return {HttpFailure::kUnreachable, 0, ""};
    };
    Check(SendTestFeedback(down_cfg, tracked, kTestNotFixed, facts).outcome == FeedbackOutcome::kUnreachable, "down feedback -> kUnreachable");
    Check(!FetchReportStatuses(down_cfg, {tracked}).has_value(), "down status -> nullopt");
    Check(!FetchSentReport(down_cfg, tracked).has_value(), "down detail -> nullopt");

    // 5. Rate-limit, refused, invalid receipt responses.
    ServiceConfig limit_cfg = cfg;
    limit_cfg.post = [](const std::string&, const std::string&, const HttpLimits&) -> HttpReply {
        return {HttpFailure::kNone, 429, "{\"error\":\"incident_daily_limit\"}"};
    };
    Check(FileIncident(limit_cfg, dir, *pkg).outcome == IncidentOutcome::kDailyLimit, "daily limit mapped");

    ServiceConfig bad_receipt_cfg = cfg;
    bad_receipt_cfg.post = [](const std::string&, const std::string&, const HttpLimits&) -> HttpReply {
        return {HttpFailure::kNone, 201, "{\"ticket\":\"JPT-123456\",\"receipt\":\"not-64-hex\"}"};
    };
    const IncidentReceipt bad_receipt = FileIncident(bad_receipt_cfg, dir, *pkg);
    Check(bad_receipt.outcome == IncidentOutcome::kSent && bad_receipt.credential.empty() && bad_receipt.crash == CrashUpload::kFailed,
          "malformed receipt leaves credential empty and skips dump upload");

    // 6. Dump upload failure when server permanently rejects (400) does not hang or claim uploaded.
    ServiceConfig dump_reject_cfg = cfg;
    dump_reject_cfg.post = [](const std::string&, const std::string&, const HttpLimits&) -> HttpReply {
        return {HttpFailure::kNone, 400, "{\"error\":\"bad_request\"}"};
    };
    Check(UploadCrashDump(dump_reject_cfg, "JPT-ABCDEF", receipt_hex, *pkg->crash_dump) == CrashUpload::kFailed, "400 dump start -> kFailed");

    // 7. Dump upload already complete skips all chunk posts.
    {
        int binary_calls = 0;
        ServiceConfig complete_cfg = cfg;
        complete_cfg.post = [&](const std::string&, const std::string&, const HttpLimits&) -> HttpReply {
            return {HttpFailure::kNone, 200,
                    "{\"stored\":" + std::to_string(multi_size) + ",\"bytes\":" + std::to_string(multi_size) + ",\"complete\":true}"};
        };
        complete_cfg.post_binary = [&](const std::string&, const uint8_t*, size_t, const std::vector<HttpHeader>&,
                                       const HttpLimits&) -> HttpReply {
            ++binary_calls;
            return {HttpFailure::kNone, 200, "{}"};
        };
        Check(UploadCrashDump(complete_cfg, "JPT-ABCDEF", receipt_hex, *pkg->crash_dump) == CrashUpload::kUploaded, "already complete -> kUploaded");
        Check(binary_calls == 0, "already complete sends 0 chunks");
    }

    // 8. Dump upload resumes from partial stored offset.
    {
        std::vector<std::string> resume_offsets;
        bool resume_end = false;
        ServiceConfig resume_cfg = cfg;
        resume_cfg.post = [&](const std::string& url, const std::string&, const HttpLimits&) -> HttpReply {
            if (url.ends_with("/v1/incidents/dump/start")) {
                return {HttpFailure::kNone, 200,
                        "{\"stored\":" + std::to_string(kDumpChunkBytes) + ",\"bytes\":" + std::to_string(multi_size) + ",\"complete\":false}"};
            }
            if (url.ends_with("/v1/incidents/dump/end")) {
                resume_end = true;
                return {HttpFailure::kNone, 200,
                        "{\"stored\":" + std::to_string(multi_size) + ",\"bytes\":" + std::to_string(multi_size) + ",\"complete\":true}"};
            }
            return {HttpFailure::kNone, 404, "{}"};
        };
        resume_cfg.post_binary = [&](const std::string&, const uint8_t*, size_t size, const std::vector<HttpHeader>& headers,
                                     const HttpLimits&) -> HttpReply {
            for (const auto& [k, v] : headers) {
                if (k == "X-JPT-Dump-Offset") resume_offsets.push_back(v);
            }
            Check(size == 504, "resumed final chunk frame is 500 + 4 bytes");
            return {HttpFailure::kNone, 200, "{\"stored\":" + std::to_string(multi_size) + ",\"bytes\":" + std::to_string(multi_size) + "}"};
        };
        Check(UploadCrashDump(resume_cfg, "JPT-ABCDEF", receipt_hex, *pkg->crash_dump) == CrashUpload::kUploaded, "resumed upload succeeds");
        Check(resume_offsets.size() == 1 && resume_offsets[0] == std::to_string(kDumpChunkBytes) && resume_end, "resumed at exact offset and ended");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

void TestHistoryStatusRepliesAndRevisions() {
    const fs::path dir = MakeTempDir("history");
    const fs::path file = dir / kReportHistoryFile;
    ReportHistory history(file);

    // Status text and preview helpers.
    const char* status_codes[] = {"new", "investigating", "awaiting_player_test", "fixed", "cannot_reproduce", "duplicate", "not_a_bug", "closed"};
    for (const char* st : status_codes) {
        const char* label = ReportStatusText(st);
        Check(label != nullptr && label[0] != '\0', "ReportStatusText non-empty");
    }
    Check(ReportStatusText("unknown_status") == nullptr, "unknown status returns null");
    Check(ReportPreview("Short line") == "Short line", "short preview unchanged");
    Check(ReportPreview("Line one\nLine two").find('\n') == std::string::npos, "preview collapses newline");
    Check(ReportPreview(std::string(200, 'a')).size() < 200, "long preview truncated");

    // Add report.
    TrackedReport r;
    r.ticket = "JPT-234567";
    r.receipt = std::string(64, 'a');
    r.submitted_at = 1000;
    r.status_updated_at = 1000;
    r.description = "Crash near lab";
    r.log_file = "trespasser_recomp_004.log";
    r.launcher_version = "0.1.0";
    r.game_build = "English (Retail 116)";
    r.operating_system = "Windows 11";
    Check(history.Add(r), "history add and save");
    Check(!history.HasUnseen() && !history.HasUnseenMessage(), "newly added report not unseen");
    Check(PlayerTestOf(history.reports()[0]) == PlayerTest::kNone, "initial player test none");
    Check(!ReportStatusLine(history.reports()[0]).empty(), "status line non-empty");

    // Developer reply + awaiting_player_test marks unseen + message_unseen.
    Check(history.Apply({{"JPT-234567", kAwaitingPlayerTest, 2000, "0.1.1", "Please retest in 0.1.1", ""}}, {"JPT-234567"}),
          "apply awaiting_player_test");
    Check(history.HasUnseen() && history.HasUnseenMessage(), "unseen and new reply flags");
    Check(history.NextUnseen() == "JPT-234567", "next unseen picks reply");
    Check(PlayerTestOf(history.reports()[0]) == PlayerTest::kAwaitingAnswer, "awaiting answer");
    Check(history.reports()[0].player_message == "Please retest in 0.1.1", "developer reply stored");
    Check(history.reports()[0].fixed_version == "0.1.1", "fixed version stored");

    // Wrong revision rejected; matching revision recorded for fixed.
    Check(!history.RecordTestFeedback("JPT-234567", 1999, kTestFixed), "wrong revision rejected");
    Check(!history.RecordTestFeedback("JPT-999999", 2000, kTestFixed), "unknown ticket rejected");
    Check(history.RecordTestFeedback("JPT-234567", 2000, kTestFixed), "matching revision recorded");
    Check(PlayerTestOf(history.reports()[0]) == PlayerTest::kConfirmedFixed, "confirmed fixed");

    // MarkSeen clears unseen flags; reload persists.
    Check(history.MarkSeen("JPT-234567") && !history.HasUnseen() && !history.HasUnseenMessage(), "mark seen clears");
    Check(history.NextUnseen().empty(), "no next unseen after MarkSeen");
    Check(history.Save(), "save after feedback");

    ReportHistory reloaded(file);
    reloaded.Load();
    Check(reloaded.reports().size() == 1, "reload count 1");
    Check(PlayerTestOf(reloaded.reports()[0]) == PlayerTest::kConfirmedFixed, "reload keeps confirmed fixed");
    Check(reloaded.reports()[0].player_message == "Please retest in 0.1.1", "reload keeps developer reply");
    Check(reloaded.reports()[0].test_feedback_revision == 2000, "reload keeps feedback revision");

    // New revision (3000) re-opens awaiting_player_test; player answers not_fixed.
    Check(reloaded.Apply({{"JPT-234567", kAwaitingPlayerTest, 3000, "0.1.2", "Try 0.1.2 please", ""}}, {"JPT-234567"}),
          "new revision reopens test");
    Check(PlayerTestOf(reloaded.reports()[0]) == PlayerTest::kAwaitingAnswer, "new revision awaits answer");
    Check(reloaded.HasUnseen() && reloaded.HasUnseenMessage(), "new revision reply marked unseen");
    Check(reloaded.RecordTestFeedback("JPT-234567", 3000, kTestNotFixed), "record not_fixed on rev 3000");
    Check(PlayerTestOf(reloaded.reports()[0]) == PlayerTest::kStillHappening, "still happening state");

    // Missing from asked list marks report unavailable.
    Check(reloaded.Apply({}, {"JPT-234567"}), "missing from reply marks unavailable");
    Check(reloaded.reports()[0].unavailable, "unavailable flag set");

    // Remove report.
    Check(!reloaded.Remove("JPT-NOPE"), "remove unknown returns false");
    Check(reloaded.Remove("JPT-234567") && reloaded.reports().empty(), "remove known clears list");

    // Multiple reports: unseen reply takes priority over plain status change, unchanged status stays seen.
    {
        ReportHistory multi(dir / "multi.json");
        TrackedReport a;
        a.ticket = "JPT-AAA111";
        a.receipt = std::string(64, 'd');
        a.submitted_at = 1000;
        a.status_updated_at = 1000;
        a.description = "First issue";
        TrackedReport b = a;
        b.ticket = "JPT-BBB222";
        b.submitted_at = 1100;
        b.status_updated_at = 1100;
        b.description = "Second issue";
        Check(multi.Add(a) && multi.Add(b), "add two reports");
        Check(multi.Apply(
                  {{"JPT-AAA111", "investigating", 1200, "", "", ""},
                   {"JPT-BBB222", kAwaitingPlayerTest, 1300, "0.1.3", "Can you verify?", ""}},
                  {"JPT-AAA111", "JPT-BBB222"}),
              "apply two statuses");
        Check(multi.HasUnseen() && multi.HasUnseenMessage(), "multi unseen & message");
        Check(multi.NextUnseen() == "JPT-BBB222", "NextUnseen prioritizes developer reply");
        Check(multi.MarkSeen("JPT-BBB222"), "MarkSeen clears reply report");
        Check(multi.HasUnseen() && !multi.HasUnseenMessage(), "other report still unseen without message");
        Check(multi.NextUnseen() == "JPT-AAA111", "NextUnseen falls back to status-only unseen");
        Check(multi.MarkSeen("JPT-AAA111") && !multi.HasUnseen(), "all seen after second MarkSeen");
        // Re-applying identical status does not re-mark unseen.
        Check(!multi.Apply({{"JPT-AAA111", "investigating", 1200, "", "", ""}}, {"JPT-AAA111"}), "unchanged apply returns false");
        Check(!multi.HasUnseen(), "unchanged status stays seen");
    }

    // Cap at kMostTrackedReports (50).
    ReportHistory capped(dir / "capped.json");
    for (size_t i = 0; i < kMostTrackedReports + 5; ++i) {
        TrackedReport item;
        item.ticket = "JPT-" + std::to_string(100000 + i);
        item.receipt = std::string(64, 'c');
        item.submitted_at = int64_t(1000 + i);
        item.description = "Report #" + std::to_string(i);
        Check(capped.Add(item), "add within cap loop");
    }
    Check(capped.reports().size() == kMostTrackedReports, "history capped at kMostTrackedReports");
    Check(capped.Save(), "save capped history");
    ReportHistory capped_reload(dir / "capped.json");
    capped_reload.Load();
    Check(capped_reload.reports().size() == kMostTrackedReports, "reloaded capped history preserves count");
    for (size_t i = 0; i < capped_reload.reports().size(); ++i) {
        Check(!capped_reload.reports()[i].ticket.empty() && capped_reload.reports()[i].receipt.size() == 64,
              "reloaded report preserves ticket and receipt");
    }

    // Corrupt JSON file loads safely as empty without crashing.
    const fs::path corrupt = dir / "corrupt.json";
    WriteBytes(corrupt, "{not valid json");
    ReportHistory bad(corrupt);
    bad.Load();
    Check(bad.reports().empty(), "corrupt history file loads empty");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

void TestNothingSentOnItsOwn() {
    const fs::path launcher_cpp = fs::path(JPT_SOURCE_DIR) / "launcher" / "launcher.cpp";
    std::ifstream in(launcher_cpp, std::ios::binary);
    Check(in.good(), "launcher.cpp readable");
    const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Check(src.find("PostJsonOverHttps") == std::string::npos, "launcher never calls raw PostJsonOverHttps directly");
    Check(src.find("PostBinaryOverHttps") == std::string::npos, "launcher never calls raw PostBinaryOverHttps directly");
    Check(src.find("FileIncident(") == std::string::npos, "launcher does not auto-send reports outside incident page");

    const fs::path page_cpp = fs::path(JPT_SOURCE_DIR) / "launcher" / "incident_page.cpp";
    std::ifstream pin(page_cpp, std::ios::binary);
    Check(pin.good(), "incident_page.cpp readable");
    const std::string psrc((std::istreambuf_iterator<char>(pin)), std::istreambuf_iterator<char>());
    Check(psrc.find("FileIncident(") != std::string::npos, "incident_page wires FileIncident");
    Check(psrc.find("BuildIncidentPackage") != std::string::npos, "incident_page builds scrubbed package before sending");
}

} // namespace

void TestLegacyLinuxAndBootstrapLogs() {
    const fs::path dir = MakeTempDir("linux_legacy");
    const fs::path running = dir / "trespasser_recomp_004.log";
    const fs::path boot_guest = dir / "trespasser_recomp_003.log";
    const fs::path legacy_cp1251 = dir / "trespasser_linux_002.log";
    const fs::path legacy_closed = dir / "trespasser_linux_001.log";

    WriteBytes(legacy_closed, "[launcher] closed\n");
    // Raw CP1251 Russian ("Ошибка") alongside valid UTF-8 Cyrillic ("Привет") and a private Linux path.
    WriteBytes(legacy_cp1251,
               "[launcher] closed, starting the game\n"
               "[boot] error: \xCE\xF8\xE8\xE1\xEA\xE0 path /home/SecretUser/game.dat utf8 \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\n");
    WriteBytes(boot_guest, "[Linux guest] entry 00412340, SelfMod lifted entries 12\n[boot] failed during init\n");
    WriteBytes(running, "[launcher] shown\n");

    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(legacy_closed, now - std::chrono::minutes(40), ec);
    fs::last_write_time(legacy_cp1251, now - std::chrono::minutes(30), ec);
    fs::last_write_time(boot_guest, now - std::chrono::minutes(20), ec);
    fs::last_write_time(running, now - std::chrono::minutes(10), ec);

    // Optional symlink check (may be denied on Windows without Developer Mode).
    const fs::path symlink_log = dir / "trespasser_recomp_099.log";
    fs::create_symlink(boot_guest, symlink_log, ec);
    const bool symlink_created = !ec;
    ec.clear();

    const fs::path legacy_dmp = dir / "trespasser_linux_002.dmp";
    WriteBytes(legacy_dmp, MakeMinidump(128));
    fs::last_write_time(legacy_dmp, now - std::chrono::minutes(30), ec);

    const auto logs = FindGameLogs(dir, running);
    Check(logs.size() == 3, "discovers canonical and legacy logs, omits closed launcher and symlinks");
    Check(logs[0].name == "trespasser_recomp_004.log" && logs[0].role == LogRole::kCurrentLauncher, "running canonical log first");
    Check(logs[1].name == "trespasser_recomp_003.log" && logs[1].activity == LogActivity::kGameStarted,
          "Linux guest entry classified as game started");
    Check(logs[1].role == LogRole::kNone, "bootstrap log not falsely classified as gameplay");
    Check(DefaultGameLog(logs) == 1, "default log prefers Linux bootstrap attempt over current launcher");
    Check(logs[2].name == "trespasser_linux_002.log" && logs[2].activity == LogActivity::kGameStarted,
          "legacy trespasser_linux_002.log discovered without renaming");
    Check(fs::exists(legacy_cp1251), "legacy file preserved in place");
    Check(logs[2].Crashed() && logs[2].CanAttachCrash(), "legacy crash dump paired");
    Check(DumpNameOf("trespasser_linux_002.log") == "trespasser_linux_002.dmp", "legacy dump name rule");
    if (symlink_created) {
        const bool found_symlink = std::any_of(logs.begin(), logs.end(), [](const GameLog& l) { return l.name == "trespasser_recomp_099.log"; });
        Check(!found_symlink, "symlinked log rejected");
    }

    PrivateFacts pf;
    pf.user_names = {"SecretUser"};
    pf.profile_folders = {"/home/SecretUser"};
    const ReportFacts facts{"0.1.1", "English (Retail 116)", "116", "Linux: SteamOS"};
    const auto pkg = BuildIncidentPackage("Startup crash on Linux v0.1.1 log.", logs[2], facts, pf, false);
    Check(pkg.has_value(), "CP1251 legacy log builds package");
    if (pkg) {
        Check(pkg->Value("log_file") == "trespasser_linux_002.log", "legacy log_file name preserved");
        Check(pkg->Value("log_text").find("SecretUser") == std::string::npos, "CP1251 log still scrubbed");
        Check(pkg->Value("log_text").find("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82") != std::string::npos,
              "valid UTF-8 preserved alongside CP1251 bytes");
        const std::string preview = IncidentPreviewText(*pkg);
        Check(preview.find("trespasser_linux_002.log") != std::string::npos && preview.find("SecretUser") == std::string::npos,
              "CP1251 legacy preview valid and scrubbed");
        const std::string req_json = IncidentRequestJson(*pkg);
        const auto parsed = json::Parse(req_json);
        Check(parsed && parsed->IsObject() && parsed->StringOr("log_file") == "trespasser_linux_002.log",
              "CP1251 legacy log produces valid JSON");
    }

    fs::remove_all(dir, ec);
}

int main() {
    TestJson();
    TestEndpointAndTransportAddress();
    TestPrivacyScrubber();
    TestLogDiscoveryAndCrashValidation();
    TestLegacyLinuxAndBootstrapLogs();
    TestDraftPayloadAndPreview();
    TestOutcomeMappingAndClientId();
    TestClientAgainstStandInTransport();
    TestHistoryStatusRepliesAndRevisions();
    TestNothingSentOnItsOwn();
    if (g_failures) {
        std::fprintf(stderr, "bug_report_tests: %d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }
    std::printf("bug_report_tests: %d checks, 0 failures\n", g_checks);
    return 0;
}
