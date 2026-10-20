// Намерения Помощников и результат «размышления» над промптом.
// Нумерация действий/объектов/целей совпадает с brain/language.py.
#pragma once

#include <string>
#include <utility>
#include <vector>

enum Action { A_NONE, A_PUSH, A_PRESS, A_ROUTE, A_LINK, A_ON, A_OFF, A_EXPLAIN, A_BREAK, A_COME, A_STAY, A_COUNT };
enum Object { O_NONE, O_BOX, O_BUTTONS, O_ROUTE, O_KEY, O_FAN, O_LIGHT, O_SHADOW, O_DOOR, O_BOSS, O_ME, O_COUNT };
enum Goal { G_NONE, G_PASS, G_ENEMY, G_UNDERSTAND, G_BAD, G_COUNT };

struct Intent {
    int action = A_NONE;
    int object = O_NONE;
    int goal = G_NONE;
    bool operator==(const Intent& o) const { return action == o.action && object == o.object && goal == o.goal; }
};

const wchar_t* actionName(int a);   // «ВЫКЛЮЧИТЬ»
const wchar_t* objectName(int o);   // «ВЕНТИЛЯТОР»
const wchar_t* goalName(int g);     // «ПРОЙТИ»
std::wstring intentPhrase(const Intent& intent);  // «выключить вентилятор, чтобы пройти»
bool needsObject(int action);

// Что Помощник «подумал» о промпте (ответ нейросети или запасного разбора).
struct Thought {
    int level = 0;
    std::wstring text;
    std::vector<std::wstring> tokens;
    std::vector<bool> known;
    std::vector<float> importance;   // вклад слова в решение
    std::vector<float> attention;    // внимание трансформера (может быть пустым)
    std::vector<std::pair<int, float>> topAction, topObject, topGoal;
    Intent intent;
    float confidence = 0.f;
    float goalConfidence = 0.f;
    float floor = 0.3f;              // ниже — Помощник переспрашивает
    int contextObject = O_NONE;      // лучший объект из тех, что есть в комнате
    bool objectPresent = true;
    bool corrupted = false;          // веса испорчены боссом
    bool fallback = false;           // нейросеть недоступна — разбор по ключевым словам
    std::wstring fallbackReason;
};

std::vector<std::wstring> tokenizeRu(const std::wstring& text);

// Запасной разбор по ключевым словам (если Python-«мозг» недоступен).
Thought keywordThink(int level, const std::wstring& text, const std::vector<int>& roomObjects);
