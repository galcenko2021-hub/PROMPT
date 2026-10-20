// «ПРОМПТ» (C++): окно Win32, игровой цикл, режим самопроверки.
//   prompt.exe             — игра
//   prompt.exe --selftest  — пройти все уровни автоматически и записать selftest.log
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>

#include <cstdio>
#include <string>

#include "audio.h"
#include "game.h"
#include "render.h"

namespace {

Input g_input;
Game* g_game = nullptr;

std::wstring exeDirectory();

// При падении пишем стек вызовов в crash.log рядом с игрой.
LONG WINAPI crashHandler(EXCEPTION_POINTERS* info) {
    HANDLE process = GetCurrentProcess(), thread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);
    CONTEXT ctx = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    std::string out = "exception 0x" + std::to_string(info->ExceptionRecord->ExceptionCode) + "\n";
    for (int i = 0; i < 40; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        out += SymFromAddr(process, frame.AddrPC.Offset, &disp, sym) ? sym->Name : "?";
        if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisp, &line)) out += std::string("  ") + line.FileName + ":" + std::to_string(line.LineNumber);
        out += "\n";
    }
    FILE* f = _wfopen((exeDirectory() + L"\\crash.log").c_str(), L"wb");
    if (f) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) break;
            g_input.keyDown(static_cast<unsigned>(wp), (lp >> 16) & 0xFF, (lp & (1 << 30)) != 0);
            if (wp == VK_TAB || wp == VK_SPACE || wp == VK_F10) return 0;
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            g_input.keyUp(static_cast<unsigned>(wp), (lp >> 16) & 0xFF);
            break;
        case WM_CHAR:
            g_input.character(static_cast<wchar_t>(wp));
            return 0;
        case WM_KILLFOCUS:
            g_input.releaseAll();
            break;
        case WM_CLOSE:
            if (g_game) g_game->requestQuit();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::wstring exeDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

int runSelfTest() {
    Game game(g_input);
    std::wstring log;
    const int failures = game.selfTest(log);
    const std::string utf8 = toUtf8(log);
    FILE* f = _wfopen((exeDirectory() + L"\\selftest.log").c_str(), L"wb");
    if (f) {
        fwrite(utf8.data(), 1, utf8.size(), f);
        fclose(f);
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        DWORD written;
        WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), log.c_str(), static_cast<DWORD>(log.size()), &written, nullptr);
    }
    return failures == 0 ? 0 : 1;
}

int runShots(std::wstring dir) {
    while (!dir.empty() && (dir.back() == L' ' || dir.back() == L'"')) dir.pop_back();
    while (!dir.empty() && (dir.front() == L' ' || dir.front() == L'"')) dir.erase(dir.begin());
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring log;
    {
        Renderer renderer;
        if (!renderer.initOffscreen(1.f)) return 2;
        Game game(g_input);
        game.start(true);
        try {
            game.screenshots(renderer, dir, log);
        } catch (const std::exception& e) {
            log += L"исключение: " + fromUtf8(e.what()) + L"\n";
        }
        game.shutdown();
    }
    const std::string utf8 = toUtf8(log);
    FILE* f = _wfopen((dir + L"\\shots.log").c_str(), L"wb");
    if (f) {
        fwrite(utf8.data(), 1, utf8.size(), f);
        fclose(f);
    }
    CoUninitialize();
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    SetUnhandledExceptionFilter(crashHandler);
    const std::wstring args(cmdLine);
    if (args.find(L"--selftest") != std::wstring::npos) return runSelfTest();
    if (const size_t pos = args.find(L"--shots "); pos != std::wstring::npos) return runShots(args.substr(pos + 8));

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"PromptGameWindow";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // Масштаб: по DPI экрана, но так, чтобы окно 1280x720 целиком помещалось.
    const UINT dpi = GetDpiForSystem();
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    float scale = dpi / 96.f;
    scale = std::min(scale, (work.right - work.left - 40) / static_cast<float>(cfg::SCREEN_W));
    scale = std::min(scale, (work.bottom - work.top - 80) / static_cast<float>(cfg::SCREEN_H));
    scale = std::max(scale, 0.5f);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect{0, 0, static_cast<LONG>(cfg::SCREEN_W * scale), static_cast<LONG>(cfg::SCREEN_H * scale)};
    AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"ПРОМПТ", style, work.left + (work.right - work.left - w) / 2,
                                work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;

    Renderer renderer;
    if (!renderer.init(hwnd, scale)) {
        MessageBoxW(hwnd, L"Не удалось инициализировать Direct2D.", L"ПРОМПТ", MB_ICONERROR);
        return 1;
    }
    audio::init();
    ShowWindow(hwnd, SW_SHOW);

    Game game(g_input);
    g_game = &game;
    game.start(true);

    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    MSG msg;
    while (game.running()) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) game.requestQuit();
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - last.QuadPart) / freq.QuadPart;
        last = now;
        dt = std::min(dt, cfg::MAX_FRAME_TIME);
        game.update(dt);
        g_input.endFrame();
        if (renderer.begin()) {
            game.draw(renderer);
            renderer.end();
        }
        if (IsIconic(hwnd)) Sleep(30);
    }
    game.shutdown();
    g_game = nullptr;
    audio::shutdown();
    DestroyWindow(hwnd);
    CoUninitialize();
    return 0;
}
