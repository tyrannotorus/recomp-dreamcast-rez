// A full-screen page on a NextUI handheld: SDL2's software window surface at the panel's size, as
// NextUI's own show2 draws, text in the embedded Noto Sans Bold, and the pad read through SDL2's
// joystick API (the keyboard too, for a PC preview). The window takes the display's size.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Surface;

namespace dreamcomp::nextui {

class Screen {
public:
    enum class Input { A, B, Up, Down, Quit };
    enum Size { kTitle, kText, kSmall, kHint, kHintLetter, kSizes };
    struct Hint {
        std::vector<std::string> buttons;  // "A", "B", ...: one circle each
        std::string label;
    };

    Screen() = default;
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;
    ~Screen();

    bool open(std::string& error);
    // A page drawn without a window (w x h, ARGB8888: fb0's BGRA8 in memory), for writing
    // straight into the framebuffer where no window can be made any more (the exit page).
    bool open_offscreen(int w, int h, std::string& error);
    const std::uint32_t* pixels() const;
    int pitch_pixels() const;
    int width() const { return w_; }
    int height() const { return h_; }
    // A length laid out for a 1024x768 panel, at this one's size.
    int px(float v) const { return static_cast<int>(v * scale_ + 0.5f); }

    void clear(std::uint32_t rgb);
    void round_rect(int x, int y, int w, int h, int r, std::uint32_t rgb);
    int text_width(Size size, const std::string& s);
    int line_height(Size size);
    void text(Size size, const std::string& s, int x, int y, std::uint32_t rgb);
    void text_centered(Size size, const std::string& s, int cx, int y, std::uint32_t rgb);
    // Lines of at most max_w pixels, broken at spaces, or at '/' in a long path.
    std::vector<std::string> wrap(Size size, const std::string& s, int max_w);
    // Button hints along the bottom, right-aligned, as NextUI shows them.
    void hints(const std::vector<Hint>& hints);
    void present();
    // The next press; Quit when the window closes.
    Input wait();

private:
    SDL_Window* window_ = nullptr;
    SDL_Surface* frame_ = nullptr;
    void* fonts_[kSizes] = {};  // TTF_Font* (its struct's name differs between SDL2_ttf versions)
    int w_ = 0, h_ = 0;
    float scale_ = 1.0f;
    std::uint32_t opened_ms_ = 0;
    bool ttf_ = false;
};

}  // namespace dreamcomp::nextui
