#include "input.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

Key Input::translate(unsigned vk, unsigned scan) {
    switch (scan) {  // буквы — по скан-коду (не зависят от раскладки)
        case 0x11: return K_W;
        case 0x1E: return K_A;
        case 0x1F: return K_S;
        case 0x20: return K_D;
        case 0x12: return K_E;
        case 0x14: return K_T;
        case 0x13: return K_R;
        case 0x15: return K_Y;
        case 0x31: return K_N;
        case 0x26: return K_L;
        default: break;
    }
    switch (vk) {
        case VK_UP: return K_UP;
        case VK_DOWN: return K_DOWN;
        case VK_LEFT: return K_LEFT;
        case VK_RIGHT: return K_RIGHT;
        case VK_RETURN: return K_ENTER;
        case VK_ESCAPE: return K_ESC;
        case VK_TAB: return K_TAB;
        case VK_BACK: return K_BACKSPACE;
        case VK_SPACE: return K_SPACE;
        case VK_DELETE: return K_DELETE;
        case '1': case VK_NUMPAD1: return K_1;
        case '2': case VK_NUMPAD2: return K_2;
        case '3': case VK_NUMPAD3: return K_3;
        case '4': case VK_NUMPAD4: return K_4;
        case '5': case VK_NUMPAD5: return K_5;
        case VK_F1: return K_F1;
        case VK_F11: return K_F11;
        default: return K_NONE;
    }
}

void Input::keyDown(unsigned vk, unsigned scan, bool repeat) {
    const Key k = translate(vk, scan);
    if (k == K_NONE) return;
    held_[k] = true;
    // Повтор клавиши нужен только для редактирования текста.
    if (!repeat || k == K_BACKSPACE || k == K_LEFT || k == K_RIGHT || k == K_UP || k == K_DOWN) pressed_.push_back(k);
}

void Input::keyUp(unsigned vk, unsigned scan) {
    const Key k = translate(vk, scan);
    if (k != K_NONE) held_[k] = false;
}

void Input::character(wchar_t c) {
    if (c >= 32 && c != 127) chars_.push_back(c);
}

void Input::releaseAll() {
    for (bool& h : held_) h = false;
}

void Input::endFrame() {
    pressed_.clear();
    chars_.clear();
}
