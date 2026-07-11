/*
 * user/bin/sdltest.c
 * First SDL2 application on NexsOS: exercises the whole port end to end —
 * libSDL2.a (cross-built, unpatched submodule) -> 'nexsos' video driver
 * overlay -> os1_video_platform -> OS1 window/blit/input ABI.
 *
 * Draws a STATIC gradient with R/G/B channel squares and one moving white
 * bar through SDL_GetWindowSurface/SDL_UpdateWindowSurface (the software
 * framebuffer path) and quits on Escape or SDL_QUIT.  Frame pacing uses
 * SDL_Delay via the NexsOS SDL timer backend (os1_mono_ns/_sys_nanosleep).
 */
#include <os1.h>

#include "SDL.h"

#define TEST_W 320
#define TEST_H 240

int main(void) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    printf("[SDLTest] SDL_Init failed: %s\n", SDL_GetError());
    exit(1);
  }
  printf("[SDLTest] SDL %d.%d.%d, video driver: %s\n", SDL_MAJOR_VERSION,
         SDL_MINOR_VERSION, SDL_PATCHLEVEL, SDL_GetCurrentVideoDriver());

  SDL_Window *window =
      SDL_CreateWindow("SDL2 on NexsOS", 80, 80, TEST_W, TEST_H, 0);
  if (!window) {
    printf("[SDLTest] SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    exit(1);
  }

  SDL_Surface *surface = SDL_GetWindowSurface(window);
  if (!surface) {
    printf("[SDLTest] SDL_GetWindowSurface failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    exit(1);
  }

  int running = 1;
  int frame = 0;
  /* Input-test state: crosshair follows SDL_MOUSEMOTION, the magenta square
   * moves with the arrow keys, clicks recolour the border (left=green,
   * right=red).  Everything is also logged on the serial console. */
  int cur_x = TEST_W / 2, cur_y = TEST_H / 2;
  int sq_x = TEST_W / 2 - 10, sq_y = TEST_H / 2 - 10;
  Uint8 border_r = 255, border_g = 255, border_b = 0;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT)
        running = 0;
      if (event.type == SDL_MOUSEMOTION) {
        cur_x = event.motion.x;
        cur_y = event.motion.y;
      }
      if (event.type == SDL_MOUSEBUTTONDOWN) {
        printf("[SDLTest] mouse button %d at %d,%d\n", event.button.button,
               event.button.x, event.button.y);
        if (event.button.button == SDL_BUTTON_LEFT) {
          border_r = 0; border_g = 255; border_b = 0;
        } else if (event.button.button == SDL_BUTTON_RIGHT) {
          border_r = 255; border_g = 0; border_b = 0;
        }
      }
      if (event.type == SDL_KEYDOWN) {
        SDL_Scancode sc = event.key.keysym.scancode;
        printf("[SDLTest] key down: scancode %d (%s)\n", (int)sc,
               SDL_GetScancodeName(sc));
        if (sc == SDL_SCANCODE_ESCAPE)
          running = 0;
        if (sc == SDL_SCANCODE_LEFT)
          sq_x -= 8;
        if (sc == SDL_SCANCODE_RIGHT)
          sq_x += 8;
        if (sc == SDL_SCANCODE_UP)
          sq_y -= 8;
        if (sc == SDL_SCANCODE_DOWN)
          sq_y += 8;
        if (sq_x < 0) sq_x = 0;
        if (sq_x > TEST_W - 20) sq_x = TEST_W - 20;
        if (sq_y < 0) sq_y = 0;
        if (sq_y > TEST_H - 20) sq_y = TEST_H - 20;
      }
      if (event.type == SDL_TEXTINPUT)
        printf("[SDLTest] text: %s\n", event.text.text);
    }

    /* STATIC gradient (any apparent motion of it is a presentation bug) +
     * three pure-channel squares for colour verification; the white bar is
     * the only element meant to move. */
    Uint32 *pixels = (Uint32 *)surface->pixels;
    int pitch_px = surface->pitch / 4;
    for (int y = 0; y < TEST_H; y++) {
      Uint8 g = (Uint8)((y * 255) / TEST_H);
      for (int x = 0; x < TEST_W; x++) {
        Uint8 r = (Uint8)((x * 255) / TEST_W);
        pixels[y * pitch_px + x] = 0xFF000000u | ((Uint32)r << 16) |
                                   ((Uint32)g << 8) | (Uint32)(255 - r);
      }
    }
    /* Channel check: pure red, green, blue squares (left to right). */
    SDL_Rect red = {12, 12, 32, 32}, green = {52, 12, 32, 32},
             blue = {92, 12, 32, 32};
    SDL_FillRect(surface, &red, SDL_MapRGB(surface->format, 255, 0, 0));
    SDL_FillRect(surface, &green, SDL_MapRGB(surface->format, 0, 255, 0));
    SDL_FillRect(surface, &blue, SDL_MapRGB(surface->format, 0, 0, 255));

    int bar_x = (frame * 3) % TEST_W;
    SDL_Rect bar = {bar_x, TEST_H - 24, 8, 24};
    SDL_FillRect(surface, &bar, SDL_MapRGB(surface->format, 255, 255, 255));

    /* Input visualization: arrow-key square, click-coloured border and the
     * mouse-motion crosshair. */
    SDL_Rect square = {sq_x, sq_y, 20, 20};
    SDL_FillRect(surface, &square, SDL_MapRGB(surface->format, 255, 0, 255));
    Uint32 border_color =
        SDL_MapRGB(surface->format, border_r, border_g, border_b);
    SDL_Rect btop = {0, 0, TEST_W, 3}, bbot = {0, TEST_H - 3, TEST_W, 3};
    SDL_Rect blft = {0, 0, 3, TEST_H}, brgt = {TEST_W - 3, 0, 3, TEST_H};
    SDL_FillRect(surface, &btop, border_color);
    SDL_FillRect(surface, &bbot, border_color);
    SDL_FillRect(surface, &blft, border_color);
    SDL_FillRect(surface, &brgt, border_color);
    Uint32 black = SDL_MapRGB(surface->format, 0, 0, 0);
    SDL_Rect ch_h = {cur_x - 6, cur_y, 13, 1}, ch_v = {cur_x, cur_y - 6, 1, 13};
    SDL_FillRect(surface, &ch_h, black);
    SDL_FillRect(surface, &ch_v, black);

    if (SDL_UpdateWindowSurface(window) != 0) {
      printf("[SDLTest] present failed: %s\n", SDL_GetError());
      running = 0;
    }

    frame++;
    if (frame == 1)
      printf("[SDLTest] first frame presented via driver '%s'\n",
             SDL_GetCurrentVideoDriver());
    if (frame == 120)
      printf("[SDLTest] 120 frames in %u ms (SDL_GetTicks64)\n",
             (unsigned)SDL_GetTicks64());

    SDL_Delay(16); /* ~60 FPS through the NexsOS SDL timer backend */
  }

  SDL_DestroyWindow(window);
  SDL_Quit();
  printf("[SDLTest] clean shutdown\n");
  exit(0);
  return 0;
}
