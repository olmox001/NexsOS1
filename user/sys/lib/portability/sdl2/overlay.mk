# Include this file from the NexsOS SDL2 build integration; do not edit SDL.
# The build must inject SDL_config_nexsos.h with `-include` (it neutralises
# the SDL tree's own SDL_config.h and routes the DUMMY bootstrap slot to the
# NexsOS driver), keeping the submodule completely unpatched.
SDL_NEXSOS_OVERLAY_SRCS := \
  user/sys/lib/portability/sdl2/SDL_nexsosvideo.c \
  user/sys/lib/portability/sdl2/SDL_nexsosframebuffer.c \
  user/sys/lib/portability/sdl2/SDL_nexsosevents.c \
  user/sys/lib/portability/sdl2/SDL_nexsostimer.c
SDL_NEXSOS_OVERLAY_CONFIG := user/sys/lib/portability/sdl2/SDL_config_nexsos.h
SDL_NEXSOS_OVERLAY_CPPFLAGS := -D__NEXSOS__ -include $(SDL_NEXSOS_OVERLAY_CONFIG)
