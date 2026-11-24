#include "Terminal.hpp"

#include <algorithm>
#include <utility>

namespace
{
VTermScreenCell BlankCell()
{
    VTermScreenCell cell{};
    cell.width = 1;
    cell.fg.type = VTERM_COLOR_DEFAULT_FG;
    cell.bg.type = VTERM_COLOR_DEFAULT_BG;
    return cell;
}
}

namespace
{
void AppendUTF8(std::uint32_t codepoint, std::string& output)
{
    if (codepoint <= 0x7F)
        output += static_cast<char>(codepoint);
    else if (codepoint <= 0x7FF)
    {
        output += static_cast<char>(0xC0 | (codepoint >> 6));
        output += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    else if (codepoint <= 0xFFFF)
    {
        output += static_cast<char>(0xE0 | (codepoint >> 12));
        output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        output += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    else
    {
        output += static_cast<char>(0xF0 | (codepoint >> 18));
        output += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        output += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
}
}

Terminal::~Terminal()
{
    Shutdown();
}

bool Terminal::Initialize(int initialRows, int initialColumns, OutputCallback outputCallback)
{
    if (terminal || initialRows < 1 || initialColumns < 1)
        return false;

    terminal = vterm_new(initialRows, initialColumns);
    if (!terminal)
        return false;

    rows = initialRows;
    columns = initialColumns;
    output = std::move(outputCallback);

    vterm_set_utf8(terminal, 1);
    state = vterm_obtain_state(terminal);
    screen = vterm_obtain_screen(terminal);
    if (!state || !screen)
    {
        Shutdown();
        return false;
    }

    // Alt screen so vim/less/htop don't pollute scrollback.
    vterm_screen_enable_altscreen(screen, 1);
    vterm_screen_reset(screen, 1);
    vterm_screen_set_damage_merge(screen, VTERM_DAMAGE_CELL);

    // NOTE: designated initializers must follow the struct's field order.
    static const VTermScreenCallbacks callbacks{
        .damage = Damage,
        .movecursor = MoveCursor,
        .settermprop = SetTermProperty,
        .sb_pushline = PushLine,
        .sb_popline = PopLine,
    };
    vterm_screen_set_callbacks(screen, &callbacks, this);
    vterm_output_set_callback(terminal, Output, this);

    damaged = true;
    return true;
}

void Terminal::Shutdown()
{
    if (terminal)
        vterm_free(terminal);
    terminal = nullptr;
    screen = nullptr;
    state = nullptr;
    output = {};
    scrollback.clear();
    viewOffset = 0;
    selectionActive = false;
    rows = 0;
    columns = 0;
    damaged = false;
}

void Terminal::ReceiveOutput(const char* data, std::size_t size)
{
    if (!terminal || !data || size == 0)
        return;

    vterm_input_write(terminal, data, size);
    vterm_screen_flush_damage(screen);
}

void Terminal::SendInput(const char* data, std::size_t size) const
{
    if (output && data && size > 0)
        output(data, size);
}

void Terminal::KeyboardKey(VTermKey key, VTermModifier mod)
{
    if (terminal)
        vterm_keyboard_key(terminal, key, mod);
}

void Terminal::KeyboardUnicode(std::uint32_t codepoint, VTermModifier mod)
{
    if (terminal)
        vterm_keyboard_unichar(terminal, codepoint, mod);
}

void Terminal::MouseMove(int row, int column, VTermModifier mod)
{
    if (terminal && mouseMode != VTERM_PROP_MOUSE_NONE)
        vterm_mouse_move(terminal, row, column, mod);
}

void Terminal::MouseButton(int button, bool pressed, VTermModifier mod)
{
    if (terminal && mouseMode != VTERM_PROP_MOUSE_NONE)
        vterm_mouse_button(terminal, button, pressed, mod);
}

bool Terminal::IsMouseReporting() const
{
    return mouseMode != VTERM_PROP_MOUSE_NONE;
}

bool Terminal::GetCell(int row, int column, VTermScreenCell& cell) const
{
    return screen && row >= 0 && row < rows && column >= 0 && column < columns &&
        vterm_screen_get_cell(screen, VTermPos{row, column}, &cell);
}

bool Terminal::GetViewCell(int row, int column, VTermScreenCell& cell) const
{
    if (row < 0 || row >= rows || column < 0 || column >= columns)
        return false;

    const int liveRow = row - viewOffset;
    if (liveRow >= 0)
        return GetCell(liveRow, column, cell);

    const long index = static_cast<long>(scrollback.size()) + liveRow;
    if (index < 0 || index >= static_cast<long>(scrollback.size()))
    {
        cell = BlankCell();
        return true;
    }

    const auto& line = scrollback[static_cast<std::size_t>(index)];
    cell = column < static_cast<int>(line.size()) ? line[column] : BlankCell();
    return true;
}

bool Terminal::ConvertColorToRGB(VTermColor color, std::uint8_t& red,
                                 std::uint8_t& green, std::uint8_t& blue) const
{
    if (!screen || VTERM_COLOR_IS_DEFAULT_FG(&color) ||
        VTERM_COLOR_IS_DEFAULT_BG(&color))
        return false;

    vterm_screen_convert_color_to_rgb(screen, &color);
    if (!VTERM_COLOR_IS_RGB(&color))
        return false;

    red = color.rgb.red;
    green = color.rgb.green;
    blue = color.rgb.blue;
    return true;
}

void Terminal::GetCursor(int& row, int& column) const
{
    VTermPos position{};
    if (state)
        vterm_state_get_cursorpos(state, &position);
    row = position.row;
    column = position.col;
}

bool Terminal::IsCursorVisible() const
{
    return cursorVisible;
}

int Terminal::GetCursorShape() const { return cursorShape; }
bool Terminal::IsCursorBlinking() const { return cursorBlink; }

int Terminal::GetRows() const
{
    return rows;
}

int Terminal::GetColumns() const
{
    return columns;
}

const std::string& Terminal::GetTitle() const
{
    return title;
}

void Terminal::ScrollView(int lines)
{
    if (altScreen)
        return;

    const int maxOffset = static_cast<int>(scrollback.size());
    const int next = std::clamp(viewOffset + lines, 0, maxOffset);
    if (next != viewOffset)
    {
        viewOffset = next;
        damaged = true;
    }
}

void Terminal::ScrollToBottom()
{
    if (viewOffset != 0)
    {
        viewOffset = 0;
        damaged = true;
    }
}

int Terminal::GetViewOffset() const
{
    return viewOffset;
}

bool Terminal::HasDamage() const
{
    return damaged;
}

void Terminal::ClearDamage()
{
    damaged = false;
}

void Terminal::Resize(int newRows, int newColumns)
{
    if (!terminal || newRows < 1 || newColumns < 1 ||
        (newRows == rows && newColumns == columns))
        return;

    selectionActive = false;
    rows = newRows;
    columns = newColumns;
    vterm_set_size(terminal, rows, columns);
    vterm_screen_flush_damage(screen);

    viewOffset = std::clamp(viewOffset, 0, static_cast<int>(scrollback.size()));
    damaged = true;
}

bool Terminal::Before(const SelectionPoint& a, const SelectionPoint& b)
{
    return a.line < b.line || (a.line == b.line && a.column < b.column);
}

Terminal::SelectionPoint Terminal::ToAbsolute(int row, int column) const
{
    row = std::clamp(row, 0, std::max(0, rows - 1));
    column = std::clamp(column, 0, std::max(0, columns - 1));
    return SelectionPoint{
        static_cast<long>(scrollback.size()) + row - viewOffset,
        column,
    };
}

bool Terminal::GetAbsoluteCell(long line, int column, VTermScreenCell& cell) const
{
    if (line < 0 || column < 0 || column >= columns)
        return false;

    const long history = static_cast<long>(scrollback.size());
    if (line >= history)
        return GetCell(static_cast<int>(line - history), column, cell);

    const auto& cells = scrollback[static_cast<std::size_t>(line)];
    cell = column < static_cast<int>(cells.size()) ? cells[column] : BlankCell();
    return true;
}

bool Terminal::GetSelectionRange(SelectionPoint& start, SelectionPoint& end) const
{
    if (!selectionActive)
        return false;

    start = selectionAnchor;
    end = selectionEnd;
    if (Before(end, start))
        std::swap(start, end);
    return true;
}

void Terminal::BeginSelection(int row, int column)
{
    if (selectionActive)
        damaged = true;
    selectionActive = false;
    selectionAnchor = ToAbsolute(row, column);
    selectionEnd = selectionAnchor;
}

void Terminal::UpdateSelection(int row, int column)
{
    const SelectionPoint point = ToAbsolute(row, column);

    // A plain click (no movement) is not a selection.
    if (!selectionActive && point.line == selectionAnchor.line &&
        point.column == selectionAnchor.column)
        return;

    if (!selectionActive || point.line != selectionEnd.line ||
        point.column != selectionEnd.column)
        damaged = true;

    selectionEnd = point;
    selectionActive = true;
}

void Terminal::ClearSelection()
{
    if (selectionActive)
        damaged = true;
    selectionActive = false;
}

bool Terminal::HasSelection() const
{
    return selectionActive;
}

bool Terminal::IsSelected(int row, int column) const
{
    SelectionPoint start;
    SelectionPoint end;
    if (!GetSelectionRange(start, end))
        return false;

    const SelectionPoint point = ToAbsolute(row, column);
    return !Before(point, start) && !Before(end, point);
}

std::string Terminal::GetSelectedText() const
{
    SelectionPoint start;
    SelectionPoint end;
    if (!GetSelectionRange(start, end))
        return {};

    std::string text;
    for (long line = start.line; line <= end.line; ++line)
    {
        const int first = line == start.line ? start.column : 0;
        const int last = line == end.line ? end.column : columns - 1;

        std::string lineText;
        for (int column = first; column <= last; ++column)
        {
            VTermScreenCell cell{};
            if (!GetAbsoluteCell(line, column, cell))
                break;

            // Right half of a double-width character.
            if (cell.chars[0] == static_cast<std::uint32_t>(-1))
                continue;

            if (cell.chars[0] == 0)
            {
                lineText += ' ';
                continue;
            }

            for (std::uint32_t character : cell.chars)
            {
                if (character == 0)
                    break;
                AppendUTF8(character, lineText);
            }
        }

        while (!lineText.empty() && lineText.back() == ' ')
            lineText.pop_back();

        text += lineText;
        if (line != end.line)
            text += '\n';
    }
    return text;
}

void Terminal::Paste(const std::string& text)
{
    if (!terminal || text.empty())
        return;

    // Terminals want CR for Enter. Also drop ESC so pasted text can't
    // smuggle in escape sequences (e.g. a fake end-of-paste marker).
    std::string cleaned;
    cleaned.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '\033')
            continue;
        if (c == '\r')
        {
            cleaned += '\r';
            if (i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
        }
        else if (c == '\n')
            cleaned += '\r';
        else
            cleaned += c;
    }

    ScrollToBottom();
    ClearSelection();

    vterm_keyboard_start_paste(terminal);
    SendInput(cleaned.data(), cleaned.size());
    vterm_keyboard_end_paste(terminal);
}

void Terminal::Output(const char* data, std::size_t size, void* user)
{
    static_cast<Terminal*>(user)->SendInput(data, size);
}

int Terminal::Damage(VTermRect, void* user)
{
    static_cast<Terminal*>(user)->damaged = true;
    return 1;
}

int Terminal::MoveCursor(VTermPos, VTermPos, int, void* user)
{
    static_cast<Terminal*>(user)->damaged = true;
    return 1;
}

int Terminal::SetTermProperty(VTermProp property, VTermValue* value, void* user)
{
    auto* self = static_cast<Terminal*>(user);

    switch (property)
    {
        case VTERM_PROP_TITLE:
        {
            // Titles can arrive in fragments (libvterm 0.3+).
            if (value->string.initial)
                self->pendingTitle.clear();
            if (value->string.str && value->string.len > 0)
                self->pendingTitle.append(value->string.str, value->string.len);
            if (value->string.final)
            {
                self->title = self->pendingTitle.empty()
                    ? std::string(DEFAULT_TITLE)
                    : self->pendingTitle;
            }
            return 1;
        }

        case VTERM_PROP_ICONNAME:
            return 1;

        case VTERM_PROP_CURSORVISIBLE:
            self->cursorVisible = value->boolean != 0;
            self->damaged = true;
            return 1;

        case VTERM_PROP_CURSORBLINK:
            self->cursorBlink = value->boolean != 0;
            self->damaged = true;
            return 1;

        case VTERM_PROP_CURSORSHAPE:
            self->cursorShape = value->number;
            self->damaged = true;
            return 1;

        case VTERM_PROP_MOUSE:
            self->mouseMode = value->number;
            return 1;

        case VTERM_PROP_ALTSCREEN:
            self->altScreen = value->boolean != 0;
            self->selectionActive = false;
            self->viewOffset = 0;
            self->damaged = true;
            return 1;

        default:
            return 0;
    }
}

int Terminal::PushLine(int cols, const VTermScreenCell* cells, void* user)
{
    auto* self = static_cast<Terminal*>(user);

    // libvterm frees `cells` after this call, so copy.
    self->scrollback.emplace_back(cells, cells + cols);

    if (self->scrollback.size() > MAX_SCROLLBACK)
    {
        self->scrollback.pop_front();
        // Everything shifted down by one; keep the selection on the same text.
        self->selectionAnchor.line = std::max(0L, self->selectionAnchor.line - 1);
        self->selectionEnd.line = std::max(0L, self->selectionEnd.line - 1);
    }

    // Keep the view pinned on the same content while scrolled up.
    if (self->viewOffset > 0)
        self->viewOffset = std::min(self->viewOffset + 1,
                                    static_cast<int>(self->scrollback.size()));
    return 1;
}

int Terminal::PopLine(int cols, VTermScreenCell* cells, void* user)
{
    auto* self = static_cast<Terminal*>(user);
    if (self->scrollback.empty())
        return 0;

    const auto& line = self->scrollback.back();
    for (int i = 0; i < cols; ++i)
        cells[i] = i < static_cast<int>(line.size()) ? line[i] : BlankCell();

    self->scrollback.pop_back();
    self->viewOffset = std::min(self->viewOffset,
                                static_cast<int>(self->scrollback.size()));
    return 1;
}
