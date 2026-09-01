#include "ui.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#include <SDL2/SDL_ttf.h>
#include <switch.h>

#define FONT_COUNT 3

static TTF_Font *g_fonts[FONT_COUNT];
static int g_ready = 0;
static SDL_Renderer *g_renderer = NULL;

static TTF_Font *font_of(UiFont f);

/* --- text, rendered once and kept -------------------------------------
 *
 * Every string used to be rasterised by FreeType and uploaded to the GPU
 * on every single frame, then thrown away. With the on-screen pad's
 * dozen labels and the diagnostics' four lines that is fifteen glyph
 * runs and fifteen texture uploads per image, sixty times a second, for
 * text that changes at most once a second -- and it showed up as video
 * lag, because the frame loop draws the picture and the interface on the
 * same thread.
 *
 * Almost every one of these strings is either constant or changes
 * rarely, so keeping the texture until the string does is nearly free
 * and removes the work entirely. */
#define TEXT_CACHE_SIZE 64
#define TEXT_CACHE_MAX_LEN 120

typedef struct {
    char text[TEXT_CACHE_MAX_LEN];
    UiFont font;
    Uint32 colour;
    SDL_Texture *tex;
    int w, h;
    Uint32 used;
} TextEntry;

static TextEntry g_cache[TEXT_CACHE_SIZE];
static Uint32 g_cache_clock = 0;

static Uint32 pack_colour(SDL_Color c) {
    return ((Uint32)c.r << 24) | ((Uint32)c.g << 16) | ((Uint32)c.b << 8) | c.a;
}

static void cache_clear(void) {
    for (int i = 0; i < TEXT_CACHE_SIZE; i++) {
        if (g_cache[i].tex) {
            SDL_DestroyTexture(g_cache[i].tex);
        }
    }
    memset(g_cache, 0, sizeof(g_cache));
}

/* The texture for this string, rendering it only if it is not already
 * here. Returns NULL if it cannot be rendered at all. */
static TextEntry *cache_get(UiFont f, SDL_Color colour, const char *text) {
    const Uint32 key = pack_colour(colour);
    int free_slot = -1, oldest = 0;

    for (int i = 0; i < TEXT_CACHE_SIZE; i++) {
        TextEntry *e = &g_cache[i];
        if (!e->tex) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (e->font == f && e->colour == key && strcmp(e->text, text) == 0) {
            e->used = ++g_cache_clock;
            return e;
        }
        if (e->used < g_cache[oldest].used) {
            oldest = i;
        }
    }

    TTF_Font *font = font_of(f);
    if (!font) {
        return NULL;
    }
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, colour);
    if (!surface) {
        return NULL;
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(g_renderer, surface);
    const int w = surface->w, h = surface->h;
    SDL_FreeSurface(surface);
    if (!tex) {
        return NULL;
    }

    /* The least recently drawn is the one to give up. */
    TextEntry *e = &g_cache[free_slot >= 0 ? free_slot : oldest];
    if (e->tex) {
        SDL_DestroyTexture(e->tex);
    }
    snprintf(e->text, sizeof(e->text), "%s", text);
    e->font = f;
    e->colour = key;
    e->tex = tex;
    e->w = w;
    e->h = h;
    e->used = ++g_cache_clock;
    return e;
}

/* --- one disc, drawn many times --------------------------------------
 *
 * A filled circle was a horizontal fill per row: a stick is eighty rows,
 * and the overlay came to roughly nine hundred draw calls an image. One
 * texture stretched to the right size is one. */
#define DISC_SIZE 256
static SDL_Texture *g_disc = NULL;

static void disc_init(SDL_Renderer *r) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, DISC_SIZE, DISC_SIZE, 32,
                                                    SDL_PIXELFORMAT_RGBA32);
    if (!s) {
        return;
    }
    const double radius = DISC_SIZE / 2.0 - 0.5;
    Uint32 *px = (Uint32 *)s->pixels;
    for (int y = 0; y < DISC_SIZE; y++) {
        for (int x = 0; x < DISC_SIZE; x++) {
            const double dx = x - radius, dy = y - radius;
            const double d = sqrt(dx * dx + dy * dy);
            /* One pixel of softness at the rim, so a disc scaled down
             * does not come out with a staircase edge. */
            double a = radius - d;
            if (a > 1.0) a = 1.0;
            if (a < 0.0) a = 0.0;
            px[y * (s->pitch / 4) + x] = 0x00FFFFFFu | ((Uint32)(a * 255.0) << 24);
        }
    }
    g_disc = SDL_CreateTextureFromSurface(r, s);
    SDL_FreeSurface(s);
    if (g_disc) {
        SDL_SetTextureBlendMode(g_disc, SDL_BLENDMODE_BLEND);
    }
}

/* Sizes chosen for a screen that is 1280x720 handheld and the same
 * pixels blown up to a television when docked: what is comfortable in
 * the hands is still legible across a room. */
/* Smaller than they were: the menu grew from five entries to thirty-odd
 * across several categories, and text sized for a television across a
 * room was covering the picture it sits on. */
static const int FONT_SIZE[FONT_COUNT] = {17, 22, 32};

int ui_init(SDL_Renderer *renderer) {
    g_renderer = renderer;

    if (TTF_Init() != 0) {
        printf("TTF_Init: %s\n", TTF_GetError());
        return -1;
    }

    /* The console's own font. Shipping one instead would mean a licence
     * to carry and a worse result: this is the face the system uses, and
     * it already covers everything the system covers. */
    Result rc = plInitialize(PlServiceType_User);
    if (R_FAILED(rc)) {
        printf("plInitialize: 0x%x\n", rc);
        return -1;
    }
    PlFontData font;
    rc = plGetSharedFontByType(&font, PlSharedFontType_Standard);
    if (R_FAILED(rc)) {
        printf("plGetSharedFontByType: 0x%x\n", rc);
        return -1;
    }

    for (int i = 0; i < FONT_COUNT; i++) {
        /* A fresh RW per font: TTF_OpenFontRW takes ownership of the
         * stream, so they cannot share one. The buffer itself is owned
         * by the pl service and outlives all of them, hence freesrc=1
         * on the stream but no copy of the data. */
        SDL_RWops *rw = SDL_RWFromConstMem(font.address, font.size);
        if (!rw) {
            printf("SDL_RWFromConstMem failed\n");
            return -1;
        }
        g_fonts[i] = TTF_OpenFontRW(rw, 1, FONT_SIZE[i]);
        if (!g_fonts[i]) {
            printf("TTF_OpenFontRW(%d): %s\n", FONT_SIZE[i], TTF_GetError());
            return -1;
        }
    }

    disc_init(renderer);
    g_ready = 1;
    return 0;
}

void ui_exit(void) {
    cache_clear();
    if (g_disc) {
        SDL_DestroyTexture(g_disc);
        g_disc = NULL;
    }
    for (int i = 0; i < FONT_COUNT; i++) {
        if (g_fonts[i]) {
            TTF_CloseFont(g_fonts[i]);
            g_fonts[i] = NULL;
        }
    }
    if (g_ready) {
        plExit();
        TTF_Quit();
        g_ready = 0;
    }
}

static TTF_Font *font_of(UiFont f) {
    int i = (int)f;
    if (i < 0 || i >= FONT_COUNT) i = UI_FONT_BODY;
    return g_fonts[i];
}

/* Blended rather than solid: the menu sits over live video, and
 * hard-edged glyphs on a moving picture are unpleasant to read. */
static void draw_text(SDL_Renderer *r, UiFont f, int x, int y, SDL_Color colour,
                      const char *text, int centre_on_x) {
    if (!text || !*text || strlen(text) >= TEXT_CACHE_MAX_LEN) {
        return;
    }
    TextEntry *e = cache_get(f, colour, text);
    if (!e) {
        return;
    }
    SDL_Rect dst = {centre_on_x ? x - e->w / 2 : x, y, e->w, e->h};
    SDL_RenderCopy(r, e->tex, NULL, &dst);
}

void ui_text(SDL_Renderer *r, UiFont f, int x, int y, SDL_Color colour, const char *text) {
    draw_text(r, f, x, y, colour, text, 0);
}

void ui_text_centered(SDL_Renderer *r, UiFont f, int cx, int y, SDL_Color colour, const char *text) {
    draw_text(r, f, cx, y, colour, text, 1);
}

int ui_text_width(UiFont f, const char *text) {
    TTF_Font *font = font_of(f);
    int w = 0, h = 0;
    if (font && text && TTF_SizeUTF8(font, text, &w, &h) == 0) {
        return w;
    }
    return 0;
}

int ui_line_height(UiFont f) {
    TTF_Font *font = font_of(f);
    return font ? TTF_FontHeight(font) : 0;
}

void ui_fill(SDL_Renderer *r, int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}


void ui_fill_circle(SDL_Renderer *r, int cx, int cy, int radius, SDL_Color colour) {
    if (!g_disc) {
        return;
    }
    SDL_SetTextureColorMod(g_disc, colour.r, colour.g, colour.b);
    SDL_SetTextureAlphaMod(g_disc, colour.a);
    SDL_Rect dst = {cx - radius, cy - radius, radius * 2, radius * 2};
    SDL_RenderCopy(r, g_disc, NULL, &dst);
}

void ui_draw_circle(SDL_Renderer *r, int cx, int cy, int radius, SDL_Color colour) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, colour.r, colour.g, colour.b, colour.a);
    /* Segments proportional to the radius rather than a fixed 360: a
     * fixed count meant thirteen controls costing nine thousand draw
     * calls an image, which slowed the frame loop -- and a slow frame
     * loop is what made the menu miss button presses in the first
     * place. About one segment every three pixels of circumference is
     * indistinguishable from a curve at these sizes. */
    int steps = radius * 2;
    if (steps < 24) steps = 24;
    if (steps > 96) steps = 96;
    SDL_Point pts[97 * 2];
    for (int i = 0; i <= steps; i++) {
        const double a = (i * 2.0 * M_PI) / steps;
        pts[i].x = cx + (int)(cos(a) * radius);
        pts[i].y = cy + (int)(sin(a) * radius);
        pts[steps + 1 + i].x = cx + (int)(cos(a) * (radius - 1));
        pts[steps + 1 + i].y = cy + (int)(sin(a) * (radius - 1));
    }
    SDL_RenderDrawLines(r, pts, steps + 1);
    SDL_RenderDrawLines(r, pts + steps + 1, steps + 1);
}

void ui_outline(SDL_Renderer *r, int x, int y, int w, int h, int thickness, SDL_Color colour) {
    ui_fill(r, x, y, w, thickness, colour);
    ui_fill(r, x, y + h - thickness, w, thickness, colour);
    ui_fill(r, x, y, thickness, h, colour);
    ui_fill(r, x + w - thickness, y, thickness, h, colour);
}
