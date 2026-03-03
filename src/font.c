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
#define MAX_FONTS 8

/** Unicode block range definition */
struct unicode_range {
    wchar_t start;
    wchar_t end;
};

/** Font entry with associated character ranges */
struct font_entry {
    FT_Face face;           ///< Font face instance
    char* name;             ///< Font name (for debugging)
    struct unicode_range* ranges;  ///< Array of Unicode ranges this font covers
    size_t num_ranges;      ///< Number of ranges
};

/** Font context. */
struct font {
    FT_Library lib;    ///< Font lib instance
    struct font_entry fonts[MAX_FONTS];  ///< Array of fonts
    size_t num_fonts;  ///< Number of loaded fonts
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
    char* str = strdup(range_str);
    char* token = strtok(str, ",");

    while (token && num_ranges < max_ranges) {
        char* dash = strchr(token, '-');
        if (dash) {
            *dash = '\0';
            ranges[num_ranges].start = strtol(token, NULL, 16);
            ranges[num_ranges].end = strtol(dash + 1, NULL, 16);
            num_ranges++;
        }
        token = strtok(NULL, ",");
    }

    free(str);
    return num_ranges;
}

/**
 * Check if a character is covered by a font entry
 * @param ch Unicode character
 * @param entry Font entry to check
 * @return true if character is covered
 */
static bool is_char_covered(wchar_t ch, const struct font_entry* entry)
{
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
 * Load a font face
 * @param name Font name
 * @param face Pointer to store loaded face
 * @return true if successful
 */
static bool load_font_face(const char* name, FT_Face* face)
{
    char font_file[PATH_MAX] = { 0 };

    if (!search_font_file(name, font_file, sizeof(font_file))) {
        return false;
    }

    return FT_New_Face(ctx.lib, font_file, 0, face) == 0;
}

/**
 * Add a font to the context
 * @param name Font name
 * @param ranges Unicode ranges this font covers (comma-separated hex ranges)
 * @return Index of added font, or SIZE_MAX on error
 */
static size_t add_font(const char* name, const char* ranges_str)
{
    if (ctx.num_fonts >= MAX_FONTS) {
        fprintf(stderr, "WARNING: Maximum number of fonts reached\n");
        return SIZE_MAX;
    }

    struct font_entry* entry = &ctx.fonts[ctx.num_fonts];

    // Load the font
    if (!load_font_face(name, &entry->face)) {
        fprintf(stderr, "WARNING: Unable to load font %s\n", name);
        return SIZE_MAX;
    }

    // Store font name
    entry->name = strdup(name);

    // Parse Unicode ranges
    entry->ranges = malloc(sizeof(struct unicode_range) * 16); // Max 16 ranges per font
    entry->num_ranges = parse_unicode_ranges(ranges_str, entry->ranges, 16);

    if (entry->num_ranges == 0) {
        // If no ranges specified, assume this is the default font (covers ASCII)
        entry->ranges[0].start = 0x20;
        entry->ranges[0].end = 0x7E;
        entry->num_ranges = 1;
    }

    ctx.num_fonts++;
    return ctx.num_fonts - 1;
}

/**
 * Find the best font for a character
 * @param ch Character to find font for
 * @return Font face to use, or NULL if not found
 */
static FT_Face find_font_for_char(wchar_t ch)
{
    // Try to find a font that covers this character
    for (size_t i = 0; i < ctx.num_fonts; i++) {
        if (is_char_covered(ch, &ctx.fonts[i])) {
            return ctx.fonts[i].face;
        }
    }

    // Fall back to default font (first one)
    if (ctx.num_fonts > 0) {
        return ctx.fonts[0].face;
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

    FT_Face default_face = ctx.fonts[0].face;
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
            FT_Face face = find_font_for_char(*text);
            if (face && FT_Load_Char(face, *text, FT_LOAD_RENDER) == 0) {
                const FT_GlyphSlot glyph = face->glyph;
                width += glyph->advance.x / POINT_FACTOR;
                if ((FT_Int)base_offset < glyph->bitmap_top) {
                    base_offset = glyph->bitmap_top;
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

    // load font
    if (FT_Init_FreeType(&ctx.lib) != 0) {
        fprintf(stderr, "WARNING: Unable to initialize FreeType\n");
        return;
    }

    // Parse font configurations
    // Format: font.N.name = "Font Name"
    //         font.N.ranges = "4E00-9FFF,3040-30FF" (hex ranges, comma-separated)

    char key_name[64];
    char key_ranges[64];
    int font_idx = 0;

    while (font_idx < MAX_FONTS) {
        snprintf(key_name, sizeof(key_name), "font.%d.name", font_idx + 1);

        const char* name = config_get(section, key_name);
        if (!name) {
            break;
        }

        snprintf(key_ranges, sizeof(key_ranges), "font.%d.ranges", font_idx + 1);
        const char* ranges = config_get(section, key_ranges);
        if (!ranges) {
            ranges = "20-7E";  // ASCII
        }

        if (add_font(name, ranges) == SIZE_MAX) {
            fprintf(stderr, "WARNING: Failed to load font %d: %s\n", font_idx + 1, name);
        }

        font_idx++;
    }

    // If no fonts loaded, try default font
    if (ctx.num_fonts == 0) {
        const char* default_font = config_get(section, CFG_FONT_NAME);
        if (default_font) {
            add_font(default_font, "20-7E");
        }
    }

    if (ctx.num_fonts == 0) {
        fprintf(stderr, "WARNING: No fonts loaded\n");
        FT_Done_FreeType(ctx.lib);
        return;
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
        FT_Set_Char_Size(ctx.fonts[i].face, ctx.size * POINT_FACTOR, 0, 96 * scale, 0);
    }
}

void font_destroy(void)
{
    for (size_t i = 0; i < ctx.num_fonts; i++) {
        if (ctx.fonts[i].face)
            FT_Done_Face(ctx.fonts[i].face);
        free(ctx.fonts[i].name);
        free(ctx.fonts[i].ranges);
    }
    if (ctx.lib) {
        FT_Done_FreeType(ctx.lib);
    }
}

bool font_render(const char* text, struct text_surface* surface)
{
    if (ctx.num_fonts == 0) {
        return false;
    }
    if (!text || !*text) {
        surface->width = 0;
        surface->height = 0;
        return true;
    }

    FT_Face default_face = ctx.fonts[0].face;
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
            FT_Face face = find_font_for_char(*it);
            if (face && FT_Load_Char(face, *it, FT_LOAD_RENDER) == 0) {
                const FT_GlyphSlot glyph = face->glyph;
                const FT_Bitmap* bmp = &glyph->bitmap;
                const size_t off_y = base_offset - glyph->bitmap_top;
                size_t size = (x + bmp->width < surface->width) ?
                              bmp->width : surface->width - x;

                for (size_t y = 0; y < bmp->rows; ++y) {
                    const size_t offset = (y + off_y) * surface->width + x;
                    uint8_t* dst = &surface->data[offset + glyph->bitmap_left];
                    memcpy(dst, &bmp->buffer[y * bmp->pitch], size);
                }

                x += glyph->advance.x / POINT_FACTOR;
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
