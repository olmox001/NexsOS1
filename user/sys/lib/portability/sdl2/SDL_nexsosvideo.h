/* SDL2 NexsOS video overlay. This file is owned by NexsOS, not SDL upstream. */
#ifndef NEXSOS_SDL2_VIDEO_H
#define NEXSOS_SDL2_VIDEO_H

#include "../../sdl/src/SDL_internal.h"
#include "../../sdl/src/video/SDL_sysvideo.h"

typedef struct NEXSOS_VideoData {
  SDL_Window *focused_window;
} NEXSOS_VideoData;

typedef struct NEXSOS_WindowData {
  int os1_window;
  SDL_Surface *framebuffer;
} NEXSOS_WindowData;

int NEXSOS_VideoInit(_THIS);
void NEXSOS_VideoQuit(_THIS);
void NEXSOS_PumpEvents(_THIS);
int NEXSOS_CreateWindow(_THIS, SDL_Window *window);
void NEXSOS_DestroyWindow(_THIS, SDL_Window *window);
void NEXSOS_SetWindowSize(_THIS, SDL_Window *window);
void NEXSOS_SetWindowTitle(_THIS, SDL_Window *window);
int NEXSOS_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format,
                                   void **pixels, int *pitch);
int NEXSOS_UpdateWindowFramebuffer(_THIS, SDL_Window *window,
                                   const SDL_Rect *rects, int numrects);
void NEXSOS_DestroyWindowFramebuffer(_THIS, SDL_Window *window);

#endif /* NEXSOS_SDL2_VIDEO_H */
