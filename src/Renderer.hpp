#pragma once

#include "Terminal.hpp"

#include <SDL.h>
#include <SDL_ttf.h>

#include <array>
#include <cstdint>
#include <unordered_map>

class Renderer
{
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Initialize(SDL_Renderer* renderer, TTF_Font* font, int padding,
                    int cellWidth, int cellHeight);
    void Shutdown();
    void Render(Terminal& terminal);

private:
    struct GlyphKey
    {
        std::array<std::uint32_t, VTERM_MAX_CHARS_PER_CELL> chars{};
        bool bold = false;
        bool italic = false;

        bool operator==(const GlyphKey&) const = default;
    };

    struct GlyphKeyHash
    {
        std::size_t operator()(const GlyphKey& key) const noexcept;
    };

    struct Glyph
    {
        SDL_Texture* texture = nullptr;
        int width = 0;
        int height = 0;
    };

    SDL_Color ResolveColor(const Terminal& terminal, const VTermColor& color,
                           bool foreground) const;
    Glyph* GetGlyph(const VTermScreenCell& cell);

    SDL_Renderer* renderer = nullptr;
    TTF_Font* font = nullptr;
    int padding = 0;
    int cellWidth = 0;
    int cellHeight = 0;
    std::unordered_map<GlyphKey, Glyph, GlyphKeyHash> glyphs;
};
