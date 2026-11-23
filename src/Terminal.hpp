#pragma once

#include <vterm.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

class Terminal
{
public:
    using OutputCallback = std::function<void(const char*, std::size_t)>;

    static constexpr const char* DEFAULT_TITLE = "fistermpromax";
    static constexpr std::size_t MAX_SCROLLBACK = 10000;

    Terminal() = default;
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    bool Initialize(int rows, int columns, OutputCallback output);
    void Shutdown();

    void ReceiveOutput(const char* data, std::size_t size);
    void SendInput(const char* data, std::size_t size) const;

    // Keyboard via libvterm (handles application cursor mode, modifiers, etc.)
    void KeyboardKey(VTermKey key, VTermModifier mod);

    // Live screen cell (ignores scrollback).
    bool GetCell(int row, int column, VTermScreenCell& cell) const;
    // Cell as currently visible, taking scrollback offset into account.
    bool GetViewCell(int row, int column, VTermScreenCell& cell) const;

    bool ConvertColorToRGB(VTermColor color, std::uint8_t& red,
                           std::uint8_t& green, std::uint8_t& blue) const;
    void GetCursor(int& row, int& column) const;
    bool IsCursorVisible() const;

    int GetRows() const;
    int GetColumns() const;
    const std::string& GetTitle() const;

    // Scrollback. Positive lines = further back into history.
    void ScrollView(int lines);
    void ScrollToBottom();
    int GetViewOffset() const;

    bool HasDamage() const;
    void ClearDamage();
    void Resize(int rows, int columns);

private:
    static void Output(const char* data, std::size_t size, void* user);
    static int Damage(VTermRect rect, void* user);
    static int MoveCursor(VTermPos pos, VTermPos oldPos, int visible, void* user);
    static int SetTermProperty(VTermProp property, VTermValue* value, void* user);
    static int PushLine(int cols, const VTermScreenCell* cells, void* user);
    static int PopLine(int cols, VTermScreenCell* cells, void* user);

    VTerm* terminal = nullptr;
    VTermScreen* screen = nullptr;
    VTermState* state = nullptr;
    OutputCallback output;

    std::string title = DEFAULT_TITLE;
    std::string pendingTitle;

    std::deque<std::vector<VTermScreenCell>> scrollback;
    int viewOffset = 0;

    int rows = 0;
    int columns = 0;
    bool damaged = false;
    bool cursorVisible = true;
    bool altScreen = false;
};
