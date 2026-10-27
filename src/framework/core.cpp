// The core launcher extension: settings, bare-launch defaults, widescreen and
// presentation. Linked into every port executable (cmake/Port.cmake).
//
// Flags it adds (`--help` lists them under "rez"):
//   --widescreen / --no-widescreen   override the saved setting for this run
//   --fit letterbox|crop|stretch     how the picture fills the window
//   --set KEY=VALUE                  override any setting for this run
//   --save-settings                  write this run's overrides back to the settings file
//   --settings FILE                  use FILE instead of the per-user settings file
// Settings it turns into engine flags: texture_pack, dump_textures, rumble (--rumble), mods
// (--mod), aspect, fullscreen, scale.
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "dream/render/display_list.h"
#include "dream/runtime/host_ext.h"
#include "dream/runtime/system.h"
#include "dreamcomp/port.h"
#include "dreamcomp/hud.h"
#include "dreamcomp/sharp2d.h"
#include "dreamcomp/settings.h"


#ifdef DREAMCOMP_NEXTUI
#include "dream/runtime/pvr/core.h"
#include "nextui/disc_flow.h"
#endif

#if __has_include("dream/render/vk/present.h") && defined(DREAM_WITH_RENDERER)
#include "dream/render/vk/present.h"
#define DREAMCOMP_HAS_PRESENTER 1
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace dreamcomp {

namespace {

const PortInfo* g_port = nullptr;
Settings g_settings;
// The highest render scale and the default; a settings file asking for more is held to it.
#if defined(DREAMCOMP_NEXTUI)
// A NextUI handheld (the Brick: a 1024x768 panel, a PowerVR GE8300): the Dreamcast's own 640x480
// by default, or the panel's own ("panel").
constexpr int kMaxScale = 2;
constexpr int kDefaultScale = 1;
#else
constexpr int kMaxScale = 8;
constexpr int kDefaultScale = 2;
#endif
static_assert(kScalePanel == static_cast<int>(dream::host::HostControls::kScalePanel));

// The setting `scale`: a multiple of 640x480 held to this build's range, or "panel", the display's
// own pixels one to one.
int scale_setting() {
    if (g_settings.get("scale") == "panel")
        return kScalePanel;
    const int scale = std::clamp(g_settings.get_int("scale", kDefaultScale), 1, kMaxScale);
#ifdef DREAMCOMP_NEXTUI
    // A handheld's choices are 640x480 and the panel's own; a saved 2 reads as the panel's
    // (--scale 2 still draws 1280x960).
    if (scale > 1)
        return kScalePanel;
#endif
    return scale;
}

std::string scale_arg(int scale) {
    return scale == kScalePanel ? std::string("panel") : std::to_string(scale);
}

std::filesystem::path exe_dir(const std::string& argv0) {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        return std::filesystem::path(buf).parent_path();
#endif
    std::error_code ec;
    // Normalised: started as ./name, paths built on the folder would hold a "/." otherwise.
    auto p = std::filesystem::absolute(argv0, ec).lexically_normal();
    return ec ? std::filesystem::current_path() : p.parent_path();
}

bool has_flag(const std::vector<std::string>& a, const char* f) {
    return std::find(a.begin(), a.end(), f) != a.end();
}

std::string flag_value(const std::vector<std::string>& a, const char* f) {
    for (std::size_t i = 1; i + 1 < a.size(); ++i)
        if (a[i] == f)
            return a[i + 1];
    return {};
}

#ifdef DREAMCOMP_NEXTUI
// Removes every `f` (and its value when `takes_value`) so the engine never sees the flag.
void drop_flag(std::vector<std::string>& a, const char* f, bool takes_value) {
    for (std::size_t i = 1; i < a.size();) {
        if (a[i] == f)
            a.erase(a.begin() + static_cast<std::ptrdiff_t>(i),
                    a.begin() + static_cast<std::ptrdiff_t>(std::min(a.size(), i + (takes_value ? 2 : 1))));
        else
            ++i;
    }
}
#endif

// `rumble` (0-100, default 100) -> --rumble N; `volume` (0-100) -> --volume N; `cpu_speed` (1-16,
// default the port's) -> --cpu-speed N; `mods = a,b` -> --mod <config dir>/mods/a ...
// (first wins). A flag the user passed wins over the setting.
void add_input_and_mod_flags(std::vector<std::string>& args,
                             const std::filesystem::path& config_dir) {
    if (!has_flag(args, "--rumble") && !g_settings.get("rumble").empty()) {
        args.push_back("--rumble");
        args.push_back(std::to_string(std::clamp(g_settings.get_int("rumble", 100), 0, 100)));
    }
    if (!has_flag(args, "--volume") && !g_settings.get("volume").empty()) {
        args.push_back("--volume");
        args.push_back(std::to_string(std::clamp(g_settings.get_int("volume", 100), 0, 100)));
    }
    if (!has_flag(args, "--soft-clip") && g_settings.get("clip", "hard") == "soft")
        args.push_back("--soft-clip");
    if (!has_flag(args, "--cpu-speed")) {
        const int dflt = g_port ? static_cast<int>(g_port->cpu_speed) : 1;
        const int speed = std::clamp(g_settings.get_int("cpu_speed", dflt), 1, 16);
        if (speed != 1) {
            args.push_back("--cpu-speed");
            args.push_back(std::to_string(speed));
        }
    }
    if (has_flag(args, "--mod"))
        return;
    const std::string mods = g_settings.get("mods");
    std::size_t at = 0;
    while (at < mods.size()) {
        std::size_t comma = mods.find(',', at);
        if (comma == std::string::npos)
            comma = mods.size();
        std::string name = mods.substr(at, comma - at);
        at = comma + 1;
        const auto b = name.find_first_not_of(" \t");
        if (b == std::string::npos)
            continue;
        name = name.substr(b, name.find_last_not_of(" \t") - b + 1);
        const auto dir = config_dir / "mods" / name;
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            std::fprintf(stderr, "rez: mod %s: no folder %s\n", name.c_str(),
                         dir.string().c_str());
            continue;
        }
        args.push_back("--mod");
        args.push_back(dir.string());
    }
}

// Texture packs and dumps. `texture_pack` names the pack directory; unset,
// `<config dir>/textures` is used when it exists, and "off" turns packs off.
// `dump_textures = true` writes every new texture to `<config dir>/texture_dump/`. The config dir
// is the settings file's own directory, so `--settings` moves both with it. A flag the user passed
// wins over the setting.
void add_texture_flags(std::vector<std::string>& args, const std::filesystem::path& config_dir) {
    if (!has_flag(args, "--texture-pack")) {
        std::string pack = g_settings.get("texture_pack");
        std::error_code ec;
        if (pack.empty() && std::filesystem::is_directory(config_dir / "textures", ec))
            pack = (config_dir / "textures").string();
        if (pack == "off" || pack == "none" || pack == "false")
            pack.clear();
        if (!pack.empty()) {
            args.push_back("--texture-pack");
            args.push_back(pack);
        }
    }
    if (!has_flag(args, "--dump-textures") && g_settings.get_bool("dump_textures", false)) {
        args.push_back("--dump-textures");
        args.push_back((config_dir / "texture_dump").string());
    }
}

// Aspect ratio, as in the N64 recomps: `aspect = original` always shows the 4:3 picture (bars
// on a wider window), `expanded` widens the game's view to the window's own shape, never
// stretching. Older values ("4:3", "16:9", ...) read as original / expanded.
bool expanded_aspect() {
    if (!g_port || !g_port->widescreen)
        return false;
    const std::string a = g_settings.get("aspect", "expanded");
    if (a == "original" || a == "4:3")
        return false;
    if (g_settings.get("aspect").empty() && !g_settings.get("widescreen").empty())
        return g_settings.get_bool("widescreen", true);
    return true;
}

// The shape in effect now: 4:3, or with Expanded the window's (clamped to what the port has been
// verified with); kept by Core::follow_window(), read by the widescreen hook and the HUD fix.
float g_aspect_now = 4.0f / 3.0f;

float target_aspect() { return g_aspect_now; }

// `--aspect W:H` wider than 4:3 on the command line: that fixed shape for this run, whatever the
// window's (headless shots and tests at 21:9 without a 21:9 screen). 0: follow the window. Not a
// setting: the saved `aspect` keeps meaning original / expanded.
float g_fixed_aspect = 0.0f;

float parse_ratio(const std::string& s) {
    float w = 0.0f, h = 0.0f;
    if (std::sscanf(s.c_str(), "%f:%f", &w, &h) == 2 && w > 0.0f && h > 0.0f)
        return w / h;
    return 0.0f;
}

// The shape Expanded asks for from a window of `window_aspect` (0: not known yet).
float expanded_target(float window_aspect) {
    const float a = g_fixed_aspect > 0.0f ? g_fixed_aspect
                    : window_aspect > 0.0f ? window_aspect
                                           : 16.0f / 9.0f;
    return std::clamp(a, 4.0f / 3.0f, g_port ? g_port->max_aspect : 4.0f / 3.0f);
}

class Core final : public dream::host::Extension {
public:
    const char* name() const override { return "rez"; }

    void adjust_args(std::vector<std::string>& args) override {
        const bool bare = args.size() == 1;
        const std::string id = g_port && g_port->id ? g_port->id : "rez";
        const std::filesystem::path config_dir =
            default_config_dir(g_port && g_port->folder ? g_port->folder : id);
        // Double-clicked: the play path with the saved settings.
        const bool play = bare;

        std::filesystem::path file = flag_value(args, "--settings");
        if (file.empty())
            file = config_dir / "settings.ini";
        g_settings.load(file);

        if (!has_flag(args, "--config")) {
            // A packaged port keeps its config next to the executable; a build tree one level up.
            const auto dir = exe_dir(args[0]);
            for (const auto& cand : {dir / (id + ".toml"), dir / "game" / (id + ".toml"),
                                     dir.parent_path().parent_path() / "game" / (id + ".toml")}) {
                if (std::filesystem::exists(cand)) {
                    args.push_back("--config");
                    args.push_back(cand.string());
                    break;
                }
            }
        }
#ifdef DREAMCOMP_NEXTUI
        // A NextUI handheld: the image comes from the disc folder beside the executable
        // (--disc-dir: another), chosen on the device's screens, and "Starting ..." stays up until
        // the game's first picture (on_vblank). A run given --disc skips them.
        {
            nextui::DiscFlow flow;
            flow.disc_dir = flag_value(args, "--disc-dir");
            drop_flag(args, "--disc-dir", true);
            if (flow.disc_dir.empty())
                flow.disc_dir = exe_dir(args[0]) / "disc";
            if (!has_flag(args, "--disc")) {
                flow.config = flag_value(args, "--config");
                flow.request = g_port ? g_port->disc_request : nullptr;
                flow.exiting = g_port ? g_port->exit_message : nullptr;
                const auto disc = nextui::choose_disc(flow);
                std::fflush(stdout);
                if (disc.empty())
                    std::exit(0);
                args.push_back("--disc");
                args.push_back(disc.string());
            }
        }
#endif
        // `--set` is parsed after this, but the texture keys decide which engine flags to add
        // here, so those two are applied now. parse_arg sets them again to the same value, and a
        // setting only becomes dirty once, so --save-settings behaves as it does for any key.
        for (std::size_t i = 1; i + 1 < args.size(); ++i) {
            if (args[i] != "--set")
                continue;
            const std::string& kv = args[i + 1];
            const auto eq = kv.find('=');
            const std::string key = kv.substr(0, eq);
            if (eq != std::string::npos &&
                (key == "texture_pack" || key == "dump_textures" || key == "aspect" ||
                 key == "fullscreen" || key == "scale" || key == "rumble" || key == "mods" || key == "volume" || key == "clip" ||
                 key == "fps" || key == "frame_timing" || key == "latency" || key == "cpu_speed"))
                g_settings.set(key, kv.substr(eq + 1));
        }
        // The aspect flags decide the render target's shape, which the engine needs before it
        // parses anything: apply them now too (parse_arg consumes them later).
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--widescreen")
                g_settings.set("aspect", "16:9");
            else if (args[i] == "--no-widescreen")
                g_settings.set("aspect", "4:3");
            else if (args[i] == "--aspect" && i + 1 < args.size()) {
                g_settings.set("aspect", args[i + 1]);
                const float r = parse_ratio(args[i + 1]);
                g_fixed_aspect = r > 4.0f / 3.0f + 0.01f ? r : 0.0f;
            }
        }
        add_texture_flags(args, g_settings.file().parent_path());
        add_input_and_mod_flags(args, g_settings.file().parent_path());
        // Expanded starts at 16:9 (the window opens at that shape) and then follows the window.
        g_aspect_now = expanded_aspect() ? expanded_target(0.0f) : 4.0f / 3.0f;
        const float aspect = target_aspect();
        if (aspect > 4.0f / 3.0f + 0.01f && g_port->widescreen_anamorphic &&
            !has_flag(args, "--render-aspect")) {
            args.push_back("--render-aspect");
            args.push_back(std::to_string(aspect));
        }
        if (g_settings.get_bool("fullscreen", false) && !has_flag(args, "--fullscreen"))
            args.push_back("--fullscreen");
        // Frame rate: auto blends a frame between game frames on displays above 60 Hz; 120
        // forces it; 60 is the game's own rate only.
        if (!has_flag(args, "--interpolate") && !has_flag(args, "--interpolate-auto")) {
            const std::string fps = g_settings.get("fps", "auto");
            if (fps == "auto")
                args.push_back("--interpolate-auto");
            else if (fps == "120")
                args.push_back("--interpolate");
        }
        // Frame timing: "display" (default) paces the game at the display's rate when it is
        // within 0.5 % of the game's, so a 60 Hz panel never repeats a frame;
        // "exact" keeps the Dreamcast's 59.94 Hz.
        if (g_settings.get("frame_timing", "display") != "exact" && !has_flag(args, "--sync-display"))
            args.push_back("--sync-display");
        // Latency: "low" presents through DXGI on Windows (about 13 ms less where every frame is
        // finished in time); "standard" (default) through the Vulkan swapchain.
        if (g_settings.get("latency", "standard") == "low" && !has_flag(args, "--present-path")) {
            args.push_back("--present-path");
            args.push_back("dxgi");
        }

        // For the in-game menu (wants_overlay()).
        config_path_ = flag_value(args, "--config");
        exe_dir_ = exe_dir(args[0]);
        // HUD overrides: the port's beside its config, the player's beside the settings.
        if (!config_path_.empty())
            hud_port_file_ = config_path_.parent_path() / "hud_overrides.ini";
        if (!g_settings.file().empty())
            hud_user_file_ = g_settings.file().parent_path() / "hud_overrides.ini";
        load_hud_overrides();

        if (!has_flag(args, "--disc")) {
            const std::string disc = g_settings.get("disc");
            if (!disc.empty()) {
                args.push_back("--disc");
                args.push_back(disc);
            }
        }
        // Played in a window (double-clicked, or `--window`): the player's saved choices. A flag
        // given on the command line wins.
        if (play || has_flag(args, "--window")) {
#ifdef DREAM_WITH_RENDERER
            if (!has_flag(args, "--window"))
                args.push_back("--window");
#endif
            // A build without a renderer plays headless.
            if (!has_flag(args, "--scale")) {
                args.push_back("--scale");
                args.push_back(scale_arg(scale_setting()));
            }
            if (!has_flag(args, "--vmu")) {
                const std::string vmu = g_settings.get(
                    "vmu", (config_dir / "vmu_a1.bin").string());
                // The engine creates a missing card, but not the folder it lives in, which on a
                // first launch does not exist yet.
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(vmu).parent_path(), ec);
                args.push_back("--vmu");
                args.push_back(vmu);
            }
            // The pipeline cache and (where they can be edited) the bindings beside the settings:
            // all of the port's files in one folder.
            if (const auto dir = g_settings.file().parent_path(); !dir.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                if (!has_flag(args, "--cache-dir")) {
                    args.push_back("--cache-dir");
                    args.push_back(dir.string());
                }
#ifndef DREAMCOMP_NEXTUI
                if (!has_flag(args, "--bindings")) {
                    args.push_back("--bindings");
                    args.push_back((dir / "bindings.txt").string());
                }
#endif
            }
            // The port's list of the pipeline variants its game uses, built at startup
            // (<id>-pipelines.txt beside the executable, or in game/ in a build tree).
            if (!has_flag(args, "--pipeline-keys")) {
                const auto dir = exe_dir(args[0]);
                for (const auto& cand : {dir / (id + "-pipelines.txt"), dir / "game" / (id + "-pipelines.txt"),
                                         dir.parent_path().parent_path() / "game" / (id + "-pipelines.txt")}) {
                    if (std::filesystem::exists(cand)) {
                        args.push_back("--pipeline-keys");
                        args.push_back(cand.string());
                        break;
                    }
                }
            }
        }
    }

    int parse_arg(int i, int argc, char** argv) override {
        const char* a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[i + 1] : nullptr; };
        if (!std::strcmp(a, "--widescreen") || !std::strcmp(a, "--no-widescreen"))
            return 1;  // applied in adjust_args
        if (!std::strcmp(a, "--aspect") && next())
            return 2;  // applied in adjust_args
        if (!std::strcmp(a, "--fit") && next()) {
            g_settings.set("fit", next());
            return 2;
        }
        if (!std::strcmp(a, "--settings") && next())
            return 2;  // consumed in adjust_args
        if (!std::strcmp(a, "--save-settings")) {
            save_on_exit_ = true;
            return 1;
        }
        if (!std::strcmp(a, "--set") && next()) {
            const std::string kv = next();
            const auto eq = kv.find('=');
            if (eq != std::string::npos)
                g_settings.set(kv.substr(0, eq), kv.substr(eq + 1));
            return 2;
        }
        return 0;
    }

    void usage(std::FILE* out) override {
        std::fprintf(out,
                     "\nrez:\n"
                     "  --aspect W:H           4:3 (original), 16:9, 21:9 or 32:9: a wider view, not\n"
                     "                         a stretch (ports with widescreen support), fixed for\n"
                     "                         this run (clamped to the port's maximum); `expanded`\n"
                     "                         follows the window\n"
                     "  --widescreen / --no-widescreen  shorthand for --aspect 16:9 / 4:3\n"
                     "  --fit MODE             letterbox, crop or stretch (default crop: no bars)\n"
                     "  --set KEY=VALUE        override one setting for this run\n"
                     "  --save-settings        keep this run's overrides\n"
                     "  --settings FILE        settings file (default: %s)\n"
                     "  Settings rumble=0..100, volume=0..100 and mods=a,b add --rumble,\n"
                     "  --volume and --mod\n"
                     "  <settings dir>/mods/<name> (first wins).\n"
                     "  Settings texture_pack=DIR|off and dump_textures=true add --texture-pack\n"
                     "  and --dump-textures (default pack: <settings dir>/textures if present).\n"
                     "  Started with no arguments, the game plays in a window with the saved\n"
                     "  settings (disc, scale, memory card).\n",
                     g_settings.file().string().c_str());
    }

    void on_start(dream::System& sys) override {
        // Settings changed by flags this run are not saved unless asked: a test run must not
        // leave its choices behind in the player's file.
        launch_dirty_ = g_settings.dirty();
        hud_edges_ = g_settings.get("hud_layout", "edges") != "center";
        // `sharp_2d` (default true): 1:1 2D quads point sampled, as the console showed them at 1x
        // (include/dreamcomp/sharp2d.h).
        sharp_2d_ = g_settings.get_bool("sharp_2d", true);
        apply_presentation();
        if (g_port && g_port->on_start)
            g_port->on_start(sys, g_settings);
        std::printf("rez: %s, settings %s%s\n", g_port && g_port->title ? g_port->title : "?",
                    g_settings.file().string().c_str(),
                    expanded_aspect() ? ", aspect expanded" : ", aspect original");
    }

    // Expanded: the view follows the window's shape. A new shape is taken once the window has
    // kept it for a quarter of a second, so dragging an edge does not rebuild the render target
    // every frame. Original: back to 4:3.
    void follow_window() {
        if (!g_port || !g_port->widescreen)
            return;
        auto& hc = dream::host::host_controls();
        const float want = expanded_aspect()
                               ? expanded_target(hc.window_aspect ? hc.window_aspect() : 0.0f)
                               : 4.0f / 3.0f;
        if (std::fabs(want - g_aspect_now) < 0.005f) {
            settle_ = 0;
            return;
        }
        if (std::fabs(want - settle_aspect_) > 0.002f) {
            settle_aspect_ = want;
            settle_ = 0;
            return;
        }
        if (++settle_ < 15)
            return;
        settle_ = 0;
        g_aspect_now = want;
        if (g_port->widescreen_anamorphic && hc.set_render_aspect)
            hc.set_render_aspect(want);
    }

    void on_vblank(dream::System& sys) override {
        ++vblanks_;
#ifdef DREAMCOMP_NEXTUI
        // The start screen gives way at the game's first picture (the port's count of them, else
        // the PowerVR model's framebuffer flips). A build without a picture says so, then ends the run.
        if (nextui::start_screen_up()) {
            const std::uint64_t shown = g_port && g_port->pictures_shown ? g_port->pictures_shown()
                                        : (sys.pvr_core ? sys.pvr_core->frame_swaps : 0);
            if (shown > 0) {
#ifdef DREAM_WITH_RENDERER
                nextui::game_started(vblanks_, true);
#else
                nextui::game_started(vblanks_, false);
                if (dream::host::host_controls().quit)
                    dream::host::host_controls().quit();
#endif
            }
        }
#endif
        follow_window();
        if (g_port && g_port->widescreen)
            g_port->widescreen(sys, widescreen() ? target_aspect() : 4.0f / 3.0f);
        if (g_port && g_port->on_vblank)
            g_port->on_vblank(sys, g_settings);
    }

    // Widescreen HUD correction (PortInfo::hud, src/hud.cpp) with the port's and the player's
    // overrides. While the F1 editor watches, every frame also leaves a snapshot of the HUD for
    // it -- with a 4:3 picture too, where nothing is moved.
    void on_frame(dream::render::Frame& frame) override {
        // Before the HUD correction, which moves pieces off whole console pixels.
        if (sharp_2d_)
            sharpen_2d(frame);
        if (!g_port || !g_port->hud.enabled)
            return;
        // A widened picture never stretches the HUD.
        const bool watching = hud_watch_.load(std::memory_order_relaxed);
        const bool apply = widescreen();
        if (!apply && !watching)
            return;
        hud::Snapshot snap;
        hud::Stats stats;
        hud::correct(frame, g_port->hud, apply ? target_aspect() : 4.0f / 3.0f, hud_edges_, apply,
                     hud_overrides_, watching ? &snap : nullptr, stats);
        if (!watching)
            return;
        snap.image_aspect = widescreen() ? target_aspect() : 4.0f / 3.0f;
        snap.frame = ++hud_snap_seq_;  // not vblanks_: a paused game redraws without a vblank
        snap.port_overrides = hud_port_overrides_.items.size();
        snap.user_overrides = hud_user_overrides_.items.size();
        snap.unsaved = hud_unsaved_;
        snap.status = hud_status_;
        snap.user_file = hud_user_file_.string();
        std::lock_guard<std::mutex> lock(hud_mutex_);
        hud_snapshot_ = std::move(snap);
        hud_snapshot_ready_ = true;
    }

    // The render thread may do this frame's work (host_ext.h) while it is only the 2D sharpening, a
    // function of the frame alone: the HUD correction and its editor read settings and overrides
    // the menus change on the guest's thread.
    bool frame_offthread_ok() override {
        if (!g_port || !g_port->hud.enabled)
            return true;
        return !hud_watch_.load(std::memory_order_relaxed) && !widescreen();
    }
    void on_frame_offthread(dream::render::Frame& frame) override {
        if (sharp_2d_)
            sharpen_2d(frame);
    }

    // --- HUD overrides (game thread) ---
    void load_hud_overrides() {
        hud_port_overrides_.items.clear();
        hud_user_overrides_.items.clear();
        std::string err;
        if (!hud_port_file_.empty() && !hud_port_overrides_.load(hud_port_file_, &err))
            std::fprintf(stderr, "rez: %s\n", err.c_str());
        err.clear();
        if (!hud_user_file_.empty() && !hud_user_overrides_.load(hud_user_file_, &err))
            std::fprintf(stderr, "rez: %s\n", err.c_str());
        rebuild_hud_overrides();
        if (!hud_overrides_.items.empty())
            std::printf("rez: HUD overrides: %zu from the port, %zu from the player\n",
                        hud_port_overrides_.items.size(), hud_user_overrides_.items.size());
    }
    void rebuild_hud_overrides() {
        hud_overrides_.items = hud_port_overrides_.items;
        hud_overrides_.items.insert(hud_overrides_.items.end(), hud_user_overrides_.items.begin(),
                                    hud_user_overrides_.items.end());
    }
    // The player's override for `box`: the one that decided it if that is the player's, else a
    // new one covering the element (2 px of slack) and its textures.
    // An element wider than the rule's full_width spans the screen: a grouping accident (pieces
    // chained across it), and a new override from it would catch everything with its textures
    // everywhere. Large but narrower elements are real, so area alone is no guard.
    static bool too_large(const hud::Box& box) {
        const float full = g_port ? g_port->hud.full_width : 600.0f;
        return box.ox1 - box.ox0 > full;
    }
    hud::Override& user_override_for(const hud::Box& box) {
        const int port_n = static_cast<int>(hud_port_overrides_.items.size());
        if (box.override_index >= port_n &&
            box.override_index - port_n < static_cast<int>(hud_user_overrides_.items.size()))
            return hud_user_overrides_.items[static_cast<std::size_t>(box.override_index - port_n)];
        hud::Override o;
        o.x0 = std::floor(box.ox0) - 2.0f;
        o.y0 = std::floor(box.oy0) - 2.0f;
        o.x1 = std::ceil(box.ox1) + 2.0f;
        o.y1 = std::ceil(box.oy1) + 2.0f;
        o.tcws = box.tcws;
        hud_user_overrides_.items.push_back(std::move(o));
        return hud_user_overrides_.items.back();
    }
    void hud_edit(const hud::Box& box, hud::Edit edit) {
        using hud::Anchor;
        const int port_n = static_cast<int>(hud_port_overrides_.items.size());
        const bool own = box.override_index >= port_n;  // edits the player's own override
        if (edit != hud::Edit::Reset && !own && too_large(box)) {
            hud_status_ = "this outline spans most of the screen: it is several elements the "
                          "grouping chained together. Right click resets an override that made it; "
                          "edit the pieces once they show separately.";
            std::printf("hud editor: refused an override over %.0f,%.0f-%.0f,%.0f (too large)\n",
                        box.ox0, box.oy0, box.ox1, box.oy1);
            return;
        }
        switch (edit) {
            case hud::Edit::Cycle: {
                const Anchor next = box.anchor == Anchor::Left     ? Anchor::Center
                                    : box.anchor == Anchor::Center ? Anchor::Right
                                    : box.anchor == Anchor::Right  ? Anchor::Stretch
                                                                   : Anchor::Left;
                user_override_for(box).anchor = next;
                hud_status_ = std::string("element set to ") + hud::name(next);
                break;
            }
            case hud::Edit::Add:
                user_override_for(box).anchor = Anchor::Auto;
                hud_status_ = "piece added to the HUD (automatic anchor)";
                break;
            case hud::Edit::Reset:
                if (box.override_index >= port_n) {
                    hud_user_overrides_.items.erase(hud_user_overrides_.items.begin() +
                                                    (box.override_index - port_n));
                    hud_status_ = "override removed";
                } else if (box.override_index >= 0) {
                    user_override_for(hud::Box{box}).anchor = Anchor::Auto;  // masks the port's
                    hud_status_ = "port override masked: automatic anchor";
                } else {
                    hud_status_ = "nothing to reset: this element is automatic";
                    return;
                }
                break;
        }
        hud_unsaved_ = true;
        rebuild_hud_overrides();
    }
    // The F2 list: an anchor chosen from a dropdown rather than cycled by clicks.
    void hud_set(const hud::Box& box, const std::string& value) {
        using hud::Anchor;
        const int port_n = static_cast<int>(hud_port_overrides_.items.size());
        if (value == "none")
            return;  // a grey piece left out of the HUD: nothing to record
        if (value == "auto" && box.hud) {
            if (box.override_index < 0) {
                hud_status_ = "already automatic";
                return;
            }
            hud_edit(box, hud::Edit::Reset);
            return;
        }
        Anchor a = Anchor::Auto;
        if (value != "auto" && !hud::parse(value, a))
            return;
        if (box.override_index < port_n && too_large(box)) {
            hud_status_ = "this outline spans the screen: it is several elements chained together";
            std::printf("hud editor: refused an override over %.0f,%.0f-%.0f,%.0f (too large)\n",
                        box.ox0, box.oy0, box.ox1, box.oy1);
            return;
        }
        user_override_for(box).anchor = a;
        hud_status_ = std::string(box.hud ? "element set to " : "piece added: ") + hud::name(a);
        std::printf("hud editor: set %s at %.0f,%.0f-%.0f,%.0f\n", hud::name(a), box.ox0, box.oy0,
                    box.ox1, box.oy1);
        hud_unsaved_ = true;
        rebuild_hud_overrides();
    }

    void hud_save() {
        std::string err;
        if (hud_user_overrides_.save(hud_user_file_, &err)) {
            hud_unsaved_ = false;
            hud_status_ = "saved " + std::to_string(hud_user_overrides_.items.size()) +
                          " override(s)";
            std::printf("rez: HUD overrides saved to %s\n", hud_user_file_.string().c_str());
        } else {
            hud_status_ = "save failed: " + err;
            std::fprintf(stderr, "rez: %s\n", err.c_str());
        }
    }

    // After Apply in the in-game menu: what can change without a restart does so now (fit, HUD,
    // rumble, fullscreen). Everything else is marked "Next start" in the menu.
    void apply_live() {
        hud_edges_ = g_settings.get("hud_layout", "edges") != "center";
        // `sharp_2d` (default true): 1:1 2D quads point sampled, as the console showed them at 1x
        // (include/dreamcomp/sharp2d.h).
        sharp_2d_ = g_settings.get_bool("sharp_2d", true);
        apply_presentation();
        auto& hc = dream::host::host_controls();
        if (hc.set_rumble)
            hc.set_rumble(static_cast<float>(std::clamp(g_settings.get_int("rumble", 100), 0, 100)) /
                          100.0f);
        if (hc.set_volume)
            hc.set_volume(static_cast<float>(std::clamp(g_settings.get_int("volume", 100), 0, 100)) /
                          100.0f);
        if (hc.set_soft_clip)
            hc.set_soft_clip(g_settings.get("clip", "hard") == "soft");
        // Resolution: the render target is rebuilt at the next vblank (no restart).
        if (hc.set_render_scale)
            hc.set_render_scale(static_cast<unsigned>(scale_setting()));
        if (hc.set_fullscreen && hc.fullscreen &&
            hc.fullscreen() != g_settings.get_bool("fullscreen", false))
            hc.set_fullscreen(g_settings.get_bool("fullscreen", false));
        launch_dirty_ = false;  // the file now holds what is in effect
        std::printf("rez: applied in game: aspect %s, hud %s, rumble %d%%, fullscreen %s\n",
                    expanded_aspect() ? "expanded" : "original", hud_edges_ ? "edges" : "center", g_settings.get_int("rumble", 100),
                    g_settings.get_bool("fullscreen", false) ? "on" : "off");
    }

    void on_stop(dream::System& sys, const char* why) override {
#ifdef DREAMCOMP_NEXTUI
        nextui::game_ending();
#endif
        if (g_port && g_port->on_stop)
            g_port->on_stop(sys, g_settings);
        (void)sys, (void)why;
        if (save_on_exit_ || (g_settings.dirty() && !launch_dirty_))
            g_settings.save();
    }

private:
    static bool widescreen() { return target_aspect() > 4.0f / 3.0f + 0.01f; }

    static void apply_presentation() {
#ifdef DREAMCOMP_HAS_PRESENTER
        auto& o = dream::render::vk::present_options();
        // Never stretched or cropped: the picture keeps its shape and the rest of the window is
        // black (with Expanded the shapes match, so there are no bars).
        o.fit = dream::render::vk::PresentOptions::Fit::Letterbox;
        // The render target already has the picture's shape: show it as is.
        o.display_aspect = 0.0f;
#endif
    }

    bool save_on_exit_ = false;
    std::filesystem::path config_path_, exe_dir_;
    std::uint64_t vblanks_ = 0;
    bool hud_edges_ = true;
    float settle_aspect_ = 0.0f;
    unsigned settle_ = 0;
    // Atomic: on_frame_offthread() reads it on the launcher's render thread.
    std::atomic<bool> sharp_2d_{true};
    // HUD overrides: the port's (read only), the player's (edited with F1), and both in order.
    hud::Overrides hud_port_overrides_, hud_user_overrides_, hud_overrides_;
    std::filesystem::path hud_port_file_, hud_user_file_;
    bool hud_unsaved_ = false;
    std::string hud_status_;
    std::atomic<bool> hud_watch_{false};
    std::atomic<bool> hud_paused_{false};
    std::uint64_t hud_snap_seq_ = 0;
    std::mutex hud_mutex_;  // hud_snapshot_ for the overlay thread
    hud::Snapshot hud_snapshot_;
    bool hud_snapshot_ready_ = false;
    bool launch_dirty_ = false;
};

Core g_core;
dream::host::Register g_reg_core(g_core);

}  // namespace

const PortInfo* port() { return g_port; }

Settings& settings() { return g_settings; }

int render_scale() {
    return scale_setting();
}

void set_render_scale(int scale) {
    if (scale != kScalePanel)
        scale = std::clamp(scale, 1, kMaxScale);
    g_settings.set("scale", scale_arg(scale));
    g_settings.save();
    auto& hc = dream::host::host_controls();
    if (hc.set_render_scale)
        hc.set_render_scale(static_cast<unsigned>(scale));
    int w = 0, h = 0;
    render_size(scale, w, h);
    std::printf("rez: drawing at %dx%d from the next frame\n", w, h);
}

int render_scale_high() {
#ifdef DREAMCOMP_NEXTUI
    return kScalePanel;  // the panel's pixels one to one
#else
    return 2;
#endif
}

void render_size(int scale, int& w, int& h) {
    unsigned tw = 0, th = 0;
    if (auto& hc = dream::host::host_controls(); hc.render_size)
        hc.render_size(static_cast<unsigned>(scale), tw, th);
    else if (scale != kScalePanel)
        tw = 640u * static_cast<unsigned>(scale), th = 480u * static_cast<unsigned>(scale);
    w = static_cast<int>(tw), h = static_cast<int>(th);
}

RegisterPort::RegisterPort(const PortInfo& info) { g_port = &info; }

}  // namespace dreamcomp
