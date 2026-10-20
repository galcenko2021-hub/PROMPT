// Связь с Python-«мозгом» Помощников (brain/server.py).
// Игра запускает сервер скрытым процессом и обменивается с ним JSON-строками
// через stdin/stdout. Ответы приходят асинхронно — игра не ждёт и показывает
// «Помощник думает…». Если Python недоступен или процесс упал, клиент пытается
// перезапустить его, а пока — отвечает запасным разбором по ключевым словам.
#pragma once

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "intent.h"
#include "json.h"

struct BrainEvent {
    enum class Kind { Info, Warning, Learned, Consolidated, Status };
    Kind kind = Kind::Info;
    std::wstring text;
    Json data;
};

class BrainClient {
public:
    enum class Mode { Offline, Starting, Online, Fallback };

    BrainClient() = default;
    ~BrainClient();
    BrainClient(const BrainClient&) = delete;
    BrainClient& operator=(const BrainClient&) = delete;

    void start();
    void shutdown();
    void update(float dt);

    int think(int level, const std::wstring& text, const std::vector<int>& roomObjects);
    bool takeThought(int id, Thought& out);
    void learn(int level, const std::wstring& text, const Intent& intent, const char* mode);
    void corrupt(int level, float amount);
    void heal(int level);
    void consolidate(int level, int steps);
    void reset(int level);
    void requestStatus();

    Mode mode() const { return mode_; }
    bool online() const { return mode_ == Mode::Online; }
    bool levelReady(int level) const;
    float levelProgress(int level) const;
    bool allReady(const std::vector<int>& levels) const;
    std::wstring modeText() const;
    const std::wstring& fallbackReason() const { return reason_; }
    const Json& status() const { return status_; }
    std::vector<BrainEvent> takeEvents();

private:
    struct Pending {
        int level;
        std::wstring text;
        std::vector<int> objects;
    };

    bool launch();
    void killProcess();
    void closeHandles();
    bool send(const Json& msg);
    void handle(const Json& msg);
    void readerLoop(void* pipe);
    void processDied(const std::wstring& why);
    void fallbackAll(const std::wstring& reason);
    void event(BrainEvent::Kind kind, const std::wstring& text, Json data = {});
    Thought parseThought(const Json& msg, const Pending& pending) const;

    Mode mode_ = Mode::Offline;
    std::wstring reason_;
    std::wstring serverPath_, modelsDir_;
    std::vector<std::wstring> candidates_;
    size_t candidate_ = 0;
    int restarts_ = 0;
    float sinceLaunch_ = 0.f;
    float restartDelay_ = 0.f;
    bool gotHello_ = false;

    void* process_ = nullptr;
    void* stdinWrite_ = nullptr;
    void* stdoutRead_ = nullptr;
    std::thread reader_;
    std::mutex mutex_;
    std::deque<std::string> inbox_;
    std::atomic<bool> readerDone_{false};

    int nextId_ = 1;
    std::map<int, Pending> pending_;
    std::map<int, Thought> done_;
    float progress_[6] = {0, 0, 0, 0, 0, 0};
    bool ready_[6] = {false, false, false, false, false, false};
    Json status_;
    std::vector<BrainEvent> events_;
};
