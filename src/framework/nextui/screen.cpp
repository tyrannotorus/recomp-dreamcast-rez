#include "nextui/screen.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dreamcomp::nextui {

extern const unsigned char kNotoSansBold[];
extern const std::size_t kNotoSansBoldSize;

namespace {

// Point sizes on a 768-line panel.
constexpr float kPoints[Screen::kSizes] = {44, 28, 24, 22, 20};
// Buttons as NextUI numbers them on tg5040 (workspace/tg5040/platform/platform.h); the d-pad is
// hat 0.
constexpr int kJoyA = 1, kJoyB = 0;

TTF_Font* font(void* f) { return static_cast<TTF_Font*>(f); }

SDL_Color color(std::uint32_t rgb) {
    return SDL_Color{static_cast<Uint8>(rgb >> 16), static_cast<Uint8>(rgb >> 8), static_cast<Uint8>(rgb), 255};
}

}  // namespace

Screen::~Screen() {
    for (auto*& f : fonts_)
        if (f) {
            TTF_CloseFont(static_cast<TTF_Font*>(f));
            f = nullptr;
        }
    if (ttf_)
        TTF_Quit();
    if (frame_)
        SDL_FreeSurface(frame_);
    // Only what open() started: the game's sound plays through SDL2 too (dream::audio's NextUI
    // sink), and SDL_Quit would close its device at the game's first picture.
    if (window_) {
        SDL_DestroyWindow(window_);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK);
    }
}

bool Screen::open_offscreen(int w, int h, std::string& error) {
    w_ = w, h_ = h;
    scale_ = std::min(static_cast<float>(w_) / 1024.0f, static_cast<float>(h_) / 768.0f);
    frame_ = SDL_CreateRGBSurfaceWithFormat(0, w_, h_, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!frame_ || TTF_Init() != 0) {
        error = std::string("SDL_ttf: ") + SDL_GetError();
        return false;
    }
    ttf_ = true;
    for (int i = 0; i < kSizes; ++i) {
        SDL_RWops* rw = SDL_RWFromConstMem(kNotoSansBold, static_cast<int>(kNotoSansBoldSize));
        fonts_[i] = rw ? TTF_OpenFontRW(rw, 1, std::max(8, px(kPoints[i]))) : nullptr;
        if (!fonts_[i]) {
            error = std::string("font: ") + TTF_GetError();
            return false;
        }
    }
    return true;
}

const std::uint32_t* Screen::pixels() const {
    return frame_ ? static_cast<const std::uint32_t*>(frame_->pixels) : nullptr;
}

int Screen::pitch_pixels() const {
    return frame_ ? frame_->pitch / 4 : 0;
}

bool Screen::open(std::string& error) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
        error = std::string("SDL: ") + SDL_GetError();
        return false;
    }
    SDL_ShowCursor(SDL_DISABLE);
    int w = 0, h = 0;
    if (SDL_DisplayMode m; SDL_GetDesktopDisplayMode(0, &m) == 0)
        w = m.w, h = m.h;
    if (w < 320 || h < 240)
        w = 1024, h = 768;
    window_ = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, SDL_WINDOW_SHOWN);
    SDL_Surface* surface = window_ ? SDL_GetWindowSurface(window_) : nullptr;
    if (!surface) {
        error = std::string("SDL window: ") + SDL_GetError();
        if (window_)
            SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK);
        return false;
    }
    w_ = surface->w, h_ = surface->h;
    scale_ = std::min(static_cast<float>(w_) / 1024.0f, static_cast<float>(h_) / 768.0f);
    frame_ = SDL_CreateRGBSurfaceWithFormat(0, w_, h_, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!frame_ || TTF_Init() != 0) {
        error = std::string("SDL_ttf: ") + SDL_GetError();
        return false;
    }
    ttf_ = true;
    for (int i = 0; i < kSizes; ++i) {
        SDL_RWops* rw = SDL_RWFromConstMem(kNotoSansBold, static_cast<int>(kNotoSansBoldSize));
        fonts_[i] = rw ? TTF_OpenFontRW(rw, 1, std::max(8, px(kPoints[i]))) : nullptr;
        if (!fonts_[i]) {
            error = std::string("font: ") + TTF_GetError();
            return false;
        }
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        SDL_JoystickOpen(i);
    opened_ms_ = SDL_GetTicks();
    std::printf("nextui: screen %dx%d\n", w_, h_);
    return true;
}

void Screen::clear(std::uint32_t rgb) {
    SDL_FillRect(frame_, nullptr, SDL_MapRGB(frame_->format, rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255));
}

// Anti-aliased: each pixel takes the share of it the shape covers, from its distance to the edge.
void Screen::round_rect(int x, int y, int w, int h, int r, std::uint32_t rgb) {
    r = std::min({r, w / 2, h / 2});
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(w_, x + w), y1 = std::min(h_, y + h);
    auto* px = static_cast<std::uint32_t*>(frame_->pixels);
    const int pitch = frame_->pitch / 4;
    for (int j = y0; j < y1; ++j)
        for (int i = x0; i < x1; ++i) {
            // Distance outside the rounded corners (negative inside), at the pixel's centre.
            const float pi = static_cast<float>(i) + 0.5f, pj = static_cast<float>(j) + 0.5f;
            const float cx = std::clamp(pi, static_cast<float>(x + r), static_cast<float>(x + w - r));
            const float cy = std::clamp(pj, static_cast<float>(y + r), static_cast<float>(y + h - r));
            const float d = std::hypot(pi - cx, pj - cy) - static_cast<float>(r);
            const float a = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (a <= 0)
                continue;
            std::uint32_t& dst = px[j * pitch + i];
            auto mix = [&](int shift) {
                const float s = static_cast<float>(rgb >> shift & 255),
                            t = static_cast<float>(dst >> shift & 255);
                return static_cast<std::uint32_t>(t + (s - t) * a + 0.5f) << shift;
            };
            dst = 0xFF000000u | mix(16) | mix(8) | mix(0);
        }
}

int Screen::text_width(Size size, const std::string& s) {
    int w = 0, h = 0;
    TTF_SizeUTF8(font(fonts_[size]), s.c_str(), &w, &h);
    return w;
}

int Screen::line_height(Size size) { return TTF_FontLineSkip(font(fonts_[size])); }

void Screen::text(Size size, const std::string& s, int x, int y, std::uint32_t rgb) {
    if (s.empty())
        return;
    SDL_Surface* t = TTF_RenderUTF8_Blended(font(fonts_[size]), s.c_str(), color(rgb));
    if (!t)
        return;
    SDL_Rect dst{x, y, t->w, t->h};
    SDL_BlitSurface(t, nullptr, frame_, &dst);
    SDL_FreeSurface(t);
}

void Screen::text_centered(Size size, const std::string& s, int cx, int y, std::uint32_t rgb) {
    text(size, s, cx - text_width(size, s) / 2, y, rgb);
}

std::vector<std::string> Screen::wrap(Size size, const std::string& s, int max_w) {
    auto trimmed = [](std::string t) {
        while (!t.empty() && t.back() == ' ')
            t.pop_back();
        return t;
    };
    // Pieces: words with their trailing space; a word wider than a line is cut into the longest
    // runs that fit, ending after a '/' where one does, and never inside a UTF-8 sequence.
    std::vector<std::string> pieces;
    std::size_t start = 0;
    while (start < s.size()) {
        std::size_t end = s.find(' ', start);
        end = end == std::string::npos ? s.size() : end + 1;
        std::string rest = s.substr(start, end - start);
        start = end;
        while (text_width(size, trimmed(rest)) > max_w) {
            std::size_t n = 0;
            for (std::size_t next = 1;; ++next) {
                while (next < rest.size() && (static_cast<unsigned char>(rest[next]) & 0xC0) == 0x80)
                    ++next;
                if (next >= rest.size() || text_width(size, rest.substr(0, next)) > max_w)
                    break;
                n = next;
            }
            if (n == 0)
                break;  // not even one character fits: leave it whole
            const std::size_t slash = rest.rfind('/', n - 1);
            if (slash != std::string::npos && slash > 0)
                n = slash + 1;
            pieces.push_back(rest.substr(0, n));
            rest.erase(0, n);
        }
        if (!rest.empty())
            pieces.push_back(rest);
    }
    std::vector<std::string> lines;
    std::string line;
    for (const auto& p : pieces) {
        if (!line.empty() && text_width(size, trimmed(line + p)) > max_w) {
            lines.push_back(trimmed(line));
            line.clear();
        }
        line += p;
    }
    if (!trimmed(line).empty())
        lines.push_back(trimmed(line));
    return lines;
}

void Screen::hints(const std::vector<Hint>& hints) {
    const int d = px(36), gap = px(10), spacing = px(28);
    const int y = h_ - px(28) - d;
    int x = w_ - px(32);
    for (auto h = hints.rbegin(); h != hints.rend(); ++h) {
        const int lw = text_width(kHint, h->label);
        x -= lw;
        text(kHint, h->label, x, y + (d - line_height(kHint)) / 2, 0xFFFFFF);
        x -= gap;
        for (auto b = h->buttons.rbegin(); b != h->buttons.rend(); ++b) {
            x -= d;
            round_rect(x, y, d, d, d / 2, 0xFFFFFF);
            text_centered(kHintLetter, *b, x + d / 2, y + (d - line_height(kHintLetter)) / 2, 0x000000);
            x -= px(6);
        }
        x -= spacing;
    }
}

void Screen::present() {
    SDL_Surface* surface = SDL_GetWindowSurface(window_);
    if (!surface)
        return;
    SDL_BlitSurface(frame_, nullptr, surface, nullptr);
    SDL_UpdateWindowSurface(window_);
}

Screen::Input Screen::wait() {
    for (;;) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 1000)) {
            present();  // keep the panel's picture, whatever else drew there
            continue;
        }
        // The press that started the pak may still be arriving.
        const bool settled = SDL_GetTicks() - opened_ms_ > 250;
        switch (e.type) {
        case SDL_QUIT:
            return Input::Quit;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_EXPOSED)
                present();
            break;
        case SDL_JOYDEVICEADDED:
            SDL_JoystickOpen(e.jdevice.which);
            break;
        case SDL_JOYBUTTONDOWN:
            if (settled && e.jbutton.button == kJoyA)
                return Input::A;
            if (settled && e.jbutton.button == kJoyB)
                return Input::B;
            break;
        case SDL_JOYHATMOTION:
            if (settled && (e.jhat.value & SDL_HAT_UP))
                return Input::Up;
            if (settled && (e.jhat.value & SDL_HAT_DOWN))
                return Input::Down;
            break;
        case SDL_KEYDOWN:
            switch (e.key.keysym.sym) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE:
                return Input::A;
            case SDLK_ESCAPE:
            case SDLK_BACKSPACE:
                return Input::B;
            case SDLK_UP:
                return Input::Up;
            case SDLK_DOWN:
                return Input::Down;
            default:
                break;
            }
            break;
        default:
            break;
        }
    }
}

}  // namespace dreamcomp::nextui
