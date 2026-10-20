// Интерфейс игры для головоломок, комнаты и босса (реализует Game).
#pragma once

#include <string>

#include "core.h"
#include "settings.h"

class Player;
class AssistantRoster;
class Room;
class BrainClient;

class GameAPI {
public:
    virtual ~GameAPI() = default;
    virtual Player& player() = 0;
    virtual AssistantRoster& roster() = 0;
    virtual Room& room() = 0;
    virtual BrainClient& brain() = 0;
    virtual void notify(const std::wstring& text, const std::wstring& speaker = L"СИСТЕМА", Color color = cfg::SYSTEM) = 0;
    // Открыть строку промпта. puzzleIndex — головоломка, на которую указал Оператор (-1 — никакая).
    virtual void openPrompt(int puzzleIndex, const std::wstring& hint) = 0;
    virtual void playerCaught(const std::wstring& reason, const Vec2* respawnAt = nullptr, bool force = false) = 0;
    virtual void onVictory() = 0;
};
