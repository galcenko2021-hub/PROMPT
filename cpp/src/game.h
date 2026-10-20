// Игра: состояния, уровни, связь промпт → нейросеть → решение → действие Помощника.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "assistant.h"
#include "brain_client.h"
#include "game_api.h"
#include "input.h"
#include "player.h"
#include "room.h"
#include "ui.h"

class Game : public GameAPI {
public:
    explicit Game(Input& input);
    ~Game() override;

    void start(bool withBrain);
    void update(float dt);
    void draw(Renderer& r);
    bool running() const { return running_; }
    void requestQuit() { running_ = false; }
    void shutdown();
    int selfTest(std::wstring& log);
    int screenshots(Renderer& r, const std::wstring& dir, std::wstring& log);  // сценарий с настоящими нейросетями

    // GameAPI
    Player& player() override { return player_; }
    AssistantRoster& roster() override { return roster_; }
    Room& room() override { return *room_; }
    BrainClient& brain() override { return brain_; }
    void notify(const std::wstring& text, const std::wstring& speaker = L"СИСТЕМА", Color color = cfg::SYSTEM) override;
    void openPrompt(int puzzleIndex, const std::wstring& hint) override;
    void playerCaught(const std::wstring& reason, const Vec2* respawnAt = nullptr, bool force = false) override;
    void onVictory() override;

private:
    enum class State { Menu, Levels, HowTo, Lab, Play, Pause, Prompt, Correct, Upgrade, Victory };

    void loadLevel(int index, bool story);
    void completeLevel();
    void handleKey(Key k);
    void updatePlay(float dt);
    void pollBrain(float dt);
    void submitPrompt(const std::wstring& text);
    void decide(const Thought& th, int target, bool corrected);
    Puzzle* findPuzzle(const Intent& in, int target);
    void onPuzzleSolved(Puzzle& p);
    void enterLab(State from);
    Vec2 camera() const;
    void drawWorld(Renderer& r);
    std::wstring trainingSummary() const;

    // самопроверка
    bool selfSolveLevel(int index, std::wstring& log, bool shuffled);
    bool selfCheckShuffle(int index, std::wstring& log);
    void simulate(float seconds);
    void realtime(float seconds);
    void typeText(const std::wstring& text);
    void press(unsigned vk, unsigned scan);

    Input& input_;
    BrainClient brain_;
    AssistantRoster roster_;
    Player player_;
    std::unique_ptr<Room> room_;
    int levelIndex_ = 0;
    State state_ = State::Menu;
    State labReturn_ = State::Menu;
    bool running_ = true;
    bool selfTesting_ = false;
    float time_ = 0.f, banner_ = 0.f, flash_ = 0.f, statusTimer_ = 0.f;

    ui::Menu mainMenu_, levelMenu_, pauseMenu_;
    ui::MessageLog messages_;
    ui::DialogPanel dialog_;
    ui::HUD hud_;
    ui::PromptBox prompt_;
    ui::ThoughtPanel thoughts_;
    ui::CorrectionMenu correction_;
    ui::LabScreen lab_;
    ui::UpgradeScreen upgrade_;
    ui::VictoryScreen victory_;
    std::optional<Interactable> focus_;

    struct PendingThink {
        int id, level, target;
        float age;
    };
    std::optional<PendingThink> pending_;
    int promptTarget_ = -1;

    struct LastThought {
        Thought th;
        int target = -1;
        float age = 0.f;
        bool valid = false;
    } last_;

    struct Expect {
        int puzzle, level;
        std::wstring text;
        Intent intent;
    };
    std::vector<Expect> expects_;
};
