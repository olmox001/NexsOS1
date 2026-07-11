/*
 * NexsOS build configuration for the SDL2 cross-build.
 *
 * This file is owned by NexsOS and injected with `-include` BEFORE any SDL
 * source line; defining SDL_config_h_ here makes the submodule's own
 * include/SDL_config.h a no-op, so the SDL tree needs no NexsOS platform
 * branch.  Together with the DUMMY_bootstrap rename below, the SDL submodule
 * builds completely unpatched — per the programme's non-negotiable
 * constraint AND upstream SDL's policy that no generated code enters its
 * tree (see the submodule's CLAUDE.md).
 */
#ifndef SDL_config_nexsos_h_
#define SDL_config_nexsos_h_
#define SDL_config_h_ /* neutralise include/SDL_config.h in the SDL tree */

/* Freestanding GCC provides these; the OS1 onion libc provides stdlib. */
#define HAVE_STDARG_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1

/* OS1 userland allocator: malloc/calloc/realloc/free from <stdlib.h>
 * (include/api), so SDL does not drag in its internal dlmalloc (which would
 * need sbrk/mmap). */
#define HAVE_STDLIB_H 1
#define HAVE_MALLOC 1
#define SDL_NEXSOS 1

#ifdef __GNUC__
#define HAVE_GCC_SYNC_LOCK_TEST_AND_SET 1
#endif

/* Static freestanding archive: OS1 has no dlopen, which turns the dynamic-API
 * trampoline off through SDL_dynapi.h's own DYNAPI_NEEDS_DLOPEN &&
 * !HAVE_DLOPEN branch (SDL_DYNAMIC_API must not be forced directly). */
#define DYNAPI_NEEDS_DLOPEN 1

/* Video: the bootstrap array in SDL_video.c has no NexsOS entry and must not
 * be patched.  Enabling the DUMMY slot and renaming its symbol routes that
 * entry to the NexsOS driver from the portability overlay; the real dummy
 * driver sources (src/video/dummy/) are simply not compiled. */
#define SDL_VIDEO_DRIVER_DUMMY 1
#define DUMMY_bootstrap NEXSOS_bootstrap
#define SDL_VIDEO_DRIVER_NEXSOS 1

/* Software rendering only: the OS1 backend presents ARGB8888 CPU surfaces. */
#define SDL_VIDEO_RENDER_SW 1

/* Everything below stays dummy/disabled until the matching OS1 capability
 * exists (audio device, threads, timers, dynamic loading...). */
#define SDL_AUDIO_DRIVER_DUMMY 1
#define SDL_JOYSTICK_DISABLED 1
#define SDL_HAPTIC_DISABLED 1
#define SDL_HIDAPI_DISABLED 1
#define SDL_SENSOR_DISABLED 1
#define SDL_LOADSO_DISABLED 1
#define SDL_THREADS_DISABLED 1
#define SDL_FILESYSTEM_DUMMY 1

/* Timers are REAL: SDL_nexsostimer.c in the overlay maps
 * SDL_GetTicks64/SDL_Delay/performance counters onto os1_mono_ns and
 * _sys_nanosleep.  (SDL_AddTimer callbacks still need threads, which stay
 * disabled; SDL_Init(SDL_INIT_TIMER) reports that at runtime.) */

#endif /* SDL_config_nexsos_h_ */
