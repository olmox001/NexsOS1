# Include this file from the NexsOS SDL2 build integration; do not edit SDL.
SDL_NEXSOS_OVERLAY_SRCS := \
  user/sys/lib/portability/sdl2/SDL_nexsosvideo.c \
  user/sys/lib/portability/sdl2/SDL_nexsosframebuffer.c \
  user/sys/lib/portability/sdl2/SDL_nexsosevents.c
SDL_NEXSOS_OVERLAY_CPPFLAGS := -DSDL_VIDEO_DRIVER_NEXSOS
