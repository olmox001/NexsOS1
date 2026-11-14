# 2026-07-11 — SDL2 timer backend over OS1 time primitives

New increment record (documentation is append-only).

## What changed

The SDL timer subsystem is now real on NexsOS instead of
`SDL_TIMERS_DISABLED` stubs:

- `user/sys/lib/portability/sdl2/SDL_nexsostimer.c` (NexsOS-owned overlay
  source, SDL tree untouched) implements the SDL platform timer interface:
  `SDL_GetTicks64` and the performance counters over `os1_mono_ns()`
  (monotonic nanoseconds, TIMER-MODEL §4) and `SDL_Delay` over
  `_sys_nanosleep`. Frequency is 1 GHz (nanosecond counter).
- `SDL_config_nexsos.h` drops `SDL_TIMERS_DISABLED`; SDL's
  `timer/dummy/SDL_systimer.c` now compiles empty and the overlay provides
  the symbols (verified with nm: all timer entry points resolve to
  `SDL_nexsostimer.o`, dummy contributes none).
- `sdltest` paces with `SDL_Delay(16)` and reports
  `120 frames in <N> ms (SDL_GetTicks64)` — expected ≈2000 ms, proving both
  the delay and the tick source against real time.

Limit: `SDL_AddTimer` callbacks need threads, which stay disabled;
`SDL_Init(SDL_INIT_TIMER)` reports that at runtime. `SDL_GetTicks`/`SDL_Delay`
do not depend on the timer thread.

## Validation

`make` and `make ARCH=amd64` fully green after a clean SDL rebuild on both
architectures; timer symbols verified in the archive. Runtime check for the
maintainer's next `make run`: launch `sdltest`, confirm the white bar sweeps
smoothly (~60 FPS) and the serial line reports ≈2000 ms for 120 frames on
both architectures.
