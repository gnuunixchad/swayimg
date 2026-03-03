// SPDX-License-Identifier: MIT
// Font renderer.
// Copyright (C) 2022 Artem Senichev <artemsen@gmail.com>

#include "font.h"

#include "array.h"

// font related
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H

#define POINT_FACTOR 64.0 // default points per pixel for 26.6 format
#define SPACE_WH_REL 2.0
#define BACKGROUND_PADDING 5
#define MAX_FONTS 4
#define GLYPH_CACHE_SIZE 128
#define RANGE_BITMAP_SIZE 8192 // Covers up to 0xFFFF (65535/8)
#define NO_ASCII_FONT ((size_t)-1) // Sentinel value for no ASCII font

// CJK Unicode ranges
#define CJK_RANGES "4E00-9FFF,3040-30FF,AC00-D7AF,3400-4DBF,20000-2A6DF"

struct unicode_range {
    wchar_t start;
    wchar_t end;
};

struct glyph_cache_entry {
    wchar_t ch;
    FT_Glyph glyph;
    int advance;
};

struct font_entry {
    FT_Face face;                   ///< Font face instance (NULL if not loaded)
    char* name;                     ///< Font name
    struct unicode_range* ranges;   ///< Array of Unicode ranges
    size_t num_ranges;              ///< Number of ranges
    char* font_path;                ///< Cached font path
    bool loaded;                    ///< Whether font is loaded
    uint8_t range_bitmap[RANGE_BITMAP_SIZE]; ///< Fast lookup bitmap
    struct glyph_cache_entry glyph_cache[GLYPH_CACHE_SIZE]; ///< Glyph cache
    int cache_pos;                  ///< Current cache position
    bool is_cjk;                    ///< Whether this is a CJK font
};

/** Font context. */
struct font {
    FT_Library lib;    ///< Font lib instance
    struct font_entry fonts[MAX_FONTS]; ///< Array of fonts
    size_t num_fonts;  ///< Number of loaded fonts
    size_t ascii_font_idx; ///< Index of primary ASCII/Latin font
    size_t size;       ///< Font size in points
    argb_t color;      ///< Font color
    argb_t shadow;     ///< Font shadow color
    argb_t background; ///< Font background
};

/** Global font context instance. */
static struct font ctx;

/**
 * Parse Unicode range string (e.g., "4E00-9FFF", "3040-30FF", "20-7E")
 * @param range_str Range string
 * @param ranges Output array of ranges
 * @param max_ranges Maximum number of ranges to parse
 * @return Number of ranges parsed
 */
static size_t parse_unicode_ranges(const char* range_str,
                                   struct unicode_range* ranges,
                                   size_t max_ranges)
{
    size_t num_ranges = 0;
    const char* p = range_str;

    while (*p && num_ranges < max_ranges) {
        // Skip whitespace
        while (*p == ' ') p++;
        if (!*p) break;

        // Parse start
        char* end;
        long start = strtol(p, &end, 16);
        if (end == p) break;

        p = end;
        while (*p == ' ') p++;

        // Check for dash
        if (*p != '-') break;
        p++;
        while (*p == ' ') p++;

        // Parse end
        long end_val = strtol(p, &end, 16);
        if (end == p) break;

        ranges[num_ranges].start = start;
        ranges[num_ranges].end = end_val;
        num_ranges++;

        p = end;
        while (*p == ' ') p++;

        // Skip comma
        if (*p == ',') p++;
    }

    return num_ranges;
}

/**
 * Build bitmap for fast character lookup
 * @param entry Font entry to build bitmap for
 */
static void build_range_bitmap(struct font_entry* entry)
{
    memset(entry->range_bitmap, 0, RANGE_BITMAP_SIZE);

    for (size_t i = 0; i < entry->num_ranges; i++) {
        wchar_t start = entry->ranges[i].start;
        wchar_t end = entry->ranges[i].end;

        // Only cache BMP (Basic Multilingual Plane) for performance
        if (start > 0xFFFF) continue;
        if (end > 0xFFFF) end = 0xFFFF;

        for (wchar_t ch = start; ch <= end; ch++) {
            size_t byte = ch / 8;
            size_t bit = ch % 8;
            entry->range_bitmap[byte] |= (1 << bit);
        }
    }
}

/**
 * Fast character coverage check using bitmap
 * @param ch Unicode character
 * @param entry Font entry to check
 * @return true if character is covered
 */
static bool is_char_covered_fast(wchar_t ch, const struct font_entry* entry)
{
    if (ch <= 0xFFFF) {
        size_t byte = ch / 8;
        size_t bit = ch % 8;
        return (entry->range_bitmap[byte] >> bit) & 1;
    }

    // Fall back to range check for supplementary planes
    for (size_t i = 0; i < entry->num_ranges; i++) {
        if (ch >= entry->ranges[i].start && ch <= entry->ranges[i].end) {
            return true;
        }
    }
    return false;
}

/**
 * Get path to the font file by its name.
 * @param name font name
 * @param font_file output buffer for file path
 * @param len size of buffer
 * @return false if font not found
 */
static bool search_font_file(const char* name, char* font_file, size_t len)
{
    FcConfig* fc = NULL;

    font_file[0] = 0;
    font_file[len - 1] = 0;

    if (FcInit()) {
        fc = FcInitLoadConfigAndFonts();
        if (fc) {
            FcPattern* fc_name = NULL;
            fc_name = FcNameParse((const FcChar8*)name);
            if (fc_name) {
                FcPattern* fc_font = NULL;
                FcResult result;
                FcConfigSubstitute(fc, fc_name, FcMatchPattern);
                FcDefaultSubstitute(fc_name);
                fc_font = FcFontMatch(fc, fc_name, &result);
                if (fc_font) {
                    FcChar8* path = NULL;
                    if (FcPatternGetString(fc_font, FC_FILE, 0, &path) ==
                        FcResultMatch) {
                        strncpy(font_file, (const char*)path, len - 1);
                    }
                    FcPatternDestroy(fc_font);
                }
                FcPatternDestroy(fc_name);
            }
            FcConfigDestroy(fc);
        }
        FcFini();
    }

    return *font_file;
}

/**
 * Ensure font is loaded (lazy loading)
 * @param entry Font entry to load
 * @return true if font is available
 */
static bool ensure_font_loaded(struct font_entry* entry)
{
    if (entry->loaded) return true;

    if (!entry->font_path) {
        char font_file[PATH_MAX];
        if (!search_font_file(entry->name, font_file, sizeof(font_file))) {
            return false;
        }
        entry->font_path = strdup(font_file);
    }

    if (FT_New_Face(ctx.lib, entry->font_path, 0, &entry->face) == 0) {
        entry->loaded = true;
        FT_Set_Char_Size(entry->face, ctx.size * POINT_FACTOR, 0, 96, 0);
        return true;
    }

    return false;
}

/**
 * Get cached glyph
 * @param entry Font entry
 * @param ch Character to get glyph for
 * @param advance Output advance width
 * @return Glyph or NULL if not available
 */
static FT_Glyph get_cached_glyph(struct font_entry* entry, wchar_t ch, int* advance)
{
    // Check cache
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        if (entry->glyph_cache[i].ch == ch) {
            *advance = entry->glyph_cache[i].advance;
            return entry->glyph_cache[i].glyph;
        }
    }

    // Ensure font is loaded
    if (!ensure_font_loaded(entry)) {
        return NULL;
    }

    // Load glyph
    if (FT_Load_Char(entry->face, ch, FT_LOAD_RENDER) != 0) {
        return NULL;
    }

    FT_Glyph glyph;
    if (FT_Get_Glyph(entry->face->glyph, &glyph) != 0) {
        return NULL;
    }

    *advance = entry->face->glyph->advance.x / POINT_FACTOR;

    // Add to cache (FIFO)
    int pos = entry->cache_pos++ % GLYPH_CACHE_SIZE;
    if (entry->glyph_cache[pos].glyph) {
        FT_Done_Glyph(entry->glyph_cache[pos].glyph);
    }
    entry->glyph_cache[pos].ch = ch;
    entry->glyph_cache[pos].glyph = glyph;
    entry->glyph_cache[pos].advance = *advance;

    return glyph;
}

/**
 * Add a font to the context
 * @param name Font name
 * @param ranges Unicode ranges this font covers
 * @param is_cjk Whether this is a CJK font
 * @return Index of added font, or SIZE_MAX on error
 */
static size_t add_font(const char* name, const char* ranges_str, bool is_cjk)
{
    if (ctx.num_fonts >= MAX_FONTS) {
        fprintf(stderr, "WARNING: Maximum number of fonts reached\n");
        return SIZE_MAX;
    }

    struct font_entry* entry = &ctx.fonts[ctx.num_fonts];
    memset(entry, 0, sizeof(struct font_entry));

    // Parse ranges first to know exact count
    struct unicode_range temp_ranges[16];
    size_t range_count = parse_unicode_ranges(ranges_str, temp_ranges, 16);

    if (range_count == 0) {
        range_count = 1;
        temp_ranges[0].start = 0x20;
        temp_ranges[0].end = 0x7E;
    }

    // Single allocation for name and ranges
    size_t name_len = strlen(name) + 1;
    size_t ranges_size = sizeof(struct unicode_range) * range_count;
    char* block = malloc(name_len + ranges_size);
    if (!block) {
        fprintf(stderr, "WARNING: Failed to allocate memory for font %s\n", name);
        return SIZE_MAX;
    }

    entry->name = block;
    strcpy(entry->name, name);

    entry->ranges = (struct unicode_range*)(block + name_len);
    memcpy(entry->ranges, temp_ranges, ranges_size);
    entry->num_ranges = range_count;
    entry->is_cjk = is_cjk;

    // Build fast lookup bitmap
    build_range_bitmap(entry);

    // Don't load font yet (lazy loading)
    entry->loaded = false;
    entry->face = NULL;
    entry->font_path = NULL;

    ctx.num_fonts++;
    return ctx.num_fonts - 1;
}

/**
 * Find the best font for a character
 * @param ch Character to find font for
 * @return Font entry to use, or NULL if not found
 */
static struct font_entry* find_font_entry_for_char(wchar_t ch)
{
    // ASCII/Latin range (0x20-0x7E) - always use the ASCII font
    if (ch >= 0x20 && ch <= 0x7E && ctx.ascii_font_idx != NO_ASCII_FONT) {
        return &ctx.fonts[ctx.ascii_font_idx];
    }

    // For non-ASCII characters, try CJK fonts first
    for (size_t i = 0; i < ctx.num_fonts; i++) {
        if (ctx.fonts[i].is_cjk && is_char_covered_fast(ch, &ctx.fonts[i])) {
            return &ctx.fonts[i];
        }
    }

    // If no CJK font found, try any other font
    for (size_t i = 0; i < ctx.num_fonts; i++) {
        if (i != ctx.ascii_font_idx && is_char_covered_fast(ch, &ctx.fonts[i])) {
            return &ctx.fonts[i];
        }
    }

    // Fall back to ASCII font
    if (ctx.ascii_font_idx != NO_ASCII_FONT) {
        return &ctx.fonts[ctx.ascii_font_idx];
    }

    return NULL;
}

/**
 * Calc size of the surface and allocate memory for the mask.
 * @param text string to print
 * @param surface text surface
 * @return base line offset or SIZE_MAX on errors
 */
static size_t allocate_surface(const wchar_t* text,
                               struct text_surface* surface)
{
    if (ctx.num_fonts == 0) return SIZE_MAX;

    // Ensure ASCII font is loaded for metrics
    if (ctx.ascii_font_idx == NO_ASCII_FONT ||
        !ensure_font_loaded(&ctx.fonts[ctx.ascii_font_idx])) {
        return SIZE_MAX;
    }

    FT_Face default_face = ctx.fonts[ctx.ascii_font_idx].face;
    const FT_Size_Metrics* metrics = &default_face->size->metrics;
    const size_t space_size = metrics->x_ppem / SPACE_WH_REL;
    const size_t height = metrics->height / POINT_FACTOR;
    size_t base_offset =
        (default_face->ascender * (metrics->y_scale / 65536.0)) / POINT_FACTOR;
    size_t width = 0;
    uint8_t* data = NULL;
    size_t data_size;

    // get total width
    while (*text) {
        if (*text == L' ') {
            width += space_size;
        } else {
            struct font_entry* entry = find_font_entry_for_char(*text);
            if (entry) {
                int advance;
                if (get_cached_glyph(entry, *text, &advance)) {
                    width += advance;

                    // Update base offset if needed
                    if (entry->loaded && entry->face) {
                        FT_Load_Char(entry->face, *text, FT_LOAD_RENDER);
                        if ((FT_Int)base_offset < entry->face->glyph->bitmap_top) {
                            base_offset = entry->face->glyph->bitmap_top;
                        }
                    }
                }
            }
        }
        ++text;
    }

    // allocate surface buffer
    data_size = width * height;
    if (data_size) {
        data = realloc(surface->data, data_size);
        if (!data) {
            return SIZE_MAX;
        }
        surface->width = width;
        surface->height = height;
        surface->data = data;
        memset(surface->data, 0, data_size);
    }

    return base_offset;
}

void font_init(const struct config* cfg)
{
    const struct config* section = config_section(cfg, CFG_FONT);
    const char* default_font;

    // load font
    if (FT_Init_FreeType(&ctx.lib) != 0) {
        fprintf(stderr, "WARNING: Unable to initialize FreeType\n");
        return;
    }

    ctx.ascii_font_idx = NO_ASCII_FONT;

    // Get the default configured font
    default_font = config_get(section, CFG_FONT_NAME);
    if (!default_font) {
        fprintf(stderr, "WARNING: No default font configured\n");
        FT_Done_FreeType(ctx.lib);
        return;
    }

    // Add the default font as ASCII/Latin font
    if (add_font(default_font, "20-7E", false) == SIZE_MAX) {
        fprintf(stderr, "WARNING: Failed to load default font %s\n", default_font);
        FT_Done_FreeType(ctx.lib);
        return;
    }
    ctx.ascii_font_idx = 0;

    // Try to add CJK fallback fonts
    const char* cjk_fonts[] = {
        "Noto Sans CJK JP",
        "Noto Sans CJK SC",
        "Noto Sans CJK TC",
        "WenQuanYi Zen Hei",
        "UnDotum",
        NULL
    };

    for (int i = 0; cjk_fonts[i] && ctx.num_fonts < MAX_FONTS; i++) {
        add_font(cjk_fonts[i], CJK_RANGES, true);
    }

    // set font size
    ctx.size = config_get_num(section, CFG_FONT_SIZE, 1, 1024);
    font_set_scale(1.0);

    // color/background/shadow parameters
    ctx.color = config_get_color(section, CFG_FONT_COLOR);
    ctx.background = config_get_color(section, CFG_FONT_BKG);
    ctx.shadow = config_get_color(section, CFG_FONT_SHADOW);
}

void font_set_scale(double scale)
{
    for (size_t i = 0; i < ctx.num_fonts; i++) {
        if (ctx.fonts[i].loaded) {
            FT_Set_Char_Size(ctx.fonts[i].face, ctx.size * POINT_FACTOR, 0, 96 * scale, 0);
        }
    }
}

void font_destroy(void)
{
    for (size_t i = 0; i < ctx.num_fonts; i++) {
        // Free glyph cache
        for (int j = 0; j < GLYPH_CACHE_SIZE; j++) {
            if (ctx.fonts[i].glyph_cache[j].glyph) {
                FT_Done_Glyph(ctx.fonts[i].glyph_cache[j].glyph);
            }
        }

        if (ctx.fonts[i].face) {
            FT_Done_Face(ctx.fonts[i].face);
        }
        free(ctx.fonts[i].name);  // Frees both name and ranges (single allocation)
        free(ctx.fonts[i].font_path);
    }
    if (ctx.lib) {
        FT_Done_FreeType(ctx.lib);
    }
}

bool font_render(const char* text, struct text_surface* surface)
{
    if (ctx.num_fonts == 0 || ctx.ascii_font_idx == NO_ASCII_FONT) {
        return false;
    }
    if (!text || !*text) {
        surface->width = 0;
        surface->height = 0;
        return true;
    }

    // Ensure ASCII font is loaded for metrics
    if (!ensure_font_loaded(&ctx.fonts[ctx.ascii_font_idx])) {
        return false;
    }

    FT_Face default_face = ctx.fonts[ctx.ascii_font_idx].face;
    size_t space_size = default_face->size->metrics.x_ppem / SPACE_WH_REL;
    wchar_t* wide = str_to_wide(text, NULL);

    if (!wide) {
        return false;
    }

    size_t base_offset = allocate_surface(wide, surface);
    if (base_offset == SIZE_MAX) {
        free(wide);
        return false;
    }

    // draw glyphs
    size_t x = 0;
    wchar_t* it = wide;
    while (*it) {
        if (*it == L' ') {
            x += space_size;
        } else {
            struct font_entry* entry = find_font_entry_for_char(*it);
            if (entry) {
                int advance;
                FT_Glyph glyph = get_cached_glyph(entry, *it, &advance);
                if (glyph) {
                    // Render glyph
                    FT_Vector origin = { (FT_Pos)(x * 64), (FT_Pos)(base_offset * 64) };
                    FT_Glyph_To_Bitmap(&glyph, FT_RENDER_MODE_NORMAL, &origin, 1);
                    FT_BitmapGlyph bitmap_glyph = (FT_BitmapGlyph)glyph;

                    const FT_Bitmap* bmp = &bitmap_glyph->bitmap;
                    const size_t off_y = base_offset - bitmap_glyph->top;

                    size_t size = (x + bmp->width < surface->width) ?
                                  bmp->width : surface->width - x;

                    for (size_t y = 0; y < bmp->rows; ++y) {
                        const size_t offset = (y + off_y) * surface->width + x;
                        uint8_t* dst = &surface->data[offset + bitmap_glyph->left];
                        memcpy(dst, &bmp->buffer[y * bmp->pitch], size);
                    }

                    x += advance;
                }
            }
        }
        ++it;
    }

    free(wide);

    return true;
}

void font_print(struct pixmap* wnd, ssize_t x, ssize_t y,
                const struct text_surface* text)
{
    if (ARGB_GET_A(ctx.background)) {
        pixmap_blend(wnd, x - BACKGROUND_PADDING, y,
                     text->width + BACKGROUND_PADDING * 2, text->height,
                     ctx.background);
    }

    if (ARGB_GET_A(ctx.shadow)) {
        ssize_t shadow_offset = text->height / 64;
        if (shadow_offset < 1) {
            shadow_offset = 1;
        }
        pixmap_apply_mask(wnd, x + shadow_offset, y + shadow_offset, text->data,
                          text->width, text->height, ctx.shadow);
    }

    pixmap_apply_mask(wnd, x, y, text->data, text->width, text->height,
                      ctx.color);
}
