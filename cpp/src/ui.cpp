#include "ui.h"

#include <algorithm>

#include "assistant.h"
#include "audio.h"
#include "boss.h"
#include "brain_client.h"
#include "gfx.h"
#include "room.h"

namespace ui {

namespace {

const wchar_t* const ARCH_SHORT[6] = {L"", L"линейная модель", L"перцептрон · 24", L"перцептрон · 48", L"трансформер", L"трансформер ×2"};

bool confirmKey(Key k) { return k == K_ENTER || k == K_SPACE || k == K_E; }

void sparkline(Renderer& r, const RectF& box, const Json& values, Color c) {
    r.drawRect(box, c.scaled(0.35f), 1);
    if (values.size() < 2) return;
    float lo = 1e9f, hi = -1e9f;
    for (size_t i = 0; i < values.size(); ++i) {
        const float v = static_cast<float>(values[i].number());
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (hi - lo < 1e-6f) hi = lo + 1e-6f;
    std::vector<Vec2> pts;
    for (size_t i = 0; i < values.size(); ++i) {
        const float x = box.x + 3 + (box.w - 6) * static_cast<float>(i) / (values.size() - 1);
        const float y = box.bottom() - 3 - (box.h - 6) * (static_cast<float>(values[i].number()) - lo) / (hi - lo);
        pts.push_back({x, y});
    }
    r.polyline(pts, c, 2);
}

}  // namespace

Color speakerColor(const std::wstring& speaker) {
    for (int l = 1; l <= 5; ++l)
        if (speaker == ASSISTANT_NAMES[l]) return ASSISTANT_COLORS[l];
    if (speaker == L"СИСТЕМА") return cfg::SYSTEM;
    if (speaker == L"ОПЕРАТОР") return cfg::PLAYER;
    if (speaker == L"ВАЙБКОДЕР") return cfg::BOSS;
    if (speaker == L"???") return Color::rgb(200, 200, 255);
    if (speaker == L"НЕЙРОСЕТЬ") return cfg::THOUGHT;
    return cfg::TEXT;
}

// ==========================================================================
// Журнал
// ==========================================================================
void MessageLog::push(const std::wstring& text, const std::wstring& speaker, Color c, float duration) {
    if (!entries_.empty() && entries_.back().text == text && entries_.back().speaker == speaker) {
        entries_.back().timer = duration;
        return;
    }
    entries_.push_back({speaker, text, c, duration});
    if (entries_.size() > 8) entries_.erase(entries_.begin());
}

void MessageLog::update(float dt) {
    for (auto& e : entries_) e.timer -= dt;
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [](const Entry& e) { return e.timer <= 0; }), entries_.end());
}

void MessageLog::draw(Renderer& r, float bottom, bool narrow) const {
    const float WIDTH = narrow ? 620.f : 820.f;
    const float left = narrow ? 200.f : (cfg::SCREEN_W - WIDTH) / 2;
    float y = bottom;
    const size_t start = entries_.size() > 3 ? entries_.size() - 3 : 0;
    for (size_t i = entries_.size(); i-- > start;) {
        const Entry& e = entries_[i];
        const float fade = std::min(1.f, e.timer / 0.8f);
        const float textH = r.wrappedHeight(e.text, WIDTH - 28, 16);
        const RectF rect{left, y - (30 + textH), WIDTH, 30 + textH};
        gfx::panel(r, rect, e.color.scaled(0.6f * fade + 0.2f), 0.72f * fade, 1);
        r.text(e.speaker, {rect.x + 14, rect.y + 6}, 13, e.color.alpha(fade), Align::TopLeft, true);
        r.textWrapped(e.text, {rect.x + 14, rect.y + 24, WIDTH - 28, textH}, 16, cfg::TEXT.alpha(fade));
        y = rect.y - 6;
    }
}

// ==========================================================================
// HUD
// ==========================================================================
void HUD::draw(Renderer& r, const LevelDef& level, int levelCount, AssistantRoster& roster, const Room& room, const BrainClient& brain,
               float t) const {
    // Статус уровня и Помощника
    const RectF st{16, 16, 392, 150};
    gfx::panel(r, st, cfg::SYSTEM.scaled(0.8f));
    r.text(L"УРОВЕНЬ " + std::to_wstring(level.number) + L"/" + std::to_wstring(levelCount) + L" · " + level.title, {30, 26}, 16, cfg::ACCENT,
           Align::TopLeft, true);
    Assistant& a = roster.current();
    std::wstring status = L"не призван — нужен терминал";
    Color statusColor = cfg::WARNING;
    if (a.broken) {
        status = L"ВЕСА ИСПОРЧЕНЫ";
        statusColor = cfg::DANGER;
    } else if (roster.summoned) {
        status = a.staying ? L"ждёт" : L"в сети";
        statusColor = cfg::SUCCESS;
    }
    RectF lbl = r.text(L"Помощник:", {30, 52}, 16, cfg::DIM);
    RectF nm = r.text(a.name, {lbl.right() + 8, 52}, 16, a.color, Align::TopLeft, true);
    r.text(L"(" + status + L")", {nm.right() + 8, 53}, 14, statusColor);
    lbl = r.text(L"Интеллект:", {30, 78}, 16, cfg::DIM);
    for (int i = 0; i < 5; ++i) {
        const RectF pip{lbl.right() + 10 + i * 20.f, 81, 14, 12};
        if (i < a.level)
            r.fillRect(pip, a.color);
        else
            r.drawRect(pip, Color::rgb(60, 64, 90), 1);
    }
    r.text(std::to_wstring(a.level) + L"/5 · " + ASSISTANT_SHAPES[a.level], {lbl.right() + 116, 78}, 15, a.color);
    r.text(L"Задачи: " + std::to_wstring(room.solvedCount()) + L"/" + std::to_wstring(room.puzzles.size()) + L"   Перестроек: " +
               std::to_wstring(room.reshuffles),
           {30, 104}, 15, cfg::TEXT);
    if (room.recording() && static_cast<int>(t * 3) % 2 == 0) r.text(L"● REC", {st.right() - 16, 104}, 16, cfg::DANGER, Align::TopRight, true);
    std::wstring brainLine = L"Нейросеть: " + std::wstring(ARCH_SHORT[a.level]);
    Color brainColor = cfg::THOUGHT;
    if (brain.mode() == BrainClient::Mode::Fallback) {
        brainLine = L"Нейросети недоступны — ключевые слова";
        brainColor = cfg::WARNING;
    } else if (!brain.levelReady(a.level)) {
        brainLine += L" · обучается " + formatPercent(brain.levelProgress(a.level));
        brainColor = cfg::WARNING;
    } else {
        brainLine += L" · готова";
    }
    r.text(brainLine, {30, 130}, 13, brainColor);

    // Подсказки по управлению
    static const wchar_t* const HINTS[][2] = {{L"WASD", L"движение"},       {L"E", L"взаимодействие"},      {L"T / Enter", L"промпт"},
                                              {L"Tab", L"поправить мысль"}, {L"1-5", L"выбор Помощника"}, {L"ESC", L"пауза"}};
    const RectF hr{cfg::SCREEN_W - 16 - 250.f, 16, 250, 22 + 21 * 6.f};
    gfx::panel(r, hr, cfg::SYSTEM.scaled(0.5f), 0.66f);
    for (int i = 0; i < 6; ++i) {
        const RectF k = r.text(HINTS[i][0], {hr.x + 14, hr.y + 11 + i * 21.f}, 15, cfg::ACCENT, Align::TopLeft, true);
        r.text(std::wstring(L"— ") + HINTS[i][1], {k.right() + 6, hr.y + 11 + i * 21.f}, 15, cfg::DIM);
    }

    // Слоты Помощников
    const float total = 5 * SLOT_W + 4 * SLOT_GAP;
    const float x0 = (cfg::SCREEN_W - total) / 2, y = slotsTop();
    const int copied = room.boss ? room.boss->copied : 0;
    for (int level = 1; level <= 5; ++level) {
        const RectF rect{x0 + (level - 1) * (SLOT_W + SLOT_GAP), y, SLOT_W, SLOT_H};
        Assistant& as = roster.get(level);
        const bool unlocked = level <= roster.unlocked;
        const bool available = std::find(roster.available.begin(), roster.available.end(), level) != roster.available.end();
        const bool active = level == roster.selected;
        Color border = Color::rgb(48, 50, 70);
        float k = 0.3f;
        if (active) {
            border = as.color;
            k = 1.f;
            r.glow(rect.center(), 90, as.color, 0.35f);
        } else if (available) {
            border = as.color.scaled(0.5f);
            k = 0.8f;
        }
        gfx::panel(r, rect, border, 0.8f, active ? 3.f : 1.f);
        const RectF key{rect.x + 6, rect.y + 6, 18, 18};
        r.drawRect(key, border, 1);
        r.text(std::to_wstring(level), key.center(), 13, cfg::TEXT.scaled(k), Align::Center, true);
        const Vec2 icon{rect.x + 40, rect.y + 31};
        if (unlocked) {
            gfx::assistantIcon(r, level, icon, 10, as.color.scaled(k), active ? t * 0.8f : 0.f);
            r.text(as.name, {rect.x + 62, rect.y + 10}, 15, as.color.scaled(k), Align::TopLeft, active);
        } else {
            r.text(L"?", icon, 22, Color::rgb(70, 72, 96), Align::Center, true);
            r.text(L"???", {rect.x + 62, rect.y + 10}, 15, Color::rgb(70, 72, 96));
        }
        std::wstring state = L"недоступен";
        Color sc = Color::rgb(70, 72, 96);
        if (!unlocked) {
            state = L"закрыт";
        } else if (as.broken) {
            state = L"ERR: сломан";
            sc = cfg::DANGER;
        } else if (copied == level) {
            state = L"скопирован";
            sc = cfg::WARNING;
        } else if (active && roster.summoned) {
            state = as.thinking ? L"думает…" : L"активен";
            sc = as.thinking ? cfg::THOUGHT : cfg::SUCCESS;
        } else if (active) {
            state = L"выбран";
            sc = cfg::WARNING;
        } else if (available) {
            state = L"доступен";
            sc = cfg::DIM;
        }
        r.text(state, {rect.x + 62, rect.y + 32}, 13, sc);
        if (as.broken && static_cast<int>(t * 10) % 2 == 0)
            for (int i = 0; i < 2; ++i) r.fillRect({rect.x + rng().uniform(0, rect.w - 40), rect.y + rng().uniform(0, rect.h - 3), 40, 2}, cfg::DANGER);
    }

    // Босс
    if (room.boss) {
        const Vibecoder& boss = *room.boss;
        const RectF br{cfg::SCREEN_W / 2.f - 220, 14, 440, 66};
        const Color c = Color::hsv(t * 0.15f, 0.7f, 1.f);
        gfx::panel(r, br, c);
        r.text(L"ВАЙБКОДЕР", {br.x + 14, br.y + 8}, 16, c, Align::TopLeft, true);
        static const wchar_t* const PHASES[4] = {L"", L"ФАЗА 1 · ПУТАНИЦА УПРАВЛЕНИЯ", L"ФАЗА 2 · ПЕРЕВОРОТ МИРА", L"ФАЗА 3 · ПОДМЕНА ВЕСОВ"};
        r.text(boss.defeated ? L"ПЕРЕОБУЧЕН" : PHASES[boss.phase()], {br.right() - 14, br.y + 10}, 13, cfg::TEXT, Align::TopRight);
        const float segW = (br.w - 28 - 16) / 3.f;
        for (int i = 0; i < cfg::BOSS_MAX_HP; ++i) {
            const RectF seg{br.x + 14 + i * (segW + 8), br.y + 36, segW, 16};
            if (i < boss.hp) r.fillRect(seg, c.scaled(0.85f));
            r.drawRect(seg, c, 1);
        }
        const std::wstring s = boss.statusText();
        if (!s.empty()) r.text(s, {cfg::SCREEN_W / 2.f, br.bottom() + 8}, 15, cfg::WARNING, Align::TopCenter, true);
    }
}

void interactionLabel(Renderer& r, Vec2 pos, const std::wstring& label, Color c) {
    const std::wstring text = L"[E] " + label;
    const Vec2 size = r.measure(text, 15, true);
    RectF rect = RectF::centered({pos.x, pos.y - 30 - (size.y + 10) / 2}, size.x + 18, size.y + 10);
    rect.x = clampf(rect.x, 8, cfg::SCREEN_W - rect.w - 8);
    rect.y = clampf(rect.y, 8, cfg::SCREEN_H - rect.h - 8);
    gfx::panel(r, rect, c, 0.85f, 1);
    r.text(text, rect.center(), 15, cfg::TEXT, Align::Center, true);
}

// ==========================================================================
// Диалоги
// ==========================================================================
void DialogPanel::start(const Dialog& lines) {
    for (const auto& l : lines) queue_.push_back(l);
    if (!has_) next();
}

void DialogPanel::next() {
    has_ = !queue_.empty();
    if (has_) {
        current_ = queue_.front();
        queue_.pop_front();
    }
    shown_ = 0.f;
}

void DialogPanel::handleKey(Key k) {
    if (!has_) return;
    if (k == K_ESC) {
        queue_.clear();
        has_ = false;
        return;
    }
    if (confirmKey(k)) {
        if (shown_ < current_.second.size())
            shown_ = static_cast<float>(current_.second.size());
        else {
            audio::play("blip");
            next();
        }
    }
}

void DialogPanel::update(float dt) {
    if (has_) shown_ = std::min(static_cast<float>(current_.second.size()), shown_ + dt * 60.f);
}

void DialogPanel::draw(Renderer& r, float t) const {
    if (!has_) return;
    const Color c = speakerColor(current_.first);
    const RectF rect{cfg::SCREEN_W / 2.f - 470, cfg::SCREEN_H - 86.f - 168, 940, 168};
    gfx::panel(r, rect, c, 0.92f);
    r.glow({rect.x + 30, rect.y + 22}, 30, c, 0.6f);
    r.text(current_.first, {rect.x + 22, rect.y + 12}, 18, c, Align::TopLeft, true);
    r.textWrapped(current_.second.substr(0, static_cast<size_t>(shown_)), {rect.x + 22, rect.y + 44, rect.w - 44, 110}, 19, cfg::TEXT);
    if (shown_ >= current_.second.size() && static_cast<int>(t * 2) % 2 == 0)
        r.text(L"E / Enter — далее · Esc — пропустить", {rect.right() - 16, rect.bottom() - 10}, 13, cfg::DIM, Align::BottomRight);
}

// ==========================================================================
// Меню
// ==========================================================================
Menu::Menu(std::wstring title, std::vector<std::pair<std::string, std::wstring>> options, std::wstring subtitle)
    : title_(std::move(title)), subtitle_(std::move(subtitle)), options_(std::move(options)) {}

std::string Menu::handleKey(Key k) {
    const int n = static_cast<int>(options_.size());
    if (k == K_W || k == K_UP) {
        index = (index + n - 1) % n;
        audio::play("blip");
    } else if (k == K_S || k == K_DOWN) {
        index = (index + 1) % n;
        audio::play("blip");
    } else if (confirmKey(k)) {
        audio::play("select");
        return options_[static_cast<size_t>(index)].first;
    }
    return "";
}

void Menu::draw(Renderer& r, float t, float top) const {
    const float cx = cfg::SCREEN_W / 2.f;
    if (!title_.empty()) {
        r.glow({cx, top - 110}, 160, cfg::ACCENT, 0.35f);
        r.text(title_, {cx, top - 110}, 44, cfg::ACCENT, Align::Center, true);
    }
    if (!subtitle_.empty()) r.text(subtitle_, {cx, top - 66}, 17, cfg::DIM, Align::Center);
    for (size_t i = 0; i < options_.size(); ++i) {
        const float y = top + i * 46.f;
        if (static_cast<int>(i) == index) {
            const float pulse = 0.75f + 0.25f * std::sin(t * 6);
            gfx::panel(r, RectF::centered({cx, y}, 460, 38), cfg::ACCENT.scaled(pulse), 0.63f);
            r.text(L">  " + options_[i].second + L"  <", {cx, y}, 22, cfg::ACCENT, Align::Center, true);
        } else {
            r.text(options_[i].second, {cx, y}, 21, cfg::DIM, Align::Center);
        }
    }
}

// ==========================================================================
// Строка промпта
// ==========================================================================
void PromptBox::open(const std::wstring& assistantName, Color color, const std::wstring& hint, const std::wstring& description,
                     const std::wstring& target) {
    name_ = assistantName;
    color_ = color;
    hint_ = hint;
    description_ = description;
    target_ = target;
    text_.clear();
    historyPos_ = -1;
}

PromptBox::Result PromptBox::handle(const Input& input) {
    for (wchar_t c : input.chars())
        if (text_.size() < 120) text_.push_back(c);
    for (Key k : input.pressed()) {
        switch (k) {
            case K_BACKSPACE:
                if (!text_.empty()) text_.pop_back();
                break;
            case K_ENTER: {
                std::wstring trimmed = text_;
                while (!trimmed.empty() && trimmed.back() == L' ') trimmed.pop_back();
                if (trimmed.empty()) break;
                if (history_.empty() || history_.back() != trimmed) history_.push_back(trimmed);
                text_ = trimmed;
                return Result::Submit;
            }
            case K_ESC: return Result::Cancel;
            case K_UP:
                if (!history_.empty()) {
                    historyPos_ = historyPos_ < 0 ? static_cast<int>(history_.size()) - 1 : std::max(0, historyPos_ - 1);
                    text_ = history_[static_cast<size_t>(historyPos_)];
                }
                break;
            case K_DOWN:
                if (historyPos_ >= 0) {
                    ++historyPos_;
                    if (historyPos_ >= static_cast<int>(history_.size())) {
                        historyPos_ = -1;
                        text_.clear();
                    } else {
                        text_ = history_[static_cast<size_t>(historyPos_)];
                    }
                }
                break;
            default: break;
        }
    }
    return Result::None;
}

void PromptBox::draw(Renderer& r, float t) const {
    float bottom = cfg::SCREEN_H - 92.f;
    const float w = 900, x = (cfg::SCREEN_W - w) / 2;
    const RectF box{x, bottom - 128, w, 128};
    if (!description_.empty()) {
        const float h = r.wrappedHeight(description_, w - 40, 17) + 44;
        const RectF d{x, box.y - h - 10, w, h};
        gfx::panel(r, d, cfg::SYSTEM, 0.92f);
        r.text(L"КОНСОЛЬ КОНТЕКСТА", {d.x + 20, d.y + 10}, 14, cfg::ACCENT, Align::TopLeft, true);
        r.textWrapped(description_, {d.x + 20, d.y + 32, w - 40, h}, 17, cfg::TEXT);
    }
    gfx::panel(r, box, color_, 0.94f, 2);
    r.text(L"ПРОМПТ → " + name_, {box.x + 20, box.y + 12}, 15, color_, Align::TopLeft, true);
    if (!target_.empty()) r.text(L"цель указания: " + target_, {box.right() - 20, box.y + 13}, 14, cfg::DIM, Align::TopRight);
    const RectF field{box.x + 20, box.y + 40, w - 40, 38};
    r.fillRect(field, Color::rgb(4, 4, 12, 0.9f));
    r.drawRect(field, color_.scaled(0.6f), 1);
    const RectF tr = r.text(text_.empty() ? L" " : text_, {field.x + 12, field.y + field.h / 2}, 20, cfg::TEXT, Align::MidLeft);
    if (static_cast<int>(t * 2) % 2 == 0) r.fillRect({text_.empty() ? field.x + 12 : tr.right() + 2, field.y + 8, 10, field.h - 16}, color_);
    if (text_.empty()) r.text(hint_, {field.x + 30, field.y + field.h / 2}, 16, cfg::DIM, Align::MidLeft);
    r.text(L"Enter — отправить · Esc — отмена · ↑ — прошлый промпт · пиши как угодно: нейросеть разберётся", {box.x + 20, box.bottom() - 16}, 13,
           cfg::DIM, Align::MidLeft);
}

// ==========================================================================
// Мысли Помощника
// ==========================================================================
void ThoughtPanel::show(const Thought& th, const Decision& d, const std::wstring& name, Color c, const std::vector<int>& objects) {
    th_ = th;
    d_ = d;
    name_ = name;
    color_ = c;
    objects_ = objects;
    visible_ = true;
    age_ = 0.f;
}

void ThoughtPanel::update(float dt) {
    if (visible_) age_ += dt;
    if (age_ > 18.f) visible_ = false;
}

void ThoughtPanel::draw(Renderer& r, float t) const {
    if (!visible_) return;
    const float w = 410, x = cfg::SCREEN_W - 16 - w, top = 180;
    const float fade = std::min(1.f, (18.f - age_) / 1.5f);
    float y = top + 12;
    int line = 0;
    auto visible = [&](int i) { return age_ > i * 0.09f; };

    const float bgH = 430;
    gfx::panel(r, {x, top, w, bgH + static_cast<float>(d_.notes.size()) * 18}, cfg::THOUGHT.scaled(0.8f * fade), 0.9f * fade);
    r.text(L"МЫСЛИ · " + name_, {x + 14, y}, 15, color_.alpha(fade), Align::TopLeft, true);
    std::wstring arch = th_.fallback ? L"ключевые слова" : (th_.level >= 1 && th_.level <= 5 ? ARCH_SHORT[th_.level] : L"");
    r.text(arch, {x + w - 14, y + 2}, 12, cfg::THOUGHT.alpha(fade), Align::TopRight);
    y += 24;
    if (visible(line++)) r.textWrapped(L"«" + th_.text + L"»", {x + 14, y, w - 28, 40}, 15, cfg::PLAYER.alpha(fade), true);
    y += std::min(40.f, r.wrappedHeight(L"«" + th_.text + L"»", w - 28, 15, true)) + 8;

    // Слова и их вклад
    if (visible(line++)) {
        r.text(L"Слова (вклад в решение):", {x + 14, y}, 12, cfg::DIM.alpha(fade));
        float wx = x + 14, wy = y + 16;
        for (size_t i = 0; i < th_.tokens.size(); ++i) {
            const float imp = i < th_.importance.size() ? th_.importance[i] : 0.f;
            const bool known = i < th_.known.size() ? th_.known[i] : true;
            std::wstring word = th_.tokens[i] + (known ? L"" : L"?");
            const Vec2 sz = r.measure(word, 15, imp > 0.25f);
            if (wx + sz.x > x + w - 14) {
                wx = x + 14;
                wy += 20;
            }
            Color c = known ? Color::mix(cfg::DIM, cfg::ACCENT, std::min(1.f, imp * 2.2f)) : cfg::DANGER;
            if (imp > 0.05f) r.fillRect({wx - 2, wy + sz.y - 2, (sz.x + 4) * std::min(1.f, imp * 2.f + 0.1f), 2}, c.alpha(fade));
            r.text(word, {wx, wy}, 15, c.alpha(fade), Align::TopLeft, imp > 0.25f);
            wx += sz.x + 8;
        }
        y = wy + 26;
    } else {
        y += 46;
    }

    // Внимание трансформера
    if (!th_.attention.empty()) {
        if (visible(line++)) {
            r.text(L"Внимание (self-attention):", {x + 14, y}, 12, cfg::DIM.alpha(fade));
            std::vector<size_t> idx(th_.attention.size());
            for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
            std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return th_.attention[a] > th_.attention[b]; });
            float bx = x + 14;
            for (size_t k = 0; k < std::min<size_t>(4, idx.size()); ++k) {
                const size_t i = idx[k];
                if (i >= th_.tokens.size()) continue;
                const std::wstring label = th_.tokens[i] + L" " + formatPercent(th_.attention[i]);
                const Vec2 sz = r.measure(label, 13);
                r.fillRect({bx, y + 16, sz.x + 8, 18}, cfg::THOUGHT.scaled(0.25f + th_.attention[i]).alpha(fade));
                r.text(label, {bx + 4, y + 17}, 13, cfg::TEXT.alpha(fade));
                bx += sz.x + 14;
                if (bx > x + w - 60) break;
            }
        }
        y += 42;
    }

    // Гипотезы
    auto hypotheses = [&](const wchar_t* label, const std::vector<std::pair<int, float>>& top, const wchar_t* (*name)(int), bool objectsRow) {
        if (!visible(line++)) {
            y += 20 + 18 * static_cast<float>(std::min<size_t>(3, top.size()));
            return;
        }
        r.text(label, {x + 14, y}, 12, cfg::DIM.alpha(fade));
        y += 16;
        for (size_t i = 0; i < std::min<size_t>(3, top.size()); ++i) {
            const auto [id, p] = top[i];
            std::wstring text = name(id);
            if (objectsRow && id != 0) {
                const bool present = std::find(objects_.begin(), objects_.end(), id) != objects_.end();
                text += present ? L" ✓" : L" ✗ нет в комнате";
            }
            const Color c = i == 0 ? cfg::TEXT : cfg::DIM;
            r.fillRect({x + 14, y + 3, 120 * p, 12}, cfg::THOUGHT.scaled(i == 0 ? 0.8f : 0.4f).alpha(fade));
            r.text(formatPercent(p), {x + 140, y}, 13, c.alpha(fade));
            r.text(text, {x + 186, y}, 13, c.alpha(fade), Align::TopLeft, i == 0);
            y += 18;
        }
        y += 4;
    };
    hypotheses(L"Действие:", th_.topAction, actionName, false);
    hypotheses(L"Объект:", th_.topObject, objectName, true);
    hypotheses(L"Цель:", th_.topGoal, goalName, false);

    // Выводы игры и решение
    for (const auto& [note, c] : d_.notes) {
        if (visible(line++)) r.textWrapped(L"• " + note, {x + 14, y, w - 28, 40}, 13, c.alpha(fade));
        y += r.wrappedHeight(L"• " + note, w - 28, 13) + 2;
    }
    if (visible(line++)) {
        r.text(L"Уверенность: " + formatPercent(th_.confidence) + L" (порог " + formatPercent(th_.floor) + L")", {x + 14, y}, 13,
               (th_.confidence >= th_.floor ? cfg::SUCCESS : cfg::WARNING).alpha(fade));
        y += 18;
        r.textWrapped(d_.verdict, {x + 14, y, w - 28, 60}, 15, d_.verdictColor.alpha(fade), true);
        y += r.wrappedHeight(d_.verdict, w - 28, 15, true) + 6;
    }
    if (th_.corrupted) {
        r.text(L"ВЕСА ИСПОРЧЕНЫ ВАЙБКОДЕРОМ", {x + 14, y}, 13, cfg::DANGER.alpha(fade), Align::TopLeft, true);
        y += 18;
    }
    if (th_.fallback && !th_.fallbackReason.empty()) {
        r.textWrapped(L"Нейросеть недоступна (" + th_.fallbackReason + L") — разбор по ключевым словам.", {x + 14, y, w - 28, 40}, 12,
                      cfg::WARNING.alpha(fade));
        y += 30;
    }
    if (d_.correctable && static_cast<int>(t * 2) % 2 == 0 && visible(line))
        r.text(L"Tab — поправить: Помощник дообучится", {x + 14, std::min(y + 2, top + bgH - 20)}, 13, cfg::ACCENT.alpha(fade), Align::TopLeft, true);
}

// ==========================================================================
// Исправление
// ==========================================================================
void CorrectionMenu::open(const Thought& th, const std::wstring& name, Color c) {
    th_ = th;
    name_ = name;
    color_ = c;
    row_ = 0;
    values_[0] = th.intent.action;
    values_[1] = th.intent.object;
    values_[2] = th.intent.goal;
}

CorrectionMenu::Result CorrectionMenu::handleKey(Key k) {
    const int counts[3] = {A_COUNT, O_COUNT, G_COUNT};
    switch (k) {
        case K_ESC: return Result::Cancel;
        case K_ENTER: return Result::Confirm;
        case K_W:
        case K_UP: row_ = (row_ + 2) % 3; audio::play("blip"); break;
        case K_S:
        case K_DOWN: row_ = (row_ + 1) % 3; audio::play("blip"); break;
        case K_A:
        case K_LEFT: values_[row_] = (values_[row_] + counts[row_] - 1) % counts[row_]; audio::play("mark"); break;
        case K_D:
        case K_RIGHT: values_[row_] = (values_[row_] + 1) % counts[row_]; audio::play("mark"); break;
        default: break;
    }
    return Result::None;
}

void CorrectionMenu::draw(Renderer& r, float t) const {
    dim(r, 0.6f);
    const RectF box = RectF::centered(cfg::SCREEN_CENTER, 820, 360);
    gfx::panel(r, box, cfg::THOUGHT, 0.96f);
    r.text(L"ОБУЧЕНИЕ · " + name_, {box.x + 28, box.y + 20}, 24, color_, Align::TopLeft, true);
    r.text(L"Объясни, что ты имел в виду. Нейросеть дообучится на этом примере.", {box.x + 28, box.y + 56}, 15, cfg::DIM);
    r.text(L"«" + th_.text + L"»", {box.x + 28, box.y + 84}, 18, cfg::PLAYER, Align::TopLeft, true);
    const wchar_t* labels[3] = {L"ДЕЙСТВИЕ", L"ОБЪЕКТ", L"ЦЕЛЬ"};
    for (int i = 0; i < 3; ++i) {
        const RectF row{box.x + 24, box.y + 124 + i * 52.f, box.w - 48, 44};
        const bool sel = i == row_;
        if (sel) gfx::panel(r, row, cfg::ACCENT, 0.45f, 1);
        r.text(labels[i], {row.x + 16, row.y + row.h / 2}, 15, sel ? cfg::ACCENT : cfg::DIM, Align::MidLeft, true);
        const std::wstring value = i == 0 ? actionName(values_[0]) : i == 1 ? objectName(values_[1]) : goalName(values_[2]);
        const Color arrows = sel ? cfg::ACCENT : Color::rgb(70, 72, 96);
        r.text(L"<", {row.x + 240, row.y + row.h / 2}, 20, arrows, Align::Center, true);
        r.text(value, {row.x + 470, row.y + row.h / 2}, 19, sel ? cfg::TEXT : cfg::DIM, Align::Center, sel);
        r.text(L">", {row.x + 700, row.y + row.h / 2}, 20, arrows, Align::Center, true);
    }
    r.text(L"Значит: «" + intentPhrase(intent()) + L"»" + (static_cast<int>(t * 2) % 2 ? L"_" : L" "), {box.x + 28, box.y + 290}, 17, cfg::ACCENT,
           Align::TopLeft, true);
    r.text(L"W/S — строка · A/D — вариант · Enter — научить и выполнить · Esc — отмена", {box.center().x, box.bottom() - 22}, 14, cfg::DIM,
           Align::Center);
}

// ==========================================================================
// Лаборатория нейросетей
// ==========================================================================
void LabScreen::draw(Renderer& r, float t, const BrainClient& brain) const {
    menuBackground(r, t);
    dim(r, 0.55f);
    r.text(L"НЕЙРОСЕТИ ПОМОЩНИКОВ", {cfg::SCREEN_W / 2.f, 44}, 34, cfg::ACCENT, Align::Center, true);
    r.text(brain.modeText(), {cfg::SCREEN_W / 2.f, 78}, 15, brain.online() ? cfg::THOUGHT : cfg::WARNING, Align::Center);
    const Json& st = brain.status();
    for (int level = 1; level <= 5; ++level) {
        const RectF row{70, 100 + (level - 1) * 108.f, cfg::SCREEN_W - 140.f, 100};
        const Color c = ASSISTANT_COLORS[level];
        const bool sel = level == selected;
        gfx::panel(r, row, sel ? c : c.scaled(0.4f), 0.82f, sel ? 2.f : 1.f);
        gfx::assistantIcon(r, level, {row.x + 36, row.y + 50}, 16, c, t * 0.6f);
        r.text(ASSISTANT_NAMES[level], {row.x + 70, row.y + 10}, 19, c, Align::TopLeft, true);
        const Json& m = st[std::to_string(level)];
        const bool ready = brain.levelReady(level);
        if (m.isNull()) {
            r.text(brain.online() ? (ready ? L"данные загружаются…" : L"обучается: " + formatPercent(brain.levelProgress(level)))
                                  : L"нет связи с нейросетями",
                   {row.x + 70, row.y + 40}, 15, cfg::DIM);
            continue;
        }
        r.text(m["arch"].wstring(), {row.x + 70, row.y + 38}, 14, cfg::TEXT);
        r.text(L"параметров: " + std::to_wstring(m["params"].integer()) + L" · корпус: " + std::to_wstring(m["corpus"].integer()) +
                   L" фраз · словарь: " + std::to_wstring(m["vocab"].integer()),
               {row.x + 70, row.y + 58}, 13, cfg::DIM);
        const Json& acc = m["acc"];
        r.text(L"точность на новых фразах: действие " + formatPercent(static_cast<float>(acc[0].number())) + L" · объект " +
                   formatPercent(static_cast<float>(acc[1].number())) + L" · цель " + formatPercent(static_cast<float>(acc[2].number())),
               {row.x + 70, row.y + 76}, 13, cfg::SUCCESS);
        const float colX = row.x + 640;
        if (!ready) {
            const float p = brain.levelProgress(level);
            r.text(L"обучение " + formatPercent(p), {colX, row.y + 12}, 14, cfg::WARNING, Align::TopLeft, true);
            r.drawRect({colX, row.y + 34, 200, 12}, cfg::WARNING, 1);
            r.fillRect({colX, row.y + 34, 200 * p, 12}, cfg::WARNING);
        } else {
            r.text(L"выучено от тебя: " + std::to_wstring(m["memory"].integer()) + L" фраз", {colX, row.y + 12}, 14, cfg::THOUGHT, Align::TopLeft,
                   true);
            r.text(L"исправлений " + std::to_wstring(m["corrections"].integer()) + L" · успехов " + std::to_wstring(m["reinforcements"].integer()),
                   {colX, row.y + 32}, 13, cfg::DIM);
            if (m["corrupted"].boolean()) r.text(L"ВЕСА ИСПОРЧЕНЫ", {colX, row.y + 50}, 13, cfg::DANGER, Align::TopLeft, true);
        }
        r.text(L"loss предобучения", {row.right() - 290, row.y + 8}, 11, cfg::DIM);
        sparkline(r, {row.right() - 290, row.y + 24, 130, 56}, m["loss_history"], c);
        r.text(L"loss дообучения", {row.right() - 148, row.y + 8}, 11, cfg::DIM);
        sparkline(r, {row.right() - 148, row.y + 24, 130, 56}, m["online_loss"], cfg::THOUGHT);
    }
    const std::wstring footer = confirmReset ? L"Нажми R ещё раз, чтобы стереть память «" + std::wstring(ASSISTANT_NAMES[selected]) +
                                                   L"» и обучить заново · Esc — отмена"
                                             : L"W/S — модель · R — сбросить и обучить заново · Esc — назад";
    r.text(footer, {cfg::SCREEN_W / 2.f, cfg::SCREEN_H - 34.f}, 15, confirmReset ? cfg::DANGER : cfg::DIM, Align::Center, true);
}

// ==========================================================================
// Экраны
// ==========================================================================
void menuBackground(Renderer& r, float t) {
    r.clear(cfg::BG);
    const float off = std::fmod(t * 18, 48.f);
    for (float x = -48; x < cfg::SCREEN_W + 48; x += 48) r.line({x + off, 0}, {x + off, cfg::SCREEN_H}, Color::rgb(22, 24, 40), 1);
    for (float y = -48; y < cfg::SCREEN_H + 48; y += 48) r.line({0, y + off}, {cfg::SCREEN_W, y + off}, Color::rgb(22, 24, 40), 1);
    for (int i = 0; i < 15; ++i) {
        const int level = i % 5 + 1;
        const float speed = 14.f + (i % 4) * 9.f;
        const float x = std::fmod(i * 173.f + t * speed, cfg::SCREEN_W + 120.f) - 60;
        const float y = std::fmod(i * 97.f + 40, static_cast<float>(cfg::SCREEN_H)) + std::sin(t * 0.7f + i) * 26;
        const Color c = ASSISTANT_COLORS[level].scaled(0.45f);
        r.glow({x, y}, 40, c, 0.5f);
        gfx::assistantIcon(r, level, {x, y}, 10.f + (i % 3) * 4, c, t * (0.3f + 0.07f * i));
    }
}

void title(Renderer& r, float t, Vec2 c) {
    const float shift = 3.f + (rng().chance(0.06f) ? 6.f : 0.f);
    r.glow(c, 220, cfg::ACCENT, 0.5f);
    r.text(L"ПРОМПТ", c + Vec2{-shift, 0}, 104, Color::rgb(255, 0, 110, 0.75f), Align::Center, true);
    r.text(L"ПРОМПТ", c + Vec2{shift, 0}, 104, Color::rgb(0, 200, 255, 0.75f), Align::Center, true);
    r.text(L"ПРОМПТ", c, 104, Color::rgb(235, 240, 255), Align::Center, true);
    if (rng().chance(0.15f)) r.fillRect({c.x - 260 + rng().uniform(-20, 20), c.y + rng().uniform(-40, 40), 520, rng().uniform(2, 6)}, cfg::BG);
}

void dim(Renderer& r, float alpha) { r.fillRect({0, 0, cfg::SCREEN_W, cfg::SCREEN_H}, Color::rgb(0, 0, 0, alpha)); }

void levelBanner(Renderer& r, const LevelDef& level, float strength) {
    if (strength <= 0) return;
    const float k = std::min(1.f, strength);
    r.text(L"УРОВЕНЬ " + std::to_wstring(level.number), {cfg::SCREEN_W / 2.f, 200}, 22, cfg::DIM.alpha(k), Align::Center, true);
    r.text(level.title, {cfg::SCREEN_W / 2.f, 242}, 46, cfg::ACCENT.alpha(k), Align::Center, true);
    r.text(level.subtitle, {cfg::SCREEN_W / 2.f, 286}, 18, cfg::TEXT.alpha(k), Align::Center);
}

void redFlash(Renderer& r, float strength) {
    if (strength > 0) r.fillRect({0, 0, cfg::SCREEN_W, cfg::SCREEN_H}, Color::rgb(255, 0, 40, 0.43f * std::min(1.f, strength)));
}

void crtOverlay(Renderer& r) {
    for (float y = 0; y < cfg::SCREEN_H; y += 3) r.line({0, y}, {cfg::SCREEN_W, y}, Color::rgb(0, 0, 0, 0.13f), 1);
    for (int i = 0; i < 30; ++i) {
        const float a = 0.35f * std::pow(1.f - i / 30.f, 2.f);
        r.drawRect({i * 4.f, i * 3.f, cfg::SCREEN_W - i * 8.f, cfg::SCREEN_H - i * 6.f}, Color::rgb(0, 0, 0, a), 4);
    }
}

void howTo(Renderer& r, float t) {
    menuBackground(r, t);
    dim(r, 0.6f);
    r.text(L"КАК ИГРАТЬ", {cfg::SCREEN_W / 2.f, 64}, 40, cfg::ACCENT, Align::Center, true);
    static const wchar_t* const ITEMS[][2] = {
        {L"Цель", L"Пройти восемь комнат Песочницы и переобучить босса-Вайбкодера."},
        {L"Ты — проекция", L"Оператор не двигает предметы. Ты пишешь задачи, Помощники их выполняют."},
        {L"Промпт", L"E у объекта или T в любом месте — открыть строку и написать задачу своими словами: «подвинь коробку на плиту»."},
        {L"Нейросети", L"Каждый Помощник — своя нейросеть (линейная, перцептрон, трансформер). Справа видно, как она рассуждает."},
        {L"Обучение", L"Понял не так — Tab: укажи, что ты имел в виду. Модель дообучится и запомнит это навсегда."},
        {L"Песочница", L"После каждого провала объекты перестраиваются: ящики, кнопки, мины, ключи и выключатели меняют места."},
        {L"Стелс", L"Стражи видят жёлтым конусом. Мины видны только вблизи. Тени боятся света."},
    };
    float y = 120;
    for (const auto& item : ITEMS) {
        r.text(item[0], {140, y}, 17, cfg::WARNING, Align::TopLeft, true);
        y += 24;
        y += r.textWrapped(item[1], {140, y, 1000, 80}, 17, cfg::TEXT) + 16;
    }
    r.text(L"Esc / Enter — назад", {cfg::SCREEN_W / 2.f, cfg::SCREEN_H - 36.f}, 16, cfg::DIM, Align::Center);
}

bool UpgradeScreen::handleKey(Key k) const { return timer_ > 0.8f && confirmKey(k); }

void UpgradeScreen::draw(Renderer& r, float t, const std::wstring& footer) const {
    menuBackground(r, t);
    dim(r, 0.55f);
    const Color c = ASSISTANT_COLORS[level_];
    const float grow = std::min(1.f, timer_ / 0.6f);
    const Vec2 center{cfg::SCREEN_W / 2.f, 250};
    r.text(L"АПГРЕЙД ПОМОЩНИКА", {cfg::SCREEN_W / 2.f, 90}, 34, cfg::ACCENT, Align::Center, true);
    for (int i = 0; i < 3; ++i) r.drawCircle(center, (60 + i * 26 + std::fmod(timer_ * 60, 26.f)) * grow, c.scaled(0.5f - i * 0.12f), 1);
    r.glow(center, 150 * grow, c, 1.2f);
    gfx::assistantIcon(r, level_, center, 46 * grow, c, t * 0.9f, 3);
    r.text(ASSISTANT_NAMES[level_], {cfg::SCREEN_W / 2.f, 380}, 42, c, Align::Center, true);
    r.text(L"Интеллект: " + std::to_wstring(level_) + L"/5 · форма: " + ASSISTANT_SHAPES[level_], {cfg::SCREEN_W / 2.f, 426}, 18, cfg::TEXT,
           Align::Center);
    r.textWrapped(ASSISTANT_DESCRIPTIONS[level_], {cfg::SCREEN_W / 2.f - 450, 450, 900, 40}, 18, cfg::DIM, false, Align::TopCenter);
    r.textWrapped(text_, {cfg::SCREEN_W / 2.f - 450, 500, 900, 60}, 17, cfg::WARNING, false, Align::TopCenter);
    r.text(footer, {cfg::SCREEN_W / 2.f, 580}, 15, cfg::THOUGHT, Align::Center);
    if (timer_ > 0.8f && static_cast<int>(t * 2) % 2 == 0)
        r.text(L"Enter — следующий уровень", {cfg::SCREEN_W / 2.f, cfg::SCREEN_H - 50.f}, 17, cfg::ACCENT, Align::Center, true);
}

bool VictoryScreen::textDone() const { return timer_ * 26.f >= finalText().size(); }

void VictoryScreen::update(float dt) {
    timer_ += dt;
    if (textDone() && timer_ > finalText().size() / 26.f + 2.5f) scroll_ += dt * 46.f;
}

bool VictoryScreen::finished() const { return scroll_ > cfg::SCREEN_H + credits().size() * 34.f; }

bool VictoryScreen::handleKey(Key k) {
    if (!confirmKey(k) && k != K_ESC) return false;
    if (!textDone()) {
        timer_ = finalText().size() / 26.f;
        return false;
    }
    if (scroll_ <= 0) {
        timer_ = finalText().size() / 26.f + 2.5f;
        scroll_ = 1.f;
        return false;
    }
    return true;
}

void VictoryScreen::draw(Renderer& r, float t) const {
    menuBackground(r, t);
    dim(r, 0.45f);
    const std::wstring shown = finalText().substr(0, std::min(finalText().size(), static_cast<size_t>(timer_ * 26.f)));
    const float ty = 280 - std::min(scroll_, 220.f);
    r.glow({cfg::SCREEN_W / 2.f, ty + 30}, 300, cfg::ACCENT, 0.3f);
    r.textWrapped(shown, {140, ty, cfg::SCREEN_W - 280.f, 120}, 32, cfg::ACCENT, true, Align::TopCenter);
    if (scroll_ > 0) {
        float y = cfg::SCREEN_H + 20.f - scroll_;
        for (const auto& line : credits()) {
            if (y > -40 && y < cfg::SCREEN_H + 40) {
                bool big = !line.empty() && line.size() < 30;
                for (wchar_t ch : line)
                    if (ch >= 0x0430 && ch <= 0x044F) big = false;  // строчные буквы — обычная строка
                r.text(line, {cfg::SCREEN_W / 2.f, y}, big ? 24.f : 18.f, big ? cfg::WARNING : cfg::TEXT, Align::Center, big);
            }
            y += 34;
        }
    }
    if (textDone())
        r.text(scroll_ > 0 ? L"Enter — в главное меню" : L"Enter — продолжить", {cfg::SCREEN_W - 24.f, cfg::SCREEN_H - 24.f}, 14, cfg::DIM,
               Align::BottomRight);
}

}  // namespace ui
