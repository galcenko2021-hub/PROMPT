// Интерфейс: HUD, журнал, диалоги, меню, строка промпта, панель мыслей Помощника,
// меню исправления (дообучение), лаборатория нейросетей, экраны апгрейда и финала.
#pragma once

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "core.h"
#include "input.h"
#include "intent.h"
#include "json.h"
#include "levels.h"
#include "render.h"

class AssistantRoster;
class BrainClient;
class Room;

namespace ui {

Color speakerColor(const std::wstring& speaker);

// --- журнал сообщений ------------------------------------------------------------
class MessageLog {
public:
    void push(const std::wstring& text, const std::wstring& speaker, Color c, float duration = 6.f);
    void clear() { entries_.clear(); }
    void update(float dt);
    void draw(Renderer& r, float bottom, bool narrow = false) const;  // narrow — сдвинуть влево (справа панель мыслей)

private:
    struct Entry {
        std::wstring speaker, text;
        Color color;
        float timer;
    };
    std::vector<Entry> entries_;
};

// --- HUD -----------------------------------------------------------------------------
class HUD {
public:
    static constexpr float SLOT_W = 168, SLOT_H = 58, SLOT_GAP = 10;
    float slotsTop() const { return cfg::SCREEN_H - SLOT_H - 14; }
    void draw(Renderer& r, const LevelDef& level, int levelCount, AssistantRoster& roster, const Room& room, const BrainClient& brain,
              float t) const;
};

void interactionLabel(Renderer& r, Vec2 pos, const std::wstring& label, Color c);

// --- диалоги -------------------------------------------------------------------------
class DialogPanel {
public:
    void start(const Dialog& lines);
    bool active() const { return has_; }
    void handleKey(Key k);
    void update(float dt);
    void draw(Renderer& r, float t) const;

private:
    void next();
    std::deque<std::pair<std::wstring, std::wstring>> queue_;
    std::pair<std::wstring, std::wstring> current_;
    bool has_ = false;
    float shown_ = 0.f;
};

// --- меню ------------------------------------------------------------------------------
class Menu {
public:
    Menu(std::wstring title, std::vector<std::pair<std::string, std::wstring>> options, std::wstring subtitle = L"");
    std::string handleKey(Key k);
    void draw(Renderer& r, float t, float top) const;
    int index = 0;

private:
    std::wstring title_, subtitle_;
    std::vector<std::pair<std::string, std::wstring>> options_;
};

// --- строка промпта --------------------------------------------------------------------
class PromptBox {
public:
    enum class Result { None, Submit, Cancel };
    void open(const std::wstring& assistantName, Color color, const std::wstring& hint, const std::wstring& description,
              const std::wstring& target);
    Result handle(const Input& input);
    const std::wstring& text() const { return text_; }
    void draw(Renderer& r, float t) const;

private:
    std::wstring name_, hint_, description_, target_, text_;
    Color color_;
    std::vector<std::wstring> history_;
    int historyPos_ = -1;
};

// --- мысли Помощника --------------------------------------------------------------------
struct Decision {
    std::vector<std::pair<std::wstring, Color>> notes;
    std::wstring verdict;
    Color verdictColor = cfg::TEXT;
    bool correctable = true;
};

class ThoughtPanel {
public:
    void show(const Thought& th, const Decision& d, const std::wstring& assistantName, Color c, const std::vector<int>& roomObjects);
    void hide() { visible_ = false; }
    bool visible() const { return visible_; }
    float age() const { return age_; }
    void update(float dt);
    void draw(Renderer& r, float t) const;

private:
    Thought th_;
    Decision d_;
    std::wstring name_;
    Color color_;
    std::vector<int> objects_;
    bool visible_ = false;
    float age_ = 0.f;
};

// --- исправление (обучение Помощника) ------------------------------------------------------
class CorrectionMenu {
public:
    void open(const Thought& th, const std::wstring& assistantName, Color c);
    enum class Result { None, Confirm, Cancel };
    Result handleKey(Key k);
    Intent intent() const { return {values_[0], values_[1], values_[2]}; }
    const Thought& thought() const { return th_; }
    void draw(Renderer& r, float t) const;

private:
    Thought th_;
    std::wstring name_;
    Color color_;
    int row_ = 0;
    int values_[3] = {0, 0, 0};
};

// --- лаборатория нейросетей ------------------------------------------------------------------
class LabScreen {
public:
    int selected = 1;
    bool confirmReset = false;
    void draw(Renderer& r, float t, const BrainClient& brain) const;
};

// --- экраны -----------------------------------------------------------------------------------
void menuBackground(Renderer& r, float t);
void title(Renderer& r, float t, Vec2 center);
void dim(Renderer& r, float alpha);
void levelBanner(Renderer& r, const LevelDef& level, float strength);
void redFlash(Renderer& r, float strength);
void crtOverlay(Renderer& r);
void howTo(Renderer& r, float t);

class UpgradeScreen {
public:
    void open(int level, const std::wstring& text) {
        level_ = level;
        text_ = text;
        timer_ = 0.f;
    }
    void update(float dt) { timer_ += dt; }
    bool handleKey(Key k) const;
    void draw(Renderer& r, float t, const std::wstring& footer) const;

private:
    int level_ = 1;
    std::wstring text_;
    float timer_ = 0.f;
};

class VictoryScreen {
public:
    void open() {
        timer_ = 0.f;
        scroll_ = 0.f;
    }
    void update(float dt);
    bool finished() const;
    bool handleKey(Key k);
    void draw(Renderer& r, float t) const;

private:
    bool textDone() const;
    float timer_ = 0.f, scroll_ = 0.f;
};

}  // namespace ui
