#include "brain_client.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "core.h"

namespace {

constexpr float HELLO_TIMEOUT = 25.f;
constexpr int MAX_RESTARTS = 3;

std::wstring exeDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

std::wstring fullPath(const std::wstring& p) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH, buf, nullptr);
    return n ? std::wstring(buf, n) : p;
}

bool fileExists(const std::wstring& p) {
    const DWORD attr = GetFileAttributesW(p.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

BrainClient::~BrainClient() { shutdown(); }

// --------------------------------------------------------------------------
// Запуск и остановка процесса
// --------------------------------------------------------------------------
void BrainClient::start() {
    const std::wstring exe = exeDirectory();
    for (const std::wstring& base : {exe + L"\\..\\brain", exe + L"\\brain", exe + L"\\..\\..\\brain"}) {
        if (fileExists(base + L"\\server.py")) {
            serverPath_ = fullPath(base + L"\\server.py");
            modelsDir_ = fullPath(base + L"\\..\\models");
            break;
        }
    }
    if (serverPath_.empty()) {
        mode_ = Mode::Fallback;
        reason_ = L"не найден brain\\server.py";
        event(BrainEvent::Kind::Warning, L"Нейросети не найдены (brain\\server.py) — работаю на разборе по ключевым словам.");
        return;
    }
    CreateDirectoryW(modelsDir_.c_str(), nullptr);
    wchar_t custom[MAX_PATH];
    if (GetEnvironmentVariableW(L"PROMPT_PYTHON", custom, MAX_PATH) > 0) candidates_.push_back(L"\"" + std::wstring(custom) + L"\"");
    candidates_.push_back(L"python");
    candidates_.push_back(L"py -3");
    candidate_ = 0;
    if (!launch()) fallbackAll(reason_);
}

bool BrainClient::launch() {
    while (candidate_ < candidates_.size()) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE outRead = nullptr, outWrite = nullptr, inRead = nullptr, inWrite = nullptr;
        if (!CreatePipe(&outRead, &outWrite, &sa, 0) || !CreatePipe(&inRead, &inWrite, &sa, 0)) {
            reason_ = L"не удалось создать каналы связи";
            return false;
        }
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
        const std::wstring logPath = modelsDir_ + L"\\brain.log";
        HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inRead;
        si.hStdOutput = outWrite;
        si.hStdError = log != INVALID_HANDLE_VALUE ? log : outWrite;
        PROCESS_INFORMATION pi{};
        std::wstring cmd = candidates_[candidate_] + L" -u -X utf8 \"" + serverPath_ + L"\" --models \"" + modelsDir_ + L"\"";
        const std::wstring workDir = serverPath_.substr(0, serverPath_.find_last_of(L"\\/"));
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, workDir.c_str(), &si, &pi);
        CloseHandle(outWrite);
        CloseHandle(inRead);
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (!ok) {
            CloseHandle(outRead);
            CloseHandle(inWrite);
            ++candidate_;
            continue;
        }
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        stdinWrite_ = inWrite;
        stdoutRead_ = outRead;
        readerDone_ = false;
        gotHello_ = false;
        sinceLaunch_ = 0.f;
        mode_ = Mode::Starting;
        reader_ = std::thread([this, outRead] { readerLoop(outRead); });
        return true;
    }
    reason_ = L"Python 3 с numpy не найден (можно указать путь в переменной PROMPT_PYTHON)";
    return false;
}

void BrainClient::readerLoop(void* pipe) {
    std::string buffer;
    char chunk[8192];
    DWORD read = 0;
    while (ReadFile(static_cast<HANDLE>(pipe), chunk, sizeof(chunk), &read, nullptr) && read > 0) {
        buffer.append(chunk, read);
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::lock_guard<std::mutex> lock(mutex_);
            inbox_.push_back(std::move(line));
        }
    }
    readerDone_ = true;
}

void BrainClient::closeHandles() {
    if (stdinWrite_) CloseHandle(static_cast<HANDLE>(stdinWrite_));
    stdinWrite_ = nullptr;
    if (reader_.joinable()) reader_.join();
    if (stdoutRead_) CloseHandle(static_cast<HANDLE>(stdoutRead_));
    stdoutRead_ = nullptr;
    if (process_) CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
}

void BrainClient::killProcess() {
    if (process_) TerminateProcess(static_cast<HANDLE>(process_), 1);
    closeHandles();
}

void BrainClient::shutdown() {
    if (!process_) return;
    Json bye = Json::object();
    bye.set("id", nextId_++).set("type", "shutdown");
    send(bye);
    if (WaitForSingleObject(static_cast<HANDLE>(process_), 5000) != WAIT_OBJECT_0) TerminateProcess(static_cast<HANDLE>(process_), 1);
    closeHandles();
    mode_ = Mode::Offline;
}

bool BrainClient::send(const Json& msg) {
    if (!stdinWrite_) return false;
    const std::string line = msg.dump() + "\n";
    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(stdinWrite_), line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) return false;
    return written == line.size();
}

// --------------------------------------------------------------------------
// Кадр: разбор ответов и надзор за процессом
// --------------------------------------------------------------------------
void BrainClient::update(float dt) {
    if (mode_ == Mode::Fallback && restartDelay_ > 0.f) {
        restartDelay_ -= dt;
        if (restartDelay_ <= 0.f) {
            event(BrainEvent::Kind::Info, L"Перезапускаю нейросети Помощников…");
            if (!launch()) fallbackAll(reason_);
        }
    }
    std::deque<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lines.swap(inbox_);
    }
    for (const auto& line : lines) {
        bool ok = false;
        Json msg = Json::parse(line, &ok);
        if (ok) handle(msg);
    }
    if (mode_ == Mode::Starting || mode_ == Mode::Online) {
        sinceLaunch_ += dt;
        const bool exited = process_ && WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_OBJECT_0;
        if (!gotHello_ && (exited || readerDone_ || sinceLaunch_ > HELLO_TIMEOUT)) {
            killProcess();  // этот способ запуска Python не сработал — пробуем следующий
            ++candidate_;
            if (!launch()) fallbackAll(reason_ + L" — подробности в models\\brain.log");
        } else if (gotHello_ && (exited || readerDone_)) {
            processDied(L"процесс нейросетей завершился");
        }
    }
}

void BrainClient::processDied(const std::wstring& why) {
    killProcess();
    if (restarts_ < MAX_RESTARTS) {
        ++restarts_;
        reason_ = why;
        mode_ = Mode::Fallback;
        restartDelay_ = 2.f;
        for (bool& r : ready_) r = false;
        event(BrainEvent::Kind::Warning, L"Нейросети упали (" + why + L"). Перезапуск через 2 с, пока работаю по ключевым словам.");
        for (auto& [id, p] : pending_) {
            Thought th = keywordThink(p.level, p.text, p.objects);
            th.fallbackReason = why;
            done_[id] = th;
        }
        pending_.clear();
    } else {
        fallbackAll(why + L" (слишком много перезапусков)");
    }
}

void BrainClient::fallbackAll(const std::wstring& reason) {
    mode_ = Mode::Fallback;
    reason_ = reason;
    restartDelay_ = 0.f;
    event(BrainEvent::Kind::Warning, L"Нейросети недоступны: " + reason + L". Помощники понимают только ключевые слова.");
    for (auto& [id, p] : pending_) {
        Thought th = keywordThink(p.level, p.text, p.objects);
        th.fallbackReason = reason;
        done_[id] = th;
    }
    pending_.clear();
}

void BrainClient::handle(const Json& msg) {
    const std::string& type = msg["type"].string();
    if (type == "hello") {
        gotHello_ = true;
        mode_ = Mode::Online;
        event(BrainEvent::Kind::Info, L"Нейросети Помощников на связи (Python, свой движок).");
        return;
    }
    if (type == "progress") {
        const int level = msg["level"].integer();
        if (level >= 1 && level <= 5) {
            progress_[level] = static_cast<float>(msg["value"].number());
            if (progress_[level] >= 1.f) ready_[level] = true;
        }
        return;
    }
    if (type == "ready") {
        for (int l = 1; l <= 5; ++l) ready_[l] = true;
        status_ = msg["status"];
        event(BrainEvent::Kind::Status, L"", msg["status"]);
        return;
    }
    if (type == "status") {
        status_ = msg["status"];
        event(BrainEvent::Kind::Status, L"", msg["status"]);
        return;
    }
    const int id = msg["id"].integer(-1);
    auto it = pending_.find(id);
    if (type == "thought" && it != pending_.end()) {
        done_[id] = parseThought(msg, it->second);
        pending_.erase(it);
        return;
    }
    if (type == "error" && it != pending_.end()) {
        Thought th = keywordThink(it->second.level, it->second.text, it->second.objects);
        if (msg["error"].string() == "training") {
            ready_[it->second.level] = false;
            th.fallbackReason = L"модель ещё обучается (" + formatPercent(static_cast<float>(msg["progress"].number())) + L")";
        } else {
            th.fallbackReason = msg["error"].wstring();
        }
        done_[id] = th;
        pending_.erase(it);
        return;
    }
    if (type == "learned") {
        event(BrainEvent::Kind::Learned, L"", msg);
        return;
    }
    if (type == "consolidated") {
        event(BrainEvent::Kind::Consolidated, L"", msg);
        return;
    }
    if (type == "reset") {
        const int level = msg["level"].integer();
        if (level >= 1 && level <= 5) {
            ready_[level] = false;
            progress_[level] = 0.f;
        }
    }
}

Thought BrainClient::parseThought(const Json& msg, const Pending& p) const {
    Thought th;
    th.level = p.level;
    th.text = p.text;
    const Json& tokens = msg["tokens"];
    for (size_t i = 0; i < tokens.size(); ++i) th.tokens.push_back(tokens[i].wstring());
    for (size_t i = 0; i < msg["known"].size(); ++i) th.known.push_back(msg["known"][i].boolean());
    for (size_t i = 0; i < msg["importance"].size(); ++i) th.importance.push_back(static_cast<float>(msg["importance"][i].number()));
    for (size_t i = 0; i < msg["attention"].size(); ++i) th.attention.push_back(static_cast<float>(msg["attention"][i].number()));
    auto readTop = [](const Json& arr, std::vector<std::pair<int, float>>& out) {
        for (size_t i = 0; i < arr.size(); ++i) out.push_back({arr[i][0].integer(), static_cast<float>(arr[i][1].number())});
    };
    readTop(msg["top_action"], th.topAction);
    readTop(msg["top_object"], th.topObject);
    readTop(msg["top_goal"], th.topGoal);
    const Json& intent = msg["intent"];
    th.intent = {intent[0].integer(), intent[1].integer(), intent[2].integer()};
    th.confidence = static_cast<float>(msg["confidence"].number());
    th.goalConfidence = static_cast<float>(msg["goal_confidence"].number());
    th.floor = static_cast<float>(msg["floor"].number(0.3));
    th.contextObject = msg["context_object"].integer(th.intent.object);
    th.objectPresent = msg["object_present"].boolean(true);
    th.corrupted = msg["corrupted"].boolean(false);
    while (th.known.size() < th.tokens.size()) th.known.push_back(true);
    while (th.importance.size() < th.tokens.size()) th.importance.push_back(0.f);
    return th;
}

// --------------------------------------------------------------------------
// Запросы
// --------------------------------------------------------------------------
int BrainClient::think(int level, const std::wstring& text, const std::vector<int>& roomObjects) {
    const int id = nextId_++;
    Pending p{level, text, roomObjects};
    if (mode_ == Mode::Online) {
        Json objects = Json::array();
        for (int o : roomObjects) objects.push(o);
        Json msg = Json::object();
        msg.set("id", id).set("type", "think").set("level", level).set("text", text).set("objects", objects);
        if (send(msg)) {
            pending_[id] = p;
            return id;
        }
        processDied(L"не удалось отправить запрос");
    }
    Thought th = keywordThink(level, text, roomObjects);
    th.fallbackReason = mode_ == Mode::Starting ? L"нейросети ещё запускаются" : reason_;
    done_[id] = th;
    return id;
}

bool BrainClient::takeThought(int id, Thought& out) {
    auto it = done_.find(id);
    if (it == done_.end()) return false;
    out = std::move(it->second);
    done_.erase(it);
    return true;
}

void BrainClient::learn(int level, const std::wstring& text, const Intent& intent, const char* mode) {
    if (mode_ != Mode::Online || !levelReady(level)) return;
    Json arr = Json::array();
    arr.push(intent.action).push(intent.object).push(intent.goal);
    Json msg = Json::object();
    msg.set("id", nextId_++).set("type", "learn").set("level", level).set("text", text).set("intent", arr).set("mode", mode);
    send(msg);
}

void BrainClient::corrupt(int level, float amount) {
    if (mode_ != Mode::Online) return;
    Json msg = Json::object();
    msg.set("id", nextId_++).set("type", "corrupt").set("level", level).set("amount", amount);
    send(msg);
}

void BrainClient::heal(int level) {
    if (mode_ != Mode::Online) return;
    Json msg = Json::object();
    msg.set("id", nextId_++).set("type", "heal").set("level", level);
    send(msg);
}

void BrainClient::consolidate(int level, int steps) {
    if (mode_ != Mode::Online || !levelReady(level)) return;
    Json msg = Json::object();
    msg.set("id", nextId_++).set("type", "consolidate").set("level", level).set("steps", steps);
    send(msg);
}

void BrainClient::reset(int level) {
    if (mode_ != Mode::Online) return;
    Json msg = Json::object();
    msg.set("id", nextId_++).set("type", "reset").set("level", level);
    if (send(msg)) {
        ready_[level] = false;
        progress_[level] = 0.f;
    }
}

void BrainClient::requestStatus() {
    if (mode_ != Mode::Online) return;
    Json msg = Json::object();
    msg.set("id", nextId_++).set("type", "status");
    send(msg);
}

bool BrainClient::levelReady(int level) const { return level >= 1 && level <= 5 && ready_[level]; }

float BrainClient::levelProgress(int level) const { return level >= 1 && level <= 5 ? progress_[level] : 0.f; }

bool BrainClient::allReady(const std::vector<int>& levels) const {
    if (mode_ == Mode::Fallback) return true;  // ждать нечего — работает запасной разбор
    for (int l : levels)
        if (!levelReady(l)) return false;
    return true;
}

std::wstring BrainClient::modeText() const {
    switch (mode_) {
        case Mode::Online: return L"нейросети: Python, свой движок";
        case Mode::Starting: return L"нейросети: запуск…";
        case Mode::Fallback: return L"нейросети недоступны: " + reason_;
        default: return L"нейросети: выключены";
    }
}

std::vector<BrainEvent> BrainClient::takeEvents() {
    std::vector<BrainEvent> out;
    out.swap(events_);
    return out;
}

void BrainClient::event(BrainEvent::Kind kind, const std::wstring& text, Json data) { events_.push_back({kind, text, std::move(data)}); }
