/* SDL2 NexsOS timer backend over the OS1 Tier-3 time primitives.
 * NexsOS-owned overlay source; the SDL tree stays unpatched (the dummy
 * timer file compiles empty once SDL_TIMERS_DISABLED is not defined). */
#include "../../sdl/src/SDL_internal.h"

#if defined(__NEXSOS__) && !defined(SDL_TIMERS_DISABLED)

#include "SDL_timer.h"

/* Mirrors <os1.h> (docs/TIMER-MODEL.md §4); declared directly so OS1's
 * macro surface (STACK_SIZE, FP_ONE, ...) does not leak into SDL units. */
extern unsigned long long os1_mono_ns(void);
extern void _sys_nanosleep(unsigned long long ns);

static SDL_bool ticks_started = SDL_FALSE;
static Uint64 start_ns;

void SDL_TicksInit(void)
{
    if (ticks_started) {
        return;
    }
    ticks_started = SDL_TRUE;
    start_ns = os1_mono_ns();
}

void SDL_TicksQuit(void)
{
    ticks_started = SDL_FALSE;
}

Uint64 SDL_GetTicks64(void)
{
    if (!ticks_started) {
        SDL_TicksInit();
    }
    return (Uint64)(os1_mono_ns() - start_ns) / 1000000ull;
}

Uint64 SDL_GetPerformanceCounter(void)
{
    return (Uint64)os1_mono_ns();
}

Uint64 SDL_GetPerformanceFrequency(void)
{
    return 1000000000ull; /* os1_mono_ns counts nanoseconds */
}

void SDL_Delay(Uint32 ms)
{
    _sys_nanosleep((unsigned long long)ms * 1000000ull);
}

#endif /* __NEXSOS__ && !SDL_TIMERS_DISABLED */
