#include "nextui/disc_flow.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "dream/translator/config/game_config.h"
#include "nextui/disc_folder.h"
#include "nextui/screen.h"

namespace dreamcomp::nextui {

namespace {

constexpr std::uint32_t kWhite = 0xFFFFFF, kGrey = 0xB4B4B4, kDim = 0x8C8C8C, kBox = 0x2A2A2A,
                        kWarn = 0xF0B040, kBlack = 0x000000;

// The folder as the player finds it on the card: relative to the SD card's root.
std::string shown_folder(const std::filesystem::path& dir) {
    const char* sd = std::getenv("SDCARD_PATH");
    const std::string root = (sd && *sd ? std::string(sd) : std::string("/mnt/SDCARD")) + "/";
    std::string s = dir.lexically_normal().generic_string();
    if (s.size() > 1 && s.back() == '/')
        s.pop_back();
    return s.compare(0, root.size(), root) == 0 ? s.substr(root.size()) : s;
}

// Text lines as wide as on the Brick, scaled: a wider panel (the Smart Pro) wraps them alike.
int text_width(const Screen& s) { return std::min(s.width() - s.px(120), s.px(904)); }

// What players call an image: its label ("Japanese"), else its name.
std::string image_name(const dream::translator::ChdImage& img) {
    return !img.label.empty() ? img.label : !img.name.empty() ? img.name : "CHD";
}

// A supported image as the player can check it: the name and, in brackets, the SHA-1 compared.
std::string image_with_sha1(const dream::translator::ChdImage& img) {
    return image_name(img) + " (" + img.sha1 + ")";
}

// A choice in the picker: "Japanese release" for a labelled image.
std::string image_choice(const dream::translator::ChdImage& img) {
    return img.label.empty() ? image_name(img) : img.label + " release";
}

// Draws a screen's text top to bottom, each line centred across. A first pass with `draw` off
// measures it, so that the whole text can be centred down the screen too (centred() below).
struct Pen {
    Screen& s;
    bool draw;
    int y = 0;

    void para(Screen::Size size, const std::string& text, std::uint32_t rgb) {
        for (const auto& line : s.wrap(size, text, text_width(s))) {
            if (draw)
                s.text_centered(size, line, s.width() / 2, y, rgb);
            y += s.line_height(size);
        }
    }
    void gap(float v) { y += s.px(v); }
    // Text on a dark bar: the folder to copy the disc into.
    void bar(const std::string& text) {
        const auto lines = s.wrap(Screen::kText, text, text_width(s) - s.px(40));
        int w = 0;
        for (const auto& l : lines)
            w = std::max(w, s.text_width(Screen::kText, l));
        w += s.px(48);
        const int h = static_cast<int>(lines.size()) * s.line_height(Screen::kText) + s.px(16);
        if (draw)
            s.round_rect(s.width() / 2 - w / 2, y, w, h, s.px(12), kBox);
        y += s.px(8);
        for (const auto& l : lines) {
            if (draw)
                s.text_centered(Screen::kText, l, s.width() / 2, y, kWhite);
            y += s.line_height(Screen::kText);
        }
        y += s.px(8);
    }
};

// A page of text centred down the screen, ending with how to leave it; A or B closes it.
template <typename Body>
void text_page(Screen& s, Body body) {
    const auto all = [&](Pen& p) {
        body(p);
        p.gap(36);
        p.para(Screen::kText, "Press A or B to exit", kWhite);
    };
    Pen measure{s, false};
    all(measure);
    Pen pen{s, true, std::max(0, (s.height() - measure.y) / 2)};
    s.clear(kBlack);
    all(pen);
    s.present();
    for (;;) {
        const auto in = s.wait();
        if (in == Screen::Input::A || in == Screen::Input::B || in == Screen::Input::Quit)
            return;
    }
}

// "<title> needs its disc": what to copy, where, and what was found instead.
void no_disc_screen(Screen& s, const dream::translator::GameConfig& cfg, const DiscFlow& flow,
                    const std::vector<DiscFile>& files) {
    text_page(s, [&](Pen& p) {
        p.para(Screen::kTitle, cfg.title + " needs its disc", kWhite);
        p.gap(18);
        p.para(Screen::kText, flow.request ? flow.request : "Copy one of these .chd images into this folder:", kGrey);
        p.gap(14);
        p.bar(shown_folder(flow.disc_dir));
        p.gap(30);
        p.para(Screen::kSmall, "Supported CHDs (SHA-1):", kDim);
        for (const auto& img : cfg.chd_images)
            p.para(Screen::kText, image_with_sha1(img), kWhite);
        if (!files.empty()) {
            p.gap(24);
            p.para(Screen::kSmall, "Found in the folder, but not supported:", kWarn);
            const std::size_t shown = files.size() > 4 ? 3 : files.size();
            for (std::size_t i = 0; i < shown; ++i)
                p.para(Screen::kSmall, files[i].shown + ": " + files[i].why, kGrey);
            if (shown < files.size())
                p.para(Screen::kSmall, "and " + std::to_string(files.size() - shown) + " more", kGrey);
        }
    });
}

// Two or more supported images: the player picks one. -1 when closed.
int picker_screen(Screen& s, const dream::translator::GameConfig& cfg, const std::vector<const DiscFile*>& discs) {
    int sel = 0;
    for (;;) {
        const int cx = s.width() / 2, max_w = text_width(s);
        s.clear(kBlack);
        Pen p{s, true, s.px(56)};
        p.para(Screen::kTitle, "Choose a disc", kWhite);
        p.gap(12);
        p.para(Screen::kText, "More than one release of " + cfg.title + " is in the disc folder.", kGrey);
        const int y = p.y;
        const int row_h = s.px(60), row_w = std::min(max_w, s.px(760));
        const int rows = static_cast<int>(discs.size());
        int ry = std::max(y + s.px(40), (s.height() - rows * (row_h + s.px(12))) / 2);
        for (int i = 0; i < rows; ++i) {
            const std::string name = image_choice(cfg.chd_images[static_cast<std::size_t>(discs[i]->image)]);
            if (i == sel)
                s.round_rect(cx - row_w / 2, ry, row_w, row_h, row_h / 2, kWhite);
            s.text_centered(Screen::kText, name, cx, ry + (row_h - s.line_height(Screen::kText)) / 2,
                            i == sel ? kBlack : kWhite);
            ry += row_h + s.px(12);
        }
        s.hints({{{"B"}, "Close"}, {{"A"}, "Play"}});
        s.present();
        switch (s.wait()) {
        case Screen::Input::A:
            return sel;
        case Screen::Input::B:
        case Screen::Input::Quit:
            return -1;
        case Screen::Input::Up:
            sel = (sel + rows - 1) % rows;
            break;
        case Screen::Input::Down:
            sel = (sel + 1) % rows;
            break;
        }
    }
}

void error_screen(Screen& s, const std::string& what) {
    text_page(s, [&](Pen& p) {
        p.para(Screen::kTitle, "Cannot start", kWhite);
        p.gap(24);
        p.para(Screen::kText, what, kGrey);
    });
}

// From the disc's choice to the game's first picture: the start screen and when it went up.
std::unique_ptr<Screen> g_start;
std::string g_title;
std::chrono::steady_clock::time_point g_start_time;
// The page the panel keeps from the game's end until NextUI draws again (PortInfo::exit_message).
const char* g_exiting = nullptr;

// The exit message, centred, written straight into the framebuffer's first two pages (NextUI's
// own, which it draws over when it starts) and shown. No window: on the Brick SDL2 cannot make one
// again once Vulkan has run. Written as the game ends, before anything is torn down (game_ending),
// and again at exit, as the renderer hands the panel back to the page it found on the way out.
void exiting_page() {
    if (!g_exiting)
        return;
    const int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return;
    fb_var_screeninfo v{};
    fb_fix_screeninfo fix{};
    Screen s;
    std::string err;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &fix) || v.bits_per_pixel != 32 ||
        !s.open_offscreen(static_cast<int>(v.xres), static_cast<int>(v.yres), err)) {
        close(fd);
        return;
    }
    const auto all = [&](Pen& p) { p.para(Screen::kTitle, g_exiting, kWhite); };
    Pen measure{s, false};
    all(measure);
    Pen pen{s, true, std::max(0, (s.height() - measure.y) / 2)};
    s.clear(kBlack);
    all(pen);
    const std::uint32_t pages = std::min<std::uint32_t>(2, v.yres_virtual / v.yres);
    for (std::uint32_t y = 0; y < pages * v.yres; ++y)
        if (pwrite(fd, s.pixels() + static_cast<std::size_t>(y % v.yres) * static_cast<std::size_t>(s.pitch_pixels()),
                   std::size_t{v.xres} * 4, static_cast<off_t>(y) * fix.line_length) < 0)
            break;
    v.xoffset = 0;
    v.yoffset = 0;
    ioctl(fd, FBIOPAN_DISPLAY, &v);
    close(fd);
}

// "Starting <title>...", the release under it: shown at once and kept while the game boots, so
// the panel never keeps NextUI's last picture as if the device had frozen.
void start_page(Screen& s, const dream::translator::ChdImage& img, const std::string& title) {
    const auto all = [&](Pen& p) {
        p.para(Screen::kTitle, "Starting " + title + "...", kWhite);
        p.gap(16);
        p.para(Screen::kText, image_choice(img), kGrey);
    };
    Pen measure{s, false};
    all(measure);
    Pen pen{s, true, std::max(0, (s.height() - measure.y) / 2)};
    s.clear(kBlack);
    all(pen);
    s.present();
}

}  // namespace

std::filesystem::path choose_disc(const DiscFlow& flow) {
    auto screen = std::make_unique<Screen>();
    std::string err;
    dream::translator::GameConfig cfg;
    if (!dream::translator::load_game_config(flow.config, cfg, err)) {
        std::fprintf(stderr, "nextui: %s\n", err.c_str());
        std::string serr;
        if (screen->open(serr))
            error_screen(*screen, "The configuration " + flow.config.filename().string() + " could not be read: " + err);
        return {};
    }
    const auto files = scan_disc_folder(flow.disc_dir, cfg.chd_images);
    std::vector<const DiscFile*> discs;
    for (const auto& f : files) {
        std::printf("nextui: disc folder: %s: %s\n", f.shown.c_str(),
                    f.image >= 0 ? image_name(cfg.chd_images[static_cast<std::size_t>(f.image)]).c_str() : f.why.c_str());
        if (f.image < 0)
            continue;
        bool dup = false;  // two copies of one image are one choice
        for (const auto* d : discs)
            dup = dup || d->image == f.image;
        if (!dup)
            discs.push_back(&f);
    }
    // In the order the port lists its images.
    std::stable_sort(discs.begin(), discs.end(), [](const DiscFile* a, const DiscFile* b) { return a->image < b->image; });

    const DiscFile* chosen = discs.size() == 1 ? discs[0] : nullptr;
    if (!screen->open(err)) {
        // No screen at all: the one image runs without screens; anything else cannot be asked.
        std::fprintf(stderr, "nextui: no screen (%s)\n", err.c_str());
        return chosen ? chosen->path : std::filesystem::path{};
    }
    if (discs.empty()) {
        std::vector<DiscFile> unsupported;
        for (const auto& f : files)
            if (f.image < 0)
                unsupported.push_back(f);
        std::printf("nextui: no supported disc in %s\n", flow.disc_dir.string().c_str());
        no_disc_screen(*screen, cfg, flow, unsupported);
        return {};
    }
    if (!chosen) {
        const int i = picker_screen(*screen, cfg, discs);
        if (i < 0)
            return {};
        chosen = discs[static_cast<std::size_t>(i)];
    }
    start_page(*screen, cfg.chd_images[static_cast<std::size_t>(chosen->image)], cfg.title);
    g_start = std::move(screen);
    g_title = cfg.title;
    g_start_time = std::chrono::steady_clock::now();
    if (flow.exiting) {
        g_exiting = flow.exiting;
        std::atexit(exiting_page);  // after main has returned: the renderer is gone by then
    }
    std::printf("nextui: disc %s; starting\n", chosen->path.string().c_str());
    return chosen->path;
}

bool start_screen_up() { return g_start != nullptr; }

void game_ending() { exiting_page(); }

void game_started(std::uint64_t frame, bool has_picture) {
    if (!g_start)
        return;
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start_time).count();
    std::printf("nextui: the game's first picture at frame %llu, %.1f s after the start screen\n",
                static_cast<unsigned long long>(frame), secs);
    std::fflush(stdout);
    if (!has_picture) {
        char took[160];
        std::snprintf(took, sizeof took, "It reached its first picture after %.1f s (frame %llu), but this build cannot show the game on this device yet.",
                      secs, static_cast<unsigned long long>(frame));
        text_page(*g_start, [&](Pen& p) {
            p.para(Screen::kTitle, g_title + " started", kWhite);
            p.gap(24);
            p.para(Screen::kText, took, kGrey);
        });
    }
    g_start.reset();
}

}  // namespace dreamcomp::nextui
