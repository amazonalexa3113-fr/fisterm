#include <SDL.h>
#include <SDL_ttf.h>
#include <fontconfig/fontconfig.h>

#include "PTY.hpp"
#include "Renderer.hpp"
#include "Terminal.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

constexpr int INITIAL_WIDTH = 960;
constexpr int INITIAL_HEIGHT = 600;
constexpr int PADDING = 12;
constexpr int WHEEL_LINES = 3;
constexpr int MAX_READS_PER_FRAME = 64; // so `cat hugefile` can't starve the UI

struct App
{
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    TTF_Font* font = nullptr;
    Renderer display;
    Terminal terminal;
    PTY pty;

    int cellWidth = 10;
    int cellHeight = 20;
    std::string windowTitle = Terminal::DEFAULT_TITLE;

    bool running = true;
    bool needRedraw = true;
    bool selecting = false; // left button held, dragging a selection
};

std::string findSystemFont()
{
    FcPattern* pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FAMILY,
                       reinterpret_cast<const FcChar8*>("monospace"));
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    FcResult result;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);

    std::string path;
    if (match)
    {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
            path = reinterpret_cast<const char*>(file);
        FcPatternDestroy(match);
    }

    FcPatternDestroy(pattern);
    return path;
}

void computeGrid(const App& app, int& columns, int& rows)
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(app.window, &width, &height);
    columns = std::max(1, (width - PADDING * 2) / app.cellWidth);
    rows = std::max(1, (height - PADDING * 2) / app.cellHeight);
}

void updateSize(App& app)
{
    int columns = 0;
    int rows = 0;
    computeGrid(app, columns, rows);
    app.terminal.Resize(rows, columns);
    app.pty.Resize(columns, rows);
    app.needRedraw = true;
}

void updateTitle(App& app)
{
    const std::string& title = app.terminal.GetTitle();
    if (title != app.windowTitle)
    {
        app.windowTitle = title;
        SDL_SetWindowTitle(app.window, app.windowTitle.c_str());
    }
}

VTermModifier toVTermMods(Uint16 sdlMods)
{
    int mods = VTERM_MOD_NONE;
    if (sdlMods & KMOD_SHIFT) mods |= VTERM_MOD_SHIFT;
    if (sdlMods & KMOD_ALT)   mods |= VTERM_MOD_ALT;
    if (sdlMods & KMOD_CTRL)  mods |= VTERM_MOD_CTRL;
    return static_cast<VTermModifier>(mods);
}

// Returns the control byte for Ctrl+<key>, or -1.
int controlChar(SDL_Keycode code)
{
    if (code >= SDLK_a && code <= SDLK_z)
        return code - SDLK_a + 1;

    switch (code)
    {
        case SDLK_SPACE:
        case SDLK_AT:           return 0;
        case SDLK_LEFTBRACKET:  return 27;
        case SDLK_BACKSLASH:    return 28;
        case SDLK_RIGHTBRACKET: return 29;
        case SDLK_CARET:        return 30;
        case SDLK_UNDERSCORE:
        case SDLK_SLASH:        return 31;
        default:                return -1;
    }
}

bool mapSpecialKey(SDL_Keycode code, VTermKey& out)
{
    if (code >= SDLK_F1 && code <= SDLK_F12)
    {
        out = static_cast<VTermKey>(VTERM_KEY_FUNCTION(1 + (code - SDLK_F1)));
        return true;
    }

    switch (code)
    {
        case SDLK_RETURN:
        case SDLK_KP_ENTER: out = VTERM_KEY_ENTER; return true;
        case SDLK_TAB:      out = VTERM_KEY_TAB; return true;
        case SDLK_BACKSPACE:out = VTERM_KEY_BACKSPACE; return true;
        case SDLK_ESCAPE:   out = VTERM_KEY_ESCAPE; return true;
        case SDLK_UP:       out = VTERM_KEY_UP; return true;
        case SDLK_DOWN:     out = VTERM_KEY_DOWN; return true;
        case SDLK_LEFT:     out = VTERM_KEY_LEFT; return true;
        case SDLK_RIGHT:    out = VTERM_KEY_RIGHT; return true;
        case SDLK_INSERT:   out = VTERM_KEY_INS; return true;
        case SDLK_DELETE:   out = VTERM_KEY_DEL; return true;
        case SDLK_HOME:     out = VTERM_KEY_HOME; return true;
        case SDLK_END:      out = VTERM_KEY_END; return true;
        case SDLK_PAGEUP:   out = VTERM_KEY_PAGEUP; return true;
        case SDLK_PAGEDOWN: out = VTERM_KEY_PAGEDOWN; return true;
        default: return false;
    }
}

#ifdef __APPLE__
constexpr bool ALT_SENDS_ESC = false; // Option composes characters on macOS
#else
constexpr bool ALT_SENDS_ESC = true;
#endif

void cellAt(const App& app, int x, int y, int& row, int& column)
{
    row = std::clamp((y - PADDING) / app.cellHeight, 0, std::max(0, app.terminal.GetRows() - 1));
    column = std::clamp((x - PADDING) / app.cellWidth, 0, std::max(0, app.terminal.GetColumns() - 1));
    if (y < PADDING)
        row = 0;
    if (x < PADDING)
        column = 0;
}

void copySelection(App& app)
{
    const std::string text = app.terminal.GetSelectedText();
    if (!text.empty())
        SDL_SetClipboardText(text.c_str());
    app.terminal.ClearSelection();
}

void pasteClipboard(App& app)
{
    if (!SDL_HasClipboardText())
        return;

    if (char* text = SDL_GetClipboardText())
    {
        app.terminal.Paste(text);
        SDL_free(text);
    }
}

void handleKey(App& app, const SDL_KeyboardEvent& key)
{
    const SDL_Keycode code = key.keysym.sym;
    const Uint16 mods = key.keysym.mod;
    const bool ctrl = (mods & KMOD_CTRL) != 0;

    // Shift+PageUp/PageDown scroll the view instead of going to the program.
    if ((mods & KMOD_SHIFT) && (code == SDLK_PAGEUP || code == SDLK_PAGEDOWN))
    {
        const int page = std::max(1, app.terminal.GetRows() - 1);
        app.terminal.ScrollView(code == SDLK_PAGEUP ? page : -page);
        return;
    }

    // Copy: Ctrl+C with a selection (or Ctrl+Shift+C). Without a selection,
    // plain Ctrl+C still falls through and sends ^C to the program.
    if (ctrl && code == SDLK_c)
    {
        if (app.terminal.HasSelection())
        {
            copySelection(app);
            return;
        }
        if (mods & KMOD_SHIFT)
            return;
    }

    // Paste: Ctrl+V, Ctrl+Shift+V or Shift+Insert.
    if ((ctrl && code == SDLK_v) || ((mods & KMOD_SHIFT) && code == SDLK_INSERT))
    {
        pasteClipboard(app);
        return;
    }

    VTermKey special;
    if (mapSpecialKey(code, special))
    {
        app.terminal.ClearSelection();
        app.terminal.ScrollToBottom();
        app.terminal.KeyboardKey(special, toVTermMods(mods));
        return;
    }

    const bool alt = (mods & KMOD_ALT) && ALT_SENDS_ESC;

    if (ctrl || alt)
    {
        char bytes[2];
        std::size_t length = 0;

        if (alt)
            bytes[length++] = '\033';

        if (ctrl)
        {
            const int control = controlChar(code);
            if (control < 0)
                return;
            bytes[length++] = static_cast<char>(control);
        }
        else if (code >= 0x20 && code < 0x7F)
        {
            char c = static_cast<char>(code);
            if ((mods & KMOD_SHIFT) && c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
            bytes[length++] = c;
        }
        else
        {
            return;
        }

        app.terminal.ClearSelection();
        app.terminal.ScrollToBottom();
        app.terminal.SendInput(bytes, length);
    }
}

void handleEvent(App& app, const SDL_Event& event)
{
    switch (event.type)
    {
        case SDL_QUIT:
            app.running = false;
            break;

        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                updateSize(app);
            app.needRedraw = true;
            break;

        case SDL_MOUSEWHEEL:
            app.terminal.ScrollView(event.wheel.y * WHEEL_LINES);
            break;

        case SDL_MOUSEBUTTONDOWN:
        {
            if (event.button.button != SDL_BUTTON_LEFT)
                break;

            int row = 0;
            int column = 0;
            cellAt(app, event.button.x, event.button.y, row, column);
            app.selecting = true;
            app.terminal.BeginSelection(row, column);
            break;
        }

        case SDL_MOUSEMOTION:
        {
            if (!app.selecting || !(event.motion.state & SDL_BUTTON_LMASK))
                break;

            // Dragging past the top/bottom edge scrolls the history.
            int width = 0;
            int height = 0;
            SDL_GetWindowSize(app.window, &width, &height);
            if (event.motion.y < PADDING)
                app.terminal.ScrollView(1);
            else if (event.motion.y > height - PADDING)
                app.terminal.ScrollView(-1);

            int row = 0;
            int column = 0;
            cellAt(app, event.motion.x, event.motion.y, row, column);
            app.terminal.UpdateSelection(row, column);
            break;
        }

        case SDL_MOUSEBUTTONUP:
            if (event.button.button == SDL_BUTTON_LEFT)
                app.selecting = false;
            break;

        case SDL_TEXTINPUT:
        {
            // Ctrl/Alt combos are already handled in handleKey.
            const Uint16 mods = SDL_GetModState();
            if ((mods & KMOD_CTRL) || ((mods & KMOD_ALT) && ALT_SENDS_ESC))
                break;

            app.terminal.ClearSelection();
            app.terminal.ScrollToBottom();
            app.terminal.SendInput(event.text.text, std::strlen(event.text.text));
            break;
        }

        case SDL_KEYDOWN:
            handleKey(app, event.key);
            break;

        default:
            break;
    }
}

void cleanup(App& app)
{
    app.pty.Stop();
    app.terminal.Shutdown();
    app.display.Shutdown();

    if (app.font)
        TTF_CloseFont(app.font);
    if (app.renderer)
        SDL_DestroyRenderer(app.renderer);
    if (app.window)
        SDL_DestroyWindow(app.window);

    TTF_Quit();
    SDL_Quit();
    FcFini();
}

int fail(App& app, const char* message)
{
    std::cerr << message << '\n';
    cleanup(app);
    return 1;
}

int main()
{
    App app;

    if (!FcInit())
    {
        std::cerr << "Fontconfig initialization failed\n";
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return fail(app, SDL_GetError());

    if (TTF_Init() != 0)
        return fail(app, TTF_GetError());

    app.window = SDL_CreateWindow(
        Terminal::DEFAULT_TITLE,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        INITIAL_WIDTH, INITIAL_HEIGHT,
        SDL_WINDOW_RESIZABLE);
    if (!app.window)
        return fail(app, SDL_GetError());

    app.renderer = SDL_CreateRenderer(
        app.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!app.renderer)
        app.renderer = SDL_CreateRenderer(app.window, -1, SDL_RENDERER_SOFTWARE);
    if (!app.renderer)
        return fail(app, SDL_GetError());

    const std::string fontPath = findSystemFont();
    if (fontPath.empty())
        return fail(app, "Could not resolve system monospace font");

    app.font = TTF_OpenFont(fontPath.c_str(), 14);
    if (!app.font)
        return fail(app, TTF_GetError());

    TTF_SizeUTF8(app.font, "M", &app.cellWidth, nullptr);
    app.cellHeight = TTF_FontLineSkip(app.font);

    if (!app.display.Initialize(app.renderer, app.font, PADDING,
                                app.cellWidth, app.cellHeight))
        return fail(app, "Failed to initialize renderer");

    // Start with the real window size so the shell never sees a wrong one.
    int columns = 80;
    int rows = 24;
    computeGrid(app, columns, rows);

    if (!app.terminal.Initialize(rows, columns, [&app](const char* data, size_t size) {
            app.pty.Write(data, size);
        }))
        return fail(app, "Failed to initialize terminal");

    const char* shell = std::getenv("SHELL");
    if (!shell || !*shell)
        shell = "/bin/sh";

    if (!app.pty.Start(shell, columns, rows))
        return fail(app, "Failed to start shell PTY");

    SDL_StartTextInput();

    char buffer[8192];

    while (app.running)
    {
        // Sleep until something happens (or 10 ms, to poll the PTY).
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, 10))
        {
            do
            {
                handleEvent(app, event);
            } while (SDL_PollEvent(&event));
        }

        for (int i = 0; i < MAX_READS_PER_FRAME; ++i)
        {
            const ssize_t count = app.pty.Read(buffer, sizeof(buffer));
            if (count <= 0)
                break;
            app.terminal.ReceiveOutput(buffer, static_cast<size_t>(count));
        }

        app.pty.Flush();

        if (!app.pty.IsRunning())
            app.running = false;

        updateTitle(app);

        if (app.needRedraw || app.terminal.HasDamage())
        {
            app.display.Render(app.terminal);
            app.needRedraw = false;
        }
    }

    SDL_StopTextInput();
    cleanup(app);
    return 0;
}
