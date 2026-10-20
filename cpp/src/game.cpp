#include "game.h"

#include <deque>

#include "audio.h"
#include "boss.h"
#include "gfx.h"

namespace {

std::vector<std::pair<std::string, std::wstring>> levelOptions() {
    std::vector<std::pair<std::string, std::wstring>> out;
    for (size_t i = 0; i < levels().size(); ++i) out.push_back({std::to_string(i), std::to_wstring(i + 1) + L". " + levels()[i].title});
    out.push_back({"back", L"Назад"});
    return out;
}

}  // namespace

Game::Game(Input& input)
    : input_(input),
      roster_([this](Assistant& a, const std::wstring& text, Color c) { messages_.push(text, a.name, c); }),
      mainMenu_(L"",
                {{"new", L"Новая игра"}, {"levels", L"Выбор уровня"}, {"lab", L"Нейросети Помощников"}, {"howto", L"Как играть"}, {"quit", L"Выход"}}),
      levelMenu_(L"ВЫБОР УРОВНЯ", levelOptions(), L"Каждый уровень можно запустить отдельно"),
      pauseMenu_(L"ПАУЗА", {{"resume", L"Продолжить"}, {"restart", L"Перезапустить уровень"}, {"lab", L"Нейросети Помощников"}, {"menu", L"Главное меню"},
                            {"quit", L"Выход из игры"}}) {
    room_ = std::make_unique<Room>(levels().front());
}

Game::~Game() { shutdown(); }

void Game::start(bool withBrain) {
    if (withBrain) brain_.start();
}

void Game::shutdown() {
    if (room_ && room_->boss && room_->boss->corrupted) brain_.heal(room_->boss->corrupted);
    brain_.shutdown();
}

// ==========================================================================
// GameAPI
// ==========================================================================
void Game::notify(const std::wstring& text, const std::wstring& speaker, Color color) { messages_.push(text, speaker, color); }

void Game::openPrompt(int puzzleIndex, const std::wstring& hint) {
    if (!roster_.summoned) {
        audio::play("error");
        notify(L"Сначала призови Помощника: подойди к терминалу и нажми E.", L"СИСТЕМА", cfg::WARNING);
        return;
    }
    Assistant& a = roster_.current();
    if (pending_) {
        notify(a.name + L" ещё думает над прошлым промптом.");
        return;
    }
    if (a.busy()) {
        notify(a.name + L" занят задачей — дождись окончания.");
        return;
    }
    promptTarget_ = puzzleIndex;
    std::wstring description, target;
    if (puzzleIndex >= 0 && puzzleIndex < static_cast<int>(room_->puzzles.size())) {
        Puzzle& p = *room_->puzzles[static_cast<size_t>(puzzleIndex)];
        target = p.title();
        if (p.kind == PuzzleKind::ContextTask) description = p.def.ctx.description;
    }
    prompt_.open(a.name, a.color, hint.empty() ? L"что сделать? например: «толкни ящик на плиту»" : hint, description, target);
    audio::play("blip");
    state_ = State::Prompt;
}

void Game::playerCaught(const std::wstring& reason, const Vec2* respawnAt, bool force) {
    if (player_.invulnerable > 0 && !force) return;
    audio::play("detected");
    flash_ = 1.f;
    room_->shake = std::max(room_->shake, 0.4f);
    notify(reason, L"СИСТЕМА", cfg::DANGER);
    room_->particles.burst(player_.pos, cfg::DANGER, 30, 220, 0.7f);
    const Vec2 target = respawnAt ? *respawnAt : room_->spawn;
    player_.respawn(target);
    room_->onPlayerCaught();
    Assistant* a = roster_.active();
    if (a && !a->busy()) a->teleportNear(target);
    // Песочница перестраивается после каждой смерти.
    const int before = room_->reshuffles;
    room_->reshuffle(rng(), player_.pos, a ? a->pos : player_.pos);
    if (room_->reshuffles != before) {
        audio::play("reshuffle");
        notify(L"Песочница перестроилась: объекты, мины и маршруты стражей сменили места.", L"СИСТЕМА", cfg::THOUGHT);
    }
}

void Game::onVictory() {
    if (room_->boss) room_->boss->releaseCorruption(*this);
    audio::play("victory");
    victory_.open();
    state_ = State::Victory;
}

// ==========================================================================
// Уровни
// ==========================================================================
void Game::loadLevel(int index, bool story) {
    if (room_ && room_->boss && room_->boss->corrupted) brain_.heal(room_->boss->corrupted);
    levelIndex_ = index;
    const LevelDef& def = levels()[static_cast<size_t>(index)];
    room_ = std::make_unique<Room>(def);
    room_->notify = [this](const std::wstring& text, const std::wstring& speaker, Color c) { notify(text, speaker, c); };
    room_->onSolvedHook = [this](Puzzle& p) { onPuzzleSolved(p); };
    player_.respawn(room_->spawn);
    player_.invulnerable = 0.f;
    roster_.setupLevel(*room_, def.available, def.unlocked);
    messages_.clear();
    thoughts_.hide();
    pending_.reset();
    last_.valid = false;
    expects_.clear();
    focus_.reset();
    dialog_ = ui::DialogPanel();
    Dialog lines;
    if (story) lines = introStory();
    lines.insert(lines.end(), def.intro.begin(), def.intro.end());
    dialog_.start(lines);
    banner_ = 3.5f;
    flash_ = 0.f;
    state_ = State::Play;
}

void Game::completeLevel() {
    const LevelDef& def = levels()[static_cast<size_t>(levelIndex_)];
    for (int l : def.available) brain_.consolidate(l, 25);  // «сон»: повторение выученного
    if (def.upgrade) {
        audio::play("upgrade");
        upgrade_.open(def.upgrade, def.upgradeText);
        state_ = State::Upgrade;
    } else if (levelIndex_ + 1 < static_cast<int>(levels().size())) {
        loadLevel(levelIndex_ + 1, false);
    } else {
        onVictory();
    }
}

void Game::enterLab(State from) {
    labReturn_ = from;
    lab_.confirmReset = false;
    brain_.requestStatus();
    statusTimer_ = 1.5f;
    state_ = State::Lab;
}

// ==========================================================================
// Ввод
// ==========================================================================
void Game::handleKey(Key k) {
    switch (state_) {
        case State::Menu: {
            const std::string c = mainMenu_.handleKey(k);
            if (c == "new") loadLevel(0, true);
            else if (c == "levels") state_ = State::Levels;
            else if (c == "lab") enterLab(State::Menu);
            else if (c == "howto") state_ = State::HowTo;
            else if (c == "quit") running_ = false;
            break;
        }
        case State::Levels: {
            if (k == K_ESC) {
                state_ = State::Menu;
                break;
            }
            const std::string c = levelMenu_.handleKey(k);
            if (c == "back") state_ = State::Menu;
            else if (!c.empty()) loadLevel(std::stoi(c), false);
            break;
        }
        case State::HowTo:
            if (k == K_ESC || k == K_ENTER || k == K_E || k == K_SPACE) state_ = State::Menu;
            break;
        case State::Lab:
            if (k == K_ESC) {
                if (lab_.confirmReset) lab_.confirmReset = false;
                else state_ = labReturn_;
            } else if (k == K_W || k == K_UP) {
                lab_.selected = lab_.selected == 1 ? 5 : lab_.selected - 1;
                lab_.confirmReset = false;
            } else if (k == K_S || k == K_DOWN) {
                lab_.selected = lab_.selected == 5 ? 1 : lab_.selected + 1;
                lab_.confirmReset = false;
            } else if (k == K_R) {
                if (lab_.confirmReset) {
                    brain_.reset(lab_.selected);
                    lab_.confirmReset = false;
                    audio::play("glitch");
                } else {
                    lab_.confirmReset = true;
                }
            }
            break;
        case State::Play:
            if (dialog_.active()) {
                dialog_.handleKey(k);
                break;
            }
            if (k == K_ESC) {
                pauseMenu_.index = 0;
                state_ = State::Pause;
            } else if (k == K_E) {
                if (focus_) focus_->action(*this);
            } else if (k == K_T || k == K_ENTER) {
                openPrompt(-1, L"");
            } else if (k == K_TAB) {
                if (last_.valid && last_.age < 40.f && roster_.summoned && roster_.current().level == last_.th.level) {
                    correction_.open(last_.th, roster_.current().name, roster_.current().color);
                    state_ = State::Correct;
                } else {
                    notify(L"Поправлять пока нечего: сначала дай Помощнику задачу (T).");
                }
            } else if (k >= K_1 && k <= K_5) {
                if (pending_) {
                    notify(L"Помощник думает — дождись ответа.");
                    break;
                }
                auto [ok, msg] = roster_.select(1 + (k - K_1));
                if (!msg.empty()) notify(msg, L"СИСТЕМА", ok ? roster_.current().color : cfg::DANGER);
                if (!ok && !msg.empty()) audio::play("error");
            }
            break;
        case State::Pause: {
            if (k == K_ESC) {
                state_ = State::Play;
                break;
            }
            const std::string c = pauseMenu_.handleKey(k);
            if (c == "resume") state_ = State::Play;
            else if (c == "restart") loadLevel(levelIndex_, false);
            else if (c == "lab") enterLab(State::Pause);
            else if (c == "menu") state_ = State::Menu;
            else if (c == "quit") running_ = false;
            break;
        }
        case State::Correct: {
            const auto res = correction_.handleKey(k);
            if (res == ui::CorrectionMenu::Result::Cancel) {
                state_ = State::Play;
            } else if (res == ui::CorrectionMenu::Result::Confirm) {
                state_ = State::Play;
                Thought th = correction_.thought();
                const Intent in = correction_.intent();
                brain_.learn(th.level, th.text, in, "correct");
                audio::play("learn");
                notify(L"Учусь: «" + th.text + L"» значит «" + intentPhrase(in) + L"». Запомню!", ASSISTANT_NAMES[th.level], cfg::THOUGHT);
                th.intent = in;
                th.confidence = 1.f;
                last_.valid = false;
                if (roster_.summoned && roster_.current().busy())
                    notify(L"Выполню, когда закончу текущую задачу — просто повтори промпт.", ASSISTANT_NAMES[th.level], cfg::THOUGHT);
                else
                    decide(th, last_.target, true);
            }
            break;
        }
        case State::Upgrade:
            if (upgrade_.handleKey(k)) loadLevel(levelIndex_ + 1, false);
            break;
        case State::Victory:
            if (victory_.handleKey(k)) state_ = State::Menu;
            break;
        case State::Prompt: break;  // ввод текста обрабатывается отдельно
    }
}

// ==========================================================================
// Промпт → нейросеть → решение
// ==========================================================================
void Game::submitPrompt(const std::wstring& text) {
    Assistant& a = roster_.current();
    const int id = brain_.think(a.level, text, room_->contextObjects());
    pending_ = PendingThink{id, a.level, promptTarget_, 0.f};
    a.thinking = true;
    audio::play("think");
    notify(L"«" + text + L"»", L"ОПЕРАТОР", cfg::PLAYER);
}

Puzzle* Game::findPuzzle(const Intent& in, int target) {
    if (target >= 0 && target < static_cast<int>(room_->puzzles.size())) {
        Puzzle& p = *room_->puzzles[static_cast<size_t>(target)];
        if (!p.solved && p.matches(in)) return &p;
    }
    Puzzle* best = nullptr;
    float bestDist = 1e9f;
    for (auto& p : room_->puzzles) {
        if (p->solved || !p->matches(in)) continue;
        const float d = p->anchor().distance(player_.pos) + (p->active ? 0.f : 1e5f);
        if (d < bestDist) {
            best = p.get();
            bestDist = d;
        }
    }
    return best;
}

void Game::decide(const Thought& thIn, int target, bool corrected) {
    Thought th = thIn;
    ui::Decision d;
    Assistant* a = roster_.active();
    if (!a || a->level != th.level) {
        notify(L"Помощник сменился — задача отменена.");
        return;
    }
    Intent in = th.intent;
    auto finish = [&](const std::wstring& verdict, Color c) {
        d.verdict = verdict;
        d.verdictColor = c;
        thoughts_.show(th, d, a->name, a->color, room_->contextObjects());
    };
    if (!corrected) {
        if (in.action == A_NONE) {
            a->say(L"Не понял, что нужно сделать. Переформулируй или нажми Tab и объясни — я научусь.", L"?");
            audio::play("error");
            finish(L"Решение: не понял задачу.", cfg::WARNING);
            return;
        }
        if (th.confidence < th.floor) {
            a->say(L"Не уверен (" + formatPercent(th.confidence) + L"). Похоже на «" + intentPhrase(in) + L"»? Уточни или нажми Tab.", L"?");
            audio::play("error");
            finish(L"Решение: слишком неуверен, переспрашиваю.", cfg::WARNING);
            return;
        }
    }
    if (in.action == A_COME) {
        a->staying = false;
        a->teleportNear(player_.pos);
        a->say(L"Иду к тебе!", L"!");
        if (!corrected) brain_.learn(th.level, th.text, in, "reinforce");
        finish(L"Решение: подойти к Оператору.", cfg::SUCCESS);
        return;
    }
    if (in.action == A_STAY) {
        a->staying = true;
        a->say(L"Жду здесь. Скажи «иди ко мне», когда понадоблюсь.", L"!");
        if (!corrected) brain_.learn(th.level, th.text, in, "reinforce");
        finish(L"Решение: остаться на месте.", cfg::SUCCESS);
        return;
    }
    Puzzle* targetPuzzle = target >= 0 && target < static_cast<int>(room_->puzzles.size()) ? room_->puzzles[static_cast<size_t>(target)].get() : nullptr;
    if (needsObject(in.action)) {
        if (in.object == O_NONE && targetPuzzle) {
            in.object = targetPuzzle->objects().front();
            d.notes.push_back({L"Объект не назван — беру тот, на который ты указал: " + std::wstring(objectName(in.object)) + L".", cfg::THOUGHT});
        } else if (!th.objectPresent && th.contextObject != O_NONE && th.contextObject != in.object) {
            d.notes.push_back({L"В комнате нет «" + std::wstring(objectName(in.object)) + L"» — думаю, ты про «" + objectName(th.contextObject) +
                                   L"».",
                               cfg::WARNING});
            in.object = th.contextObject;
        }
    }
    th.intent = in;
    Puzzle* p = findPuzzle(in, target);
    if (!p) {
        a->say(L"Понял задачу как «" + intentPhrase(in) + L"», но здесь такого сделать нельзя.", L"?");
        audio::play("error");
        if (!corrected && targetPuzzle && !targetPuzzle->matches(in)) brain_.learn(th.level, th.text, th.intent, "discourage");
        finish(L"Решение: подходящей задачи в комнате нет.", cfg::WARNING);
        return;
    }
    if (!p->active) {
        a->say(L"Этот модуль зашифрован Вайбкодером. Сначала текущая фаза.", L"×", cfg::DANGER);
        finish(L"Решение: модуль зашифрован.", cfg::DANGER);
        return;
    }
    d.notes.push_back({std::wstring(L"Задача: ") + p->title() + L".", cfg::DIM});
    if (a->solve(*p, in)) {
        expects_.push_back({p->index, a->level, th.text, in});
        finish(L"Решение: " + intentPhrase(in) + L". Выполняю.", cfg::SUCCESS);
    } else {
        finish(L"Решение: понял, но не могу выполнить (см. реплику).", cfg::DANGER);
    }
}

void Game::onPuzzleSolved(Puzzle& p) {
    for (auto it = expects_.begin(); it != expects_.end();) {
        if (it->puzzle == p.index) {
            brain_.learn(it->level, it->text, it->intent, "reinforce");
            notify(L"Закрепляю: «" + it->text + L"» → " + intentPhrase(it->intent) + L". Сработало!", ASSISTANT_NAMES[it->level], cfg::THOUGHT);
            audio::play("learn");
            it = expects_.erase(it);
        } else {
            ++it;
        }
    }
}

void Game::pollBrain(float dt) {
    brain_.update(dt);
    for (const auto& e : brain_.takeEvents()) {
        if (e.kind == BrainEvent::Kind::Info || e.kind == BrainEvent::Kind::Warning)
            notify(e.text, L"НЕЙРОСЕТЬ", e.kind == BrainEvent::Kind::Warning ? cfg::WARNING : cfg::THOUGHT);
    }
    if (pending_) {
        pending_->age += dt;
        Thought th;
        if (brain_.takeThought(pending_->id, th)) {
            const PendingThink p = *pending_;
            pending_.reset();
            roster_.get(p.level).thinking = false;
            last_ = {th, p.target, 0.f, true};
            audio::play("blip");
            decide(th, p.target, false);
        } else if (pending_->age > 12.f) {
            roster_.get(pending_->level).thinking = false;
            pending_.reset();
            notify(L"Нейросеть не ответила вовремя. Попробуй ещё раз.", L"НЕЙРОСЕТЬ", cfg::WARNING);
        }
    }
    if (state_ == State::Lab && (statusTimer_ -= dt) <= 0) {
        statusTimer_ = 1.5f;
        brain_.requestStatus();
    }
}

// ==========================================================================
// Кадр
// ==========================================================================
void Game::update(float dt) {
    time_ += dt;
    pollBrain(dt);
    if (state_ == State::Prompt) {
        const auto res = prompt_.handle(input_);
        if (res == ui::PromptBox::Result::Submit) {
            state_ = State::Play;
            submitPrompt(prompt_.text());
        } else if (res == ui::PromptBox::Result::Cancel) {
            state_ = State::Play;
        }
    } else {
        for (Key k : input_.pressed()) {
            const State before = state_;
            handleKey(k);
            if (state_ == State::Prompt && before != State::Prompt) break;  // остаток кадра — уже ввод текста
        }
    }
    switch (state_) {
        case State::Play: updatePlay(dt); break;
        case State::Upgrade: upgrade_.update(dt); break;
        case State::Victory:
            victory_.update(dt);
            if (victory_.finished()) state_ = State::Menu;
            break;
        default: break;
    }
}

void Game::updatePlay(float dt) {
    messages_.update(dt);
    thoughts_.update(dt);
    banner_ = std::max(0.f, banner_ - dt);
    flash_ = std::max(0.f, flash_ - dt * 2.5f);
    if (last_.valid) last_.age += dt;
    if (dialog_.active()) {
        dialog_.update(dt);
        focus_.reset();
        return;
    }
    if (selfTesting_) player_.invulnerable = 1e6f;
    room_->update(dt, *this);
    if (state_ != State::Play) return;  // босс мог завершить игру
    const Scramble* scramble = room_->boss ? room_->boss->controlScramble() : nullptr;
    player_.update(dt, input_, *room_, scramble);
    roster_.update(dt, player_);
    focus_.reset();
    float best = 1e9f;
    for (auto& it : room_->interactables(*this)) {
        const float dist = it.pos.distance(player_.pos);
        if (dist <= it.radius && dist < best) {
            best = dist;
            focus_ = std::move(it);
        }
    }
    if (room_->playerOnExit(player_.pos)) completeLevel();
}

// ==========================================================================
// Отрисовка
// ==========================================================================
Vec2 Game::camera() const {
    Vec2 cam = player_.pos - cfg::SCREEN_CENTER;
    if (room_->shake > 0) {
        const float p = 10 * room_->shake;
        cam += Vec2{rng().uniform(-p, p), rng().uniform(-p, p)};
    }
    return {std::round(cam.x), std::round(cam.y)};
}

void Game::drawWorld(Renderer& r) {
    const Vec2 cam = camera();
    const auto flip = room_->boss ? room_->boss->flipFlags() : std::pair{false, false};
    r.clear(cfg::BG);
    if (flip.first || flip.second) r.setFlip(flip.first, flip.second);
    room_->draw(r, cam, time_, [&] {
        roster_.draw(r, cam, time_);
        player_.draw(r, cam, time_);
    });
    r.resetTransform();
    if (state_ == State::Play && !dialog_.active() && focus_) {
        Vec2 pos = focus_->pos - cam;
        if (flip.first) pos.x = cfg::SCREEN_W - pos.x;
        if (flip.second) pos.y = cfg::SCREEN_H - pos.y;
        ui::interactionLabel(r, pos, focus_->label, focus_->color);
    }
    const LevelDef& def = levels()[static_cast<size_t>(levelIndex_)];
    hud_.draw(r, def, static_cast<int>(levels().size()), roster_, *room_, brain_, time_);
    thoughts_.draw(r, time_);
    messages_.draw(r, hud_.slotsTop() - 10, thoughts_.visible());
    ui::levelBanner(r, def, std::min(1.f, banner_));
    dialog_.draw(r, time_);
    ui::redFlash(r, flash_);
}

std::wstring Game::trainingSummary() const {
    if (brain_.mode() == BrainClient::Mode::Fallback) return L"Нейросети недоступны: " + brain_.fallbackReason();
    if (brain_.mode() != BrainClient::Mode::Online) return L"Нейросети: запуск Python…";
    std::wstring s = L"Нейросети:";
    bool all = true;
    for (int l = 1; l <= 5; ++l) {
        const bool ready = brain_.levelReady(l);
        all = all && ready;
        s += L" " + std::wstring(ASSISTANT_NAMES[l]) + L" " + (ready ? L"✓" : formatPercent(brain_.levelProgress(l)));
    }
    return all ? L"Нейросети готовы: свой движок на Python, учатся на твоих промптах" : s;
}

void Game::draw(Renderer& r) {
    const float t = time_;
    switch (state_) {
        case State::Menu:
            ui::menuBackground(r, t);
            ui::title(r, t, {cfg::SCREEN_W / 2.f, 170});
            r.text(L"головоломка о правильных формулировках", {cfg::SCREEN_W / 2.f, 248}, 18, cfg::DIM, Align::Center);
            mainMenu_.draw(r, t, 350);
            r.text(trainingSummary(), {cfg::SCREEN_W / 2.f, cfg::SCREEN_H - 62.f}, 15, cfg::THOUGHT, Align::Center);
            r.text(L"W/S — выбор · Enter — подтвердить", {cfg::SCREEN_W / 2.f, cfg::SCREEN_H - 34.f}, 15, cfg::DIM, Align::Center);
            break;
        case State::Levels:
            ui::menuBackground(r, t);
            ui::dim(r, 0.45f);
            levelMenu_.draw(r, t, 210);
            break;
        case State::HowTo: ui::howTo(r, t); break;
        case State::Lab: lab_.draw(r, t, brain_); break;
        case State::Play:
        case State::Pause:
        case State::Prompt:
        case State::Correct:
            drawWorld(r);
            if (state_ == State::Pause) {
                ui::dim(r, 0.66f);
                pauseMenu_.draw(r, t, 320);
            } else if (state_ == State::Prompt) {
                prompt_.draw(r, t);
            } else if (state_ == State::Correct) {
                correction_.draw(r, t);
            }
            break;
        case State::Upgrade: upgrade_.draw(r, t, L"Помощники повторяют пройденное во сне и закрепляют твои формулировки…"); break;
        case State::Victory: victory_.draw(r, t); break;
    }
    ui::crtOverlay(r);
}

// ==========================================================================
// Самопроверка (prompt.exe --selftest): проходит все уровни и проверяет перестановки
// ==========================================================================
void Game::simulate(float seconds) {
    constexpr float DT = 1.f / 60.f;
    for (float s = 0; s < seconds; s += DT) {
        input_.endFrame();
        update(DT);
    }
}

namespace {

std::vector<Tile> safeFieldPath(const Room& room, Tile pad, Tile goal) {
    // Через поле идём только по безопасным клеткам; вход в поле — с обычного пола у площадки.
    auto passable = [&](Tile t) { return t == goal || (!room.blocksPlayer(t) && !room.mines.count(t)); };
    return room.findPath(pad, TileSet{goal}, passable);
}

}  // namespace

bool Game::selfSolveLevel(int index, std::wstring& log, bool shuffled) {
    loadLevel(index, false);
    dialog_ = ui::DialogPanel();
    if (shuffled) room_->reshuffle(rng(), room_->spawn, room_->spawn);
    const LevelDef& def = levels()[static_cast<size_t>(index)];
    bool ok = true;
    if (room_->terminal) player_.pos = tileCenter(*room_->freeTileNear(*room_->terminal));
    roster_.summon(player_.pos);
    for (auto& up : room_->puzzles) {
        Puzzle& p = *up;
        if (room_->boss) simulate(0.1f);
        // подходящий Помощник
        int level = 0;
        for (int l : def.available)
            if (l >= puzzleMinLevel(p.kind) && (!room_->boss || room_->boss->copied != l) && l < 5) {
                level = l;
                break;
            }
        if (!level) level = def.available.back();
        roster_.setBroken(0);
        if (room_->boss) room_->boss->corrupted = 0;
        if (roster_.selected != level) roster_.select(level);
        Assistant& a = roster_.current();
        Intent in;
        Tile stand = tileOf(p.anchor());
        switch (p.kind) {
            case PuzzleKind::PushBox: in = {A_PUSH, O_BOX, G_NONE}; break;
            case PuzzleKind::ButtonSequence: {
                auto& b = static_cast<ButtonSequence&>(p);
                b.marks = b.order;
                in = {A_PRESS, O_BUTTONS, G_NONE};
                break;
            }
            case PuzzleKind::RouteMemory: {
                auto& rm = static_cast<RouteMemory&>(p);
                const auto path = safeFieldPath(*room_, rm.pad, rm.goal);
                if (path.empty()) {
                    log += L"  ✗ нет безопасного пути через мины\n";
                    return false;
                }
                player_.pos = tileCenter(rm.pad);
                rm.recording = true;
                rm.ready = false;
                rm.route = {rm.pad};
                for (const Tile& t : path) {
                    player_.pos = tileCenter(t);
                    simulate(1.f / 60.f);
                }
                if (!rm.ready) {
                    log += L"  ✗ маршрут не записан\n";
                    return false;
                }
                stand = rm.pad;
                in = {A_ROUTE, O_ROUTE, G_NONE};
                break;
            }
            case PuzzleKind::LinkObjects:
                room_->linkSource = static_cast<LinkObjects*>(&p);
                stand = static_cast<LinkObjects&>(p).lock;
                in = {A_LINK, O_KEY, G_NONE};
                break;
            case PuzzleKind::ContextTask:
                in = {p.def.ctx.action, p.def.ctx.object, p.def.ctx.goals.front()};
                break;
        }
        if (auto t = room_->freeTileNear(stand)) player_.pos = tileCenter(*t);
        a.teleportNear(player_.pos);
        Thought th;
        th.level = a.level;
        th.text = L"самопроверка";
        th.intent = in;
        th.confidence = 1.f;
        th.floor = 0.1f;
        th.contextObject = in.object;
        decide(th, p.index, true);
        float waited = 0;
        while (!p.solved && waited < 60.f) {
            simulate(0.5f);
            waited += 0.5f;
            if (!a.busy() && !p.solved && !p.inProgress && waited < 59.f) {  // Оракул мог отказаться — повторяем
                if (room_->boss) {
                    room_->boss->corrupted = 0;
                    roster_.setBroken(0);
                }
                a.teleportNear(player_.pos);
                decide(th, p.index, true);
            }
        }
        log += std::wstring(p.solved ? L"  ✓ " : L"  ✗ ") + p.title() + L" (" + std::wstring(p.def.id.begin(), p.def.id.end()) + L") за " +
               formatFloat(waited) + L" с\n";
        ok = ok && p.solved;
    }
    if (room_->boss) {
        simulate(5.f);
        const bool won = state_ == State::Victory;
        log += won ? L"  ✓ босс побеждён, титры\n" : L"  ✗ босс не побеждён\n";
        return ok && won;
    }
    simulate(0.2f);
    if (!room_->doorOpen) {
        log += L"  ✗ выход не открылся\n";
        return false;
    }
    player_.pos = tileCenter(room_->doors.front());
    simulate(1.f / 60.f);
    const bool left = state_ != State::Play || levelIndex_ != index;
    log += left ? L"  ✓ уровень пройден\n" : L"  ✗ не удалось выйти\n";
    return ok && left;
}

bool Game::selfCheckShuffle(int index, std::wstring& log) {
    loadLevel(index, false);
    dialog_ = ui::DialogPanel();
    if (room_->terminal) player_.pos = tileCenter(*room_->freeTileNear(*room_->terminal));
    roster_.summon(player_.pos);
    int bad = 0;
    for (int k = 0; k < 40; ++k) {
        room_->reshuffle(rng(), room_->spawn, roster_.current().pos);
        for (auto& up : room_->puzzles) {
            if (auto* pb = dynamic_cast<PushBox*>(up.get())) {
                std::vector<std::pair<Tile, bool>> moves;
                bool solvable = false;
                for (int y = pb->def.zone.y0; y <= pb->def.zone.y1 && !solvable; ++y)
                    for (int x = pb->def.zone.x0; x <= pb->def.zone.x1 && !solvable; ++x)
                        if (room_->isFloorFree({x, y}) && Tile{x, y} != pb->box) solvable = pb->planMoves({x, y}, pb->box, pb->target, moves);
                if (!solvable) ++bad;
            } else if (auto* rm = dynamic_cast<RouteMemory*>(up.get())) {
                if (safeFieldPath(*room_, rm->pad, rm->goal).empty()) ++bad;
            } else if (auto* bs = dynamic_cast<ButtonSequence*>(up.get())) {
                TileSet uniq(bs->buttons.begin(), bs->buttons.end());
                if (uniq.size() != bs->buttons.size()) ++bad;
                for (const Tile& t : bs->buttons)
                    if (!room_->assistantCanWalk(t)) ++bad;
            }
        }
    }
    log += bad ? L"  ✗ перестановки: " + std::to_wstring(bad) + L" нерешаемых раскладок\n"
               : L"  ✓ 40 перестановок, все раскладки решаемы (перестроек " + std::to_wstring(room_->reshuffles) + L")\n";
    return bad == 0;
}

int Game::selfTest(std::wstring& log) {
    selfTesting_ = true;
    int failures = 0;
    for (int i = 0; i < static_cast<int>(levels().size()); ++i) {
        const LevelDef& def = levels()[static_cast<size_t>(i)];
        for (const auto& row : def.map)
            if (row.size() != def.map.front().size()) {
                log += L"Уровень " + std::to_wstring(i + 1) + L": строки карты разной длины\n";
                ++failures;
                break;
            }
        log += L"Уровень " + std::to_wstring(def.number) + L" «" + def.title + L"»\n";
        try {
            if (!selfSolveLevel(i, log, false)) ++failures;
            if (!selfCheckShuffle(i, log)) ++failures;
            // и ещё раз — уже после перестановок
            log += L"  после перестановок:\n";
            if (!selfSolveLevel(i, log, true)) ++failures;
        } catch (const std::exception& e) {
            log += L"  ✗ исключение: " + fromUtf8(e.what()) + L"\n";
            ++failures;
        }
    }
    selfTesting_ = false;
    log += failures ? L"ОШИБОК: " + std::to_wstring(failures) + L"\n" : L"ВСЁ В ПОРЯДКЕ\n";
    return failures;
}

// ==========================================================================
// Сценарий скриншотов (prompt.exe --shots <папка>): настоящие промпты к нейросетям
// ==========================================================================
void Game::realtime(float seconds) {
    for (float t = 0; t < seconds; t += 0.02f) {
        Sleep(20);
        input_.endFrame();
        update(0.02f);
    }
}

void Game::typeText(const std::wstring& text) {
    for (wchar_t c : text) {
        input_.character(c);
        update(1.f / 60.f);
        input_.endFrame();
    }
}

void Game::press(unsigned vk, unsigned scan) {
    input_.keyDown(vk, scan, false);
    update(1.f / 60.f);
    input_.endFrame();
    input_.keyUp(vk, scan);
}

int Game::screenshots(Renderer& r, const std::wstring& dir, std::wstring& log) {
    CreateDirectoryW(dir.c_str(), nullptr);
    auto shot = [&](const std::wstring& name) {
        if (r.begin()) {
            draw(r);
            r.end();
        }
        r.savePng(dir + L"\\" + name + L".png");
        log += name + L"\n";
    };
    auto waitThought = [&] {
        for (int i = 0; i < 400 && pending_; ++i) realtime(0.03f);
    };
    auto prepare = [&](int level) {
        loadLevel(level, false);
        dialog_ = ui::DialogPanel();
        banner_ = 0.f;
        if (room_->terminal) player_.pos = tileCenter(*room_->freeTileNear(*room_->terminal));
        roster_.summon(player_.pos);
    };
    for (int i = 0; i < 1800 && brain_.mode() != BrainClient::Mode::Fallback && !brain_.allReady({1, 2, 3, 4, 5}); ++i) realtime(0.1f);
    log += brain_.modeText() + L"\n";

    state_ = State::Menu;
    time_ = 2.f;
    shot(L"01_menu");
    enterLab(State::Menu);
    realtime(1.5f);
    shot(L"02_lab");

    loadLevel(0, true);
    simulate(1.2f);
    shot(L"03_story");
    prepare(0);
    Puzzle* box = room_->puzzles[0].get();
    player_.pos = tileCenter(*room_->freeTileNear(tileOf(box->anchor())));
    roster_.current().teleportNear(player_.pos);
    simulate(0.5f);
    openPrompt(box->index, box->promptHint());
    typeText(L"пиксель, подвинь коробку на плиту");
    shot(L"04_prompt");
    press(VK_RETURN, 0x1C);
    waitThought();
    simulate(1.5f);
    shot(L"05_thought");
    simulate(1.5f);
    shot(L"06_action");
    simulate(4.f);

    openPrompt(-1, L"");
    typeText(L"пни груз до плиты");
    press(VK_RETURN, 0x1C);
    waitThought();
    simulate(1.5f);
    shot(L"07_reply");
    press(VK_TAB, 0x0F);
    shot(L"08_correction");
    press(VK_RETURN, 0x1C);
    realtime(0.6f);
    simulate(1.5f);
    shot(L"09_learned");

    prepare(4);
    roster_.select(4);
    Puzzle* fan = room_->puzzles[0].get();
    player_.pos = tileCenter(*room_->freeTileNear(tileOf(fan->anchor())));
    roster_.current().teleportNear(player_.pos);
    simulate(0.5f);
    openPrompt(fan->index, fan->promptHint());
    typeText(L"вырубай пропеллер, мне надо пройти по коридору");
    shot(L"10_context_prompt");
    press(VK_RETURN, 0x1C);
    waitThought();
    simulate(1.5f);
    shot(L"11_context_thought");
    simulate(5.f);
    shot(L"12_fan_off");

    prepare(2);
    player_.pos = tileCenter({8, 12});
    simulate(1.f);
    shot(L"13_level3");
    playerCaught(L"Проверка: страж заметил Оператора", nullptr, true);
    simulate(0.6f);
    shot(L"14_reshuffle");

    prepare(5);
    simulate(1.f);
    shot(L"15_level6");

    prepare(7);
    simulate(4.f);
    shot(L"16_boss");
    return 0;
}
