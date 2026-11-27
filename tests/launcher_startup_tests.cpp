// The launcher's start on scripted systems: renderer choice and fallback, --launcher-renderer, the graphics card list and
// what a failed launcher may do. Windows and Linux; no SDL, device, window or game.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "graphics_cards.h"
#include "jpt_config.h"
#include "jpt_renderer.h"
#include "launcher_renderer.h"

namespace fs = std::filesystem;
using namespace jpt;

static int g_checks, g_failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        g_checks++;                                                     \
        if (!(cond)) {                                                  \
            g_failures++;                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

using Names = std::vector<std::string_view>;
using Events = std::vector<std::string>;

static bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

static std::string Source(const char* relative) {
    std::ifstream in(fs::path(JPT_SOURCE_DIR) / relative, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    std::string result = text.str();
    result.erase(std::remove(result.begin(), result.end(), '\r'), result.end());
    return result;
}

static std::string Between(const std::string& text, const char* from, const char* to) {
    const size_t start = text.find(from);
    if (start == std::string::npos) return {};
    const size_t end = text.find(to, start + std::strlen(from));
    return text.substr(start, end == std::string::npos ? end : end - start);
}

// ---- renderer

struct ScriptedSystem {
    std::vector<std::string> failing;
    RendererStage stage = RendererStage::kCreate;
    std::string automatic = "opengl"; // what SDL opens for the empty name
    Events events;
    std::string open;

    RendererSelection Select(std::string_view requested, bool windows) {
        return SelectLauncherRenderer(
            requested, windows,
            [this](std::string_view name) -> std::optional<RendererFailure> {
                CHECK(open.empty()); // nothing of an earlier attempt is left
                events.push_back("open:" + std::string(name));
                open = name.empty() ? automatic : std::string(name);
                for (const std::string& bad : failing) {
                    if (bad == name) return RendererFailure{stage, "scripted " + AttemptLabel(name)};
                }
                return std::nullopt;
            },
            [this] {
                events.push_back("discard");
                open.clear();
            },
            [this] { return open; });
    }
};

static void TestValues() {
    for (const char* both : {"auto", "software"}) CHECK(jpt_launcher_renderer_valid(both, 1) && jpt_launcher_renderer_valid(both, 0));
    CHECK(jpt_launcher_renderer_valid("direct3d11", 1) && !jpt_launcher_renderer_valid("direct3d11", 0));
    for (const char* bad : {"", "Software", "d3d11", "vulkan", "opengl", "software ", "--launcher"})
        CHECK(!jpt_launcher_renderer_valid(bad, 1) && !jpt_launcher_renderer_valid(bad, 0));
    CHECK(!jpt_launcher_renderer_valid(nullptr, 1));
}

static void TestOrder() {
    CHECK(LauncherRendererOrder(true) == Names({"direct3d11", "software"}) && LauncherRendererOrder(false) == Names({"", "software"}));
    CHECK(LauncherRendererOrder(true, "auto") == LauncherRendererOrder(true) && LauncherRendererOrder(false, "auto") == LauncherRendererOrder(false));
    CHECK(LauncherRendererOrder(true, "software") == Names({"software"}) && LauncherRendererOrder(false, "software") == Names({"software"}));
    CHECK(LauncherRendererOrder(true, "direct3d11") == Names({"direct3d11"}));
    for (const bool windows : {true, false})
        for (const char* request : {"auto", "software", "direct3d11", "vulkan", ""})
            for (const std::string_view name : LauncherRendererOrder(windows, request)) CHECK(name != "vulkan");
#ifdef _WIN32
    CHECK(RendererModulesToPin("software").empty() && RendererModulesToPin("direct3d11") == Names({"d3d11.dll", "dxgi.dll"}));
#else
    CHECK(RendererModulesToPin("software").empty() && RendererModulesToPin("direct3d11").empty() && RendererModulesToPin("").empty());
#endif
}

static void TestSelection() {
    {
        ScriptedSystem system;
        const RendererSelection s = system.Select("auto", true);
        CHECK(s.opened && s.effective == "direct3d11" && !s.fallback && system.events == Events({"open:direct3d11"}));
        CHECK(RendererResultLine(s) == "startup: renderer effective=direct3d11 fallback=0");
        CHECK(RendererAttemptLine(s.attempts[0]) == "startup: renderer attempt=direct3d11 result=ok");
    }
    for (const RendererStage stage : {RendererStage::kPin, RendererStage::kWindow, RendererStage::kCreate, RendererStage::kFirstFrame,
                                      RendererStage::kBackend}) {
        ScriptedSystem system;
        system.failing = {"direct3d11"};
        system.stage = stage;
        const RendererSelection s = system.Select("auto", true);
        CHECK(s.opened && s.effective == "software" && s.fallback);
        CHECK(system.events == Events({"open:direct3d11", "discard", "open:software"}));
        CHECK(RendererResultLine(s) == std::string("startup: renderer effective=software fallback=1 fallback_reason=direct3d11_") + RendererStageName(stage));
        CHECK(Has(RendererAttemptLine(s.attempts[0]), "result=failed stage=") && Has(RendererAttemptLine(s.attempts[0]), "reason=scripted direct3d11"));
    }
    for (const bool windows : {true, false}) {
        ScriptedSystem system;
        const RendererSelection s = system.Select("software", windows);
        CHECK(s.opened && s.effective == "software" && !s.fallback && system.events == Events({"open:software"}));
    }
    {
        ScriptedSystem system;
        system.failing = {"direct3d11"};
        const RendererSelection s = system.Select("direct3d11", true);
        CHECK(!s.opened && s.attempts.size() == 1 && system.events == Events({"open:direct3d11", "discard"}));
    }
    {
        ScriptedSystem system;
        const RendererSelection s = system.Select("auto", false);
        CHECK(s.opened && s.effective == "opengl" && !s.fallback && system.events == Events({"open:"}));
    }
    {
        ScriptedSystem system;
        system.failing = {""};
        const RendererSelection s = system.Select("auto", false);
        CHECK(s.opened && s.effective == "software" && s.fallback && Has(RendererResultLine(s), "fallback_reason=accelerated_create"));
    }
    {
        // SDL itself can end on its software renderer.
        ScriptedSystem system;
        system.automatic = "software";
        const RendererSelection s = system.Select("auto", false);
        CHECK(s.opened && s.effective == "software" && s.fallback);
        CHECK(RendererResultLine(s) == "startup: renderer effective=software fallback=1");
    }
    for (const bool windows : {true, false}) {
        ScriptedSystem system;
        system.failing = {"direct3d11", "", "software"};
        const RendererSelection s = system.Select("auto", windows);
        CHECK(!s.opened && s.effective.empty() && s.attempts.size() == 2 && system.open.empty());
        CHECK(RendererResultLine(s) == "startup: renderer effective=none");
        CHECK(Has(RendererFailureText(s), "software: scripted software"));
    }
    CHECK(RendererRequestLine("auto", true) == "startup: renderer requested=auto order=direct3d11,software");
    CHECK(RendererRequestLine("software", true) == "startup: renderer requested=software order=software");
    CHECK(RendererRequestLine("auto", false) == "startup: renderer requested=auto order=accelerated,software");
}

// ---- graphics cards

struct ScriptedCards {
    std::vector<std::string> names;
    std::string detail;
    unsigned msaa = 4u | 16u; // sample counts as bits, as jpt_gpu reports them: 4 = 2x, 16 = 4x, 256 = 8x
    int lists = 0, samples = 0;

    GraphicsCards Detect(bool skip) {
        return DetectGraphicsCards(skip, {[this](std::string& why) {
                                              ++lists;
                                              why = detail;
                                              std::vector<jpt_gpu_t> gpus(names.size());
                                              for (size_t i = 0; i < names.size(); ++i)
                                                  std::snprintf(gpus[i].name, sizeof gpus[i].name, "%s", names[i].c_str());
                                              jpt_gpu_assign_labels(gpus.data(), static_cast<int>(gpus.size()));
                                              return gpus;
                                          },
                                          [this](int) {
                                              ++samples;
                                              return msaa;
                                          }});
    }
};

static void TestGraphicsCards() {
    const std::string saved = "NVIDIA GeForce RTX 4070";
    {
        ScriptedCards system;
        system.names = {"Intel", "", saved};
        const GraphicsCards cards = system.Detect(false);
        CHECK(system.lists == 1 && system.samples == 2 && cards.names == std::vector<std::string>({"Intel", saved}));
        CHECK(std::string(GraphicsCardsResult(cards)) == "ok" && !*GraphicsCardsProblem(cards));
        CHECK(ChosenCard(cards, saved) == 2 && !CardToStore(cards, saved));
        CHECK(CardToStore(cards, "gone") == std::optional<std::string>("Intel"));
        CHECK(AntiAliasingLevels(cards, saved, "4").levels == std::vector<unsigned>({2, 4}) && !AntiAliasingLevels(cards, saved, "4").unsupported);
        CHECK(AntiAliasingLevels(cards, saved, "8").unsupported && !AntiAliasingLevels(cards, saved, "1").unsupported);
    }
    {
        ScriptedCards system;
        system.names = {saved};
        const GraphicsCards cards = system.Detect(true);
        CHECK(system.lists == 0 && system.samples == 0 && cards.state == GraphicsCards::State::kSkipped);
        CHECK(std::string(GraphicsCardsResult(cards)) == "skipped" && Has(GraphicsCardsProblem(cards), "software recovery"));
        CHECK(!CardToStore(cards, saved) && !CardToStore(cards, ""));
    }
    for (const std::vector<std::string>& none : {std::vector<std::string>{}, std::vector<std::string>{"", ""}}) {
        ScriptedCards system;
        system.names = none;
        system.detail = "loader: not found";
        const GraphicsCards cards = system.Detect(false);
        CHECK(std::string(GraphicsCardsResult(cards)) == "unavailable" && cards.detail == "loader: not found");
        CHECK(Has(GraphicsCardsProblem(cards), "unavailable") && !CardToStore(cards, saved));
        for (const char* level : {"0", "2", "4", "8"}) {
            const SampleLevels levels = AntiAliasingLevels(cards, saved, level);
            CHECK(!levels.unsupported && levels.levels.size() == (std::strcmp(level, "0") ? 1u : 0u));
        }
    }
    CHECK(std::string(GraphicsCardsResult(GraphicsCards{})) == "not_asked");
    struct Expected {
        unsigned msaa;
        std::vector<unsigned> levels;
    };
    for (const Expected& card : {Expected{4u, {2}}, Expected{4u | 16u, {2, 4}}, Expected{0u, {}}, Expected{4u | 16u | 256u, {2, 4, 8}},
                                 Expected{2u | 16u | 512u | 65536u, {4}}}) {
        ScriptedCards system;
        system.names = {saved};
        system.msaa = card.msaa;
        const GraphicsCards cards = system.Detect(false);
        const SampleLevels levels = AntiAliasingLevels(cards, saved, "0");
        CHECK(levels.levels == card.levels && AntiAliasingLevels(cards, saved, "0").levels == levels.levels && !levels.unsupported);
        for (const unsigned level : {2u, 4u, 8u}) {
            const bool supported = std::find(card.levels.begin(), card.levels.end(), level) != card.levels.end();
            CHECK(AntiAliasingLevels(cards, saved, std::to_string(level)).unsupported == !supported);
        }
    }
}

// ---- wiring

static void TestSettingsUntouched() {
    // The launcher's renderer is no setting, and the game's renderer values are the ones they were.
    CHECK(!jpt_config_valid("launcher_renderer", "software") && !jpt_config_valid(JPT_KEY_RENDERER, "direct3d11"));
    for (const char* value : {"auto", "d3d11", "vulkan", "software"}) CHECK(jpt_config_valid(JPT_KEY_RENDERER, value));
    const std::string launcher = Source("launcher/launcher.cpp");
    const std::string detect = Between(launcher, "void DetectGraphics()", "void GraphicsCardCombo()");
    CHECK(Has(detect, "SoftwareRecovery(config_)") && Has(detect, "CardToStore(") && !Has(detect, "JPT_KEY_RENDERER"));
    const std::string frame = Between(launcher, "    Outcome Frame() {", "    // Lists the installed versions again");
    CHECK(!frame.empty() && !Has(frame, "ListGpus") && !Has(frame, "DetectGraphics"));
    for (const char* file : {"src/host/main.c", "src/linux/linux_main.c"}) {
        const std::string host = Source(file);
        CHECK(Has(host, "jpt_launcher_renderer_valid") && Has(host, "launcher_renderer = o") && !Has(host, "add_override(o, \"launcher_renderer"));
    }
}

static void TestFailureDoesNotStartTheGame() {
    const std::string launcher = Source("launcher/launcher.cpp");
    const std::string report = Between(launcher, "jpt_launcher_result_t ReportLauncherFailure(", "extern \"C\"");
    CHECK(Has(report, "ShowNativeError") && Has(report, "return JPT_LAUNCHER_FAILED") && !Has(report, "JPT_LAUNCHER_PLAY"));
    const std::string run = Between(launcher, "extern \"C\" jpt_launcher_result_t jpt_launcher_run", "\n}\n");
    CHECK(Has(run, "if (!app.Run(outcome)) return ReportLauncherFailure(") && !Has(run, "starting the game"));
    const size_t skip = run.find("return JPT_LAUNCHER_PLAY;"), open = run.find("app.Run(outcome)");
    CHECK(run.find("LauncherWanted(") < skip && skip < open); // --skip-launcher and Show this launcher Off
    CHECK(Has(run.substr(open), "outcome == Outcome::kPlay ? JPT_LAUNCHER_PLAY : JPT_LAUNCHER_QUIT"));

    const std::string windows = Between(Source("src/host/main.c"), "switch (jpt_launcher_run(&launcher))", "jpt_config_load(");
    CHECK(Has(windows, "case JPT_LAUNCHER_PLAY: break;"));
    CHECK(Has(Between(windows, "case JPT_LAUNCHER_FAILED:", "default:"), "return HOST_EXIT_SETUP;"));
    const std::string linux_play = Between(Source("src/linux/linux_main.c"), "static int play(", "choose_version(");
    CHECK(Has(linux_play, "launched != JPT_LAUNCHER_PLAY") && Has(linux_play, "launched == JPT_LAUNCHER_FAILED ? EXIT_SETUP : 0"));
    CHECK(Has(linux_play, "o->run_direct ? JPT_LAUNCHER_PLAY"));
}

static void TestHostRecovery() {
    const std::string host = Source("launcher/host.cpp");
    const std::string open = Between(host, "LauncherHost::OpenRenderer(", "bool LauncherHost::Open(");
    const size_t pin = open.find("PinRendererModules"), window = open.find("OpenWindow("), create = open.find("SDL_CreateRenderer"),
                 probe = open.find("ProbeFirstFrame"), vsync = open.find("ConfigureVsync"), backend = open.find("ImGui_ImplSDL3_InitForSDLRenderer");
    CHECK(pin < window && window < create && create < probe && probe < vsync && vsync < backend && backend != std::string::npos);
    CHECK(Has(open, "StartVideo(name == kLauncherRendererSoftware)"));

    const std::string present = Between(host, "std::string LauncherHost::PresentChecked()", "\n}\n");
    const size_t flush = present.find("SDL_FlushRenderer"), clear = present.find("SDL_ClearError"), shown = present.find("SDL_RenderPresent");
    CHECK(flush < clear && clear < shown && shown < present.find("SDL_GetError", shown) && present.find("SDL_GetError", shown) != std::string::npos);

    const std::string release = Between(host, "void LauncherHost::ReleaseWindow()", "\n}\n");
    const size_t textures = release.find("SDL_DestroyTexture"), renderer_backend = release.find("if (renderer_backend_)"),
                 sdl_backend = release.find("if (sdl_backend_)"), context = release.find("ImGui::DestroyContext"),
                 renderer = release.find("SDL_DestroyRenderer"), closed = release.find("SDL_DestroyWindow");
    CHECK(textures < renderer_backend && renderer_backend < sdl_backend && sdl_backend < context && context < renderer && renderer < closed &&
          closed != std::string::npos);

    const std::string launcher = Source("launcher/launcher.cpp");
    const std::string retry = Between(launcher, "bool RetryWithSoftware(", "    bool Ready() const");
    const size_t reset = retry.find("launcher_.reset()"), icons = retry.find("page_icons_.clear()"), close = retry.find("host_.Close()"),
                 again = retry.find("host_.Open(options)");
    CHECK(reset < close && icons < close && close < again && again != std::string::npos);
    CHECK(Has(retry, "options.renderer == kLauncherRendererAuto") && Has(retry, "kLauncherRendererSoftware"));
    for (const std::string& text : {host, launcher, Source("launcher/graphics_cards.h"), Source("launcher/launcher_renderer.h")})
        CHECK(!Has(text, (std::string("DEBUG_") + "SAULO").c_str()));
}

int main() {
    TestValues();
    TestOrder();
    TestSelection();
    TestGraphicsCards();
    TestSettingsUntouched();
    TestFailureDoesNotStartTheGame();
    TestHostRecovery();
    std::printf("launcher_startup_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
