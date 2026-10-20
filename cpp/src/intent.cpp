#include "intent.h"

#include <algorithm>

namespace {

const wchar_t* const ACTION_NAMES[A_COUNT] = {L"НЕ ПОНЯЛ", L"ТОЛКНУТЬ", L"НАЖАТЬ", L"ПРОЙТИ МАРШРУТ", L"СВЯЗАТЬ", L"ВКЛЮЧИТЬ",
                                              L"ВЫКЛЮЧИТЬ", L"ОБЪЯСНИТЬ", L"СЛОМАТЬ", L"ПОДОЙТИ", L"ЖДАТЬ"};
const wchar_t* const OBJECT_NAMES[O_COUNT] = {L"—", L"ЯЩИК", L"КНОПКИ", L"МАРШРУТ", L"КЛЮЧ И ЗАМОК", L"ВЕНТИЛЯТОР",
                                              L"СВЕТ", L"ТЕНЬ", L"ДВЕРЬ", L"ВАЙБКОДЕР", L"ОПЕРАТОР"};
const wchar_t* const GOAL_NAMES[G_COUNT] = {L"НЕ НАЗВАНА", L"ПРОЙТИ", L"УБРАТЬ ВРАГА", L"НАУЧИТЬ ПОНИМАТЬ", L"БЕЗ ПОЛЬЗЫ"};

const wchar_t* const ACTION_VERBS[A_COUNT] = {L"ничего не делать", L"толкнуть", L"нажать", L"пройти", L"связать", L"включить",
                                              L"выключить", L"объяснить", L"сломать", L"подойти", L"ждать"};
const wchar_t* const OBJECT_FORMS[O_COUNT] = {L"", L"ящик", L"кнопки", L"маршрут", L"ключ с замком", L"вентилятор",
                                              L"свет", L"тень", L"дверь", L"Вайбкодеру", L"к Оператору"};
const wchar_t* const GOAL_FORMS[G_COUNT] = {L"", L"чтобы пройти", L"чтобы враг исчез", L"чтобы он понимал, что пишет", L"просто так"};

struct Keyword {
    const wchar_t* stem;
    int value;
};

// Основы слов для запасного разбора (сравнение по началу слова).
const Keyword ACTION_KEYS[] = {
    {L"толк", A_PUSH},   {L"подвин", A_PUSH},  {L"задвин", A_PUSH},  {L"пих", A_PUSH},     {L"сдвин", A_PUSH},   {L"передвин", A_PUSH},
    {L"нажм", A_PRESS},  {L"нажа", A_PRESS},   {L"жми", A_PRESS},    {L"тыкн", A_PRESS},   {L"повтор", A_ROUTE}, {L"пройди", A_ROUTE},
    {L"следуй", A_ROUTE}, {L"свяж", A_LINK},   {L"соедин", A_LINK},  {L"подключ", A_LINK}, {L"привяж", A_LINK},  {L"выключ", A_OFF},
    {L"отключ", A_OFF},  {L"выруб", A_OFF},    {L"останов", A_OFF},  {L"погас", A_OFF},    {L"включ", A_ON},     {L"зажг", A_ON},
    {L"вруб", A_ON},     {L"запуст", A_ON},    {L"объясн", A_EXPLAIN}, {L"научи", A_EXPLAIN}, {L"растолк", A_EXPLAIN},
    {L"слома", A_BREAK}, {L"разбе", A_BREAK},  {L"взорв", A_BREAK},  {L"ко", A_COME},      {L"сюда", A_COME},    {L"подойд", A_COME},
    {L"стой", A_STAY},   {L"жди", A_STAY},     {L"замри", A_STAY},   {L"подожд", A_STAY},
};
const Keyword OBJECT_KEYS[] = {
    {L"ящик", O_BOX},       {L"короб", O_BOX},     {L"груз", O_BOX},       {L"куб", O_BOX},      {L"кноп", O_BUTTONS},
    {L"клавиш", O_BUTTONS}, {L"маршрут", O_ROUTE}, {L"пут", O_ROUTE},      {L"дорог", O_ROUTE},  {L"след", O_ROUTE},
    {L"ключ", O_KEY},       {L"замк", O_KEY},      {L"замок", O_KEY},      {L"вентил", O_FAN},   {L"кулер", O_FAN},
    {L"пропел", O_FAN},     {L"ветер", O_FAN},     {L"свет", O_LIGHT},     {L"ламп", O_LIGHT},   {L"освещ", O_LIGHT},
    {L"тень", O_SHADOW},    {L"врага", O_SHADOW},  {L"двер", O_DOOR},      {L"выход", O_DOOR},   {L"вайбкод", O_BOSS},
    {L"босс", O_BOSS},      {L"мне", O_ME},
};
const Keyword GOAL_KEYS[] = {
    {L"пройт", G_PASS},       {L"проход", G_PASS},       {L"дальше", G_PASS},     {L"враг", G_ENEMY},     {L"тень", G_ENEMY},
    {L"исчез", G_ENEMY},      {L"прогна", G_ENEMY},      {L"понима", G_UNDERSTAND}, {L"понял", G_UNDERSTAND},
    {L"научил", G_UNDERSTAND}, {L"разобр", G_UNDERSTAND}, {L"просто", G_BAD},     {L"быстр", G_BAD},      {L"весел", G_BAD},
    {L"шутк", G_BAD},         {L"громче", G_BAD},        {L"темнее", G_BAD},      {L"заметил", G_BAD},
};

bool startsWith(const std::wstring& word, const wchar_t* stem) { return word.rfind(stem, 0) == 0; }

bool inCurriculum(int level, int action) {
    switch (action) {
        case A_ROUTE: return level >= 2;
        case A_LINK: return level >= 3;
        case A_ON:
        case A_OFF:
        case A_EXPLAIN: return level >= 4;
        default: return true;
    }
}

}  // namespace

const wchar_t* actionName(int a) { return ACTION_NAMES[std::clamp(a, 0, A_COUNT - 1)]; }
const wchar_t* objectName(int o) { return OBJECT_NAMES[std::clamp(o, 0, O_COUNT - 1)]; }
const wchar_t* goalName(int g) { return GOAL_NAMES[std::clamp(g, 0, G_COUNT - 1)]; }
bool needsObject(int action) { return action != A_NONE && action != A_STAY && action != A_COME; }

std::wstring intentPhrase(const Intent& intent) {
    std::wstring s = ACTION_VERBS[std::clamp(intent.action, 0, A_COUNT - 1)];
    if (intent.action == A_NONE) return s;
    const std::wstring obj = OBJECT_FORMS[std::clamp(intent.object, 0, O_COUNT - 1)];
    if (!obj.empty() && needsObject(intent.action)) s += L" " + obj;
    const std::wstring goal = GOAL_FORMS[std::clamp(intent.goal, 0, G_COUNT - 1)];
    if (!goal.empty()) s += L", " + goal;
    return s;
}

std::vector<std::wstring> tokenizeRu(const std::wstring& text) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : text) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
        if (c >= 0x0410 && c <= 0x042F) c = static_cast<wchar_t>(c + 0x20);
        if (c == 0x0401 || c == 0x0451) c = 0x0435;
        const bool letter = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || (c >= 0x0430 && c <= 0x044F);
        if (letter) {
            cur.push_back(c);
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

Thought keywordThink(int level, const std::wstring& text, const std::vector<int>& roomObjects) {
    Thought th;
    th.level = level;
    th.text = text;
    th.tokens = tokenizeRu(text);
    th.fallback = true;
    th.floor = 0.3f;
    int action = A_NONE, object = O_NONE, goal = G_NONE;
    int hits = 0;
    th.importance.assign(th.tokens.size(), 0.f);
    for (size_t i = 0; i < th.tokens.size(); ++i) {
        const auto& w = th.tokens[i];
        bool used = false;
        for (const auto& k : ACTION_KEYS)
            if (action == A_NONE && (std::wstring(k.stem) == L"ко" ? w == L"ко" : startsWith(w, k.stem))) {
                action = k.value;
                used = true;
                break;
            }
        for (const auto& k : OBJECT_KEYS)
            if (object == O_NONE && startsWith(w, k.stem)) {
                object = k.value;
                used = true;
                break;
            }
        for (const auto& k : GOAL_KEYS)
            if (goal == G_NONE && startsWith(w, k.stem) && i > 0) {
                goal = k.value;
                used = true;
                break;
            }
        th.known.push_back(used);
        if (used) {
            th.importance[i] = 1.f;
            ++hits;
        }
    }
    if (!inCurriculum(level, action)) action = A_NONE;
    if (action == A_COME) object = O_ME;
    if (action == A_NONE || action == A_STAY) object = O_NONE;
    for (float& v : th.importance) v = hits ? v / hits : 0.f;
    th.intent = {action, object, goal};
    th.confidence = action == A_NONE ? 0.f : (needsObject(action) && object == O_NONE ? 0.35f : 0.7f);
    th.goalConfidence = goal == G_NONE ? 0.5f : 0.7f;
    th.topAction = {{action, th.confidence}};
    th.topObject = {{object, object == O_NONE ? 0.3f : 0.7f}};
    th.topGoal = {{goal, th.goalConfidence}};
    th.contextObject = object;
    if (needsObject(action) && object != O_NONE && std::find(roomObjects.begin(), roomObjects.end(), object) == roomObjects.end()) {
        th.objectPresent = false;
        for (int o : roomObjects)
            if (o != O_NONE) {
                th.contextObject = o;
                break;
            }
    }
    return th;
}
