// Клавиатура: буквы определяются по физическому положению клавиши (скан-коду),
// поэтому WASD/E работают и в русской раскладке. Текст для промптов — из WM_CHAR.
#pragma once

#include <vector>

enum Key {
    K_NONE,
    K_W, K_A, K_S, K_D, K_E, K_T, K_R, K_Y, K_N, K_L,
    K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_ENTER, K_ESC, K_TAB, K_BACKSPACE, K_SPACE, K_DELETE,
    K_1, K_2, K_3, K_4, K_5,
    K_F1, K_F11,
    K_COUNT
};

class Input {
public:
    void keyDown(unsigned vk, unsigned scan, bool repeat);
    void keyUp(unsigned vk, unsigned scan);
    void character(wchar_t c);
    void releaseAll();
    void endFrame();

    bool held(Key k) const { return held_[k]; }
    const std::vector<Key>& pressed() const { return pressed_; }
    const std::vector<wchar_t>& chars() const { return chars_; }

private:
    static Key translate(unsigned vk, unsigned scan);

    bool held_[K_COUNT] = {};
    std::vector<Key> pressed_;
    std::vector<wchar_t> chars_;
};
