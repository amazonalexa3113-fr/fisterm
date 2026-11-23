#include "Renderer.hpp"

#include <cstring>

namespace
{
constexpr SDL_Color DEFAULT_FOREGROUND{255, 255, 255, 255};
constexpr SDL_Color DEFAULT_BACKGROUND{0, 0, 0, 255};

void AppendUTF8(std::uint32_t codepoint, char*& output)
{
    if (codepoint <= 0x7F)
        *output++ = static_cast<char>(codepoint);
    else if (codepoint <= 0x7FF)
    {
        *output++ = static_cast<char>(0xC0 | (codepoint >> 6));
        *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    else if (codepoint <= 0xFFFF)
    {
        *output++ = static_cast<char>(0xE0 | (codepoint >> 12));
        *output++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    else
    {
        *output++ = static_cast<char>(0xF0 | (codepoint >> 18));
        *output++ = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        *output++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
    }
}
}

Renderer::~Renderer()
{
    Shutdown();
}

bool Renderer::Initialize(SDL_Renderer* sdlRenderer, TTF_Font* sdlFont,
                          int cellPadding, int glyphWidth, int glyphHeight)
{
    if (!sdlRenderer || !sdlFont || glyphWidth < 1 || glyphHeight < 1)
        return false;

    Shutdown();
    renderer = sdlRenderer;
    font = sdlFont;
    padding = cellPadding;
    cellWidth = glyphWidth;
    cellHeight = glyphHeight;
    return true;
}

void Renderer::Shutdown()
{
    for (auto& [key, glyph] : glyphs)
    {
        (void)key;
        if (glyph.texture)
            SDL_DestroyTexture(glyph.texture);
    }
    glyphs.clear();
    renderer = nullptr;
    font = nullptr;
}

void Renderer::Render(Terminal& terminal)
{
    if (!renderer || !font)
        return;

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    for (int row = 0; row < terminal.GetRows(); ++row)
    {
        for (int column = 0; column < terminal.GetColumns(); ++column)
        {
            VTermScreenCell cell{};
            // chars[0] == (uint32_t)-1 marks the right half of a double-width char.
            if (!terminal.GetViewCell(row, column, cell) ||
                cell.chars[0] == static_cast<std::uint32_t>(-1))
                continue;

            const int x = padding + column * cellWidth;
            const int y = padding + row * cellHeight;
            const int span = cell.width > 1 ? cell.width : 1;
            SDL_Rect cellRect{x, y, cellWidth * span, cellHeight};
            // Selected text is drawn inverted (flips again if already reversed).
            const bool inverted = (cell.attrs.reverse != 0) != terminal.IsSelected(row, column);
            const VTermColor& foregroundColor = inverted ? cell.bg : cell.fg;
            const VTermColor& backgroundColor = inverted ? cell.fg : cell.bg;

            SDL_Color background = ResolveColor(terminal, backgroundColor, false);
            SDL_SetRenderDrawColor(renderer, background.r, background.g,
                                   background.b, background.a);
            SDL_RenderFillRect(renderer, &cellRect);

            bool hasCharacter = false;
            for (std::uint32_t character : cell.chars)
            {
                if (character == 0)
                    break;
                hasCharacter = true;
            }

            if (hasCharacter && !cell.attrs.conceal)
            {
                if (Glyph* glyph = GetGlyph(cell))
                {
                    SDL_Color foreground = ResolveColor(terminal, foregroundColor, true);
                    SDL_SetTextureColorMod(glyph->texture, foreground.r,
                                           foreground.g, foreground.b);
                    SDL_SetTextureAlphaMod(glyph->texture, foreground.a);
                    SDL_Rect destination{x, y, glyph->width, glyph->height};
                    SDL_RenderCopy(renderer, glyph->texture, nullptr, &destination);
                }
            }

            if (cell.attrs.underline != VTERM_UNDERLINE_OFF)
            {
                SDL_Color foreground = ResolveColor(terminal, foregroundColor, true);
                SDL_SetRenderDrawColor(renderer, foreground.r, foreground.g,
                                       foreground.b, foreground.a);
                const int underlineY = y + cellHeight - 2;
                SDL_RenderDrawLine(renderer, x, underlineY,
                                   x + cellWidth * span - 1, underlineY);
                if (cell.attrs.underline == VTERM_UNDERLINE_DOUBLE)
                    SDL_RenderDrawLine(renderer, x, underlineY - 2,
                                       x + cellWidth * span - 1, underlineY - 2);
            }
        }
    }

    int cursorRow = 0;
    int cursorColumn = 0;
    terminal.GetCursor(cursorRow, cursorColumn);
    cursorRow += terminal.GetViewOffset();
    if (terminal.IsCursorVisible() && cursorRow >= 0 && cursorRow < terminal.GetRows())
    {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_Rect cursorRect{
            padding + cursorColumn * cellWidth,
            padding + cursorRow * cellHeight + cellHeight - 2,
            cellWidth,
            2,
        };
        SDL_RenderFillRect(renderer, &cursorRect);
    }
    SDL_RenderPresent(renderer);
    terminal.ClearDamage();
}

SDL_Color Renderer::ResolveColor(const Terminal& terminal, const VTermColor& color,
                                 bool foreground) const
{
    if (VTERM_COLOR_IS_DEFAULT_FG(&color))
        return DEFAULT_FOREGROUND;
    if (VTERM_COLOR_IS_DEFAULT_BG(&color))
        return DEFAULT_BACKGROUND;

    Uint8 red = 255;
    Uint8 green = 255;
    Uint8 blue = 255;
    if (!terminal.ConvertColorToRGB(color, red, green, blue))
        return foreground ? DEFAULT_FOREGROUND : DEFAULT_BACKGROUND;
    return SDL_Color{red, green, blue, 255};
}

Renderer::Glyph* Renderer::GetGlyph(const VTermScreenCell& cell)
{
    GlyphKey key{};
    std::memcpy(key.chars.data(), cell.chars, sizeof(cell.chars));
    key.bold = cell.attrs.bold;

    auto found = glyphs.find(key);
    if (found != glyphs.end())
        return &found->second;

    char utf8[VTERM_MAX_CHARS_PER_CELL * 4 + 1]{};
    char* output = utf8;
    for (std::uint32_t character : cell.chars)
    {
        if (character == 0)
            break;
        AppendUTF8(character, output);
    }
    *output = '\0';

    const int originalStyle = TTF_GetFontStyle(font);
    TTF_SetFontStyle(font, key.bold ? originalStyle | TTF_STYLE_BOLD
                                    : originalStyle & ~TTF_STYLE_BOLD);
    SDL_Surface* surface = TTF_RenderUTF8_Blended(
        font, utf8, SDL_Color{255, 255, 255, 255});
    TTF_SetFontStyle(font, originalStyle);
    if (!surface)
        return nullptr;

    Glyph glyph;
    glyph.texture = SDL_CreateTextureFromSurface(renderer, surface);
    glyph.width = surface->w;
    glyph.height = surface->h;
    SDL_FreeSurface(surface);
    if (!glyph.texture)
        return nullptr;

    return &glyphs.emplace(key, glyph).first->second;
}

std::size_t Renderer::GlyphKeyHash::operator()(const GlyphKey& key) const noexcept
{
    std::size_t hash = key.bold ? 1 : 0;
    for (std::uint32_t character : key.chars)
        hash ^= static_cast<std::size_t>(character) + 0x9e3779b9 +
                (hash << 6) + (hash >> 2);
    return hash;
}
