#ifndef CAPTURE2SWITCH_UI_H
#define CAPTURE2SWITCH_UI_H

#include <SDL2/SDL.h>

/*
 * Text and the few shapes the interface needs.
 *
 * The font is the console's own, obtained through libnx's pl service
 * rather than shipped inside the .nro: it is already on every Switch,
 * it covers the scripts the system covers, and it keeps a font licence
 * out of this project entirely.
 */

typedef enum {
    UI_FONT_SMALL,
    UI_FONT_BODY,
    UI_FONT_TITLE
} UiFont;

int  ui_init(SDL_Renderer *renderer);
void ui_exit(void);

/* Draws `text` at (x, y), y being the TOP of the line. */
void ui_text(SDL_Renderer *r, UiFont font, int x, int y, SDL_Color colour, const char *text);

/* Same, centred horizontally on `cx`. */
void ui_text_centered(SDL_Renderer *r, UiFont font, int cx, int y, SDL_Color colour, const char *text);

/* Width the text would occupy, for laying out anything beside it. */
int  ui_text_width(UiFont font, const char *text);
int  ui_line_height(UiFont font);

void ui_fill(SDL_Renderer *r, int x, int y, int w, int h, SDL_Color colour);

/* A filled disc and its outline, which the on-screen pad is made of.
 * Drawn as horizontal spans rather than with a texture: at these sizes
 * it is a few dozen fills, and it keeps the whole overlay free of any
 * asset to ship. */
void ui_fill_circle(SDL_Renderer *r, int cx, int cy, int radius, SDL_Color colour);
void ui_draw_circle(SDL_Renderer *r, int cx, int cy, int radius, SDL_Color colour);

/* Rectangle outline, `thickness` pixels drawn inside the bounds. */
void ui_outline(SDL_Renderer *r, int x, int y, int w, int h, int thickness, SDL_Color colour);

#endif
