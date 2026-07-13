/*
 * user/bin/raptor.c
 * "NexRaptor" — top-down scrolling shoot-em-up, in the spirit of the classic
 * shadow-squadron shmups of the '90s. This is the OS's hidden easter egg:
 * a single self-contained file exercising the SDL2 port exactly like
 * sdltest.c (SDL_GetWindowSurface software framebuffer path), plus the
 * fixed-point math helpers from kernel/lib/math.c (sin_fp/cos_fp) already
 * used by demo3d.c for the sine-wave enemy movement.
 *
 * Design goals: simple controls, procedurally generated waves so no two
 * runs play the same, chunky bright pixel-art sprites, and a persistent
 * high score stored in the OS registry (OS1_registry_get/set) so bragging
 * rights survive a reboot.
 *
 * WEAPON SYSTEM ("raptor / call of the shadow" style):
 *   5 weapon families, unlocked one at a time by clearing bosses:
 *     1 NORMALE  - always available, single/twin/triple shot as it levels up
 *     2 TRIPLO   - wide spread, unlocked after boss #1
 *     3 MITRA    - twin-barrel rapid fire, unlocked after boss #2
 *     4 LASER    - thin piercing beam-bullet (passes through enemies),
 *                  unlocked after boss #3
 *     5 MISSILI  - slow homing missile with splash damage, unlocked after
 *                  boss #4
 *   Number keys 1-5 switch between *unlocked* weapons at any time; picking
 *   up a weapon powerup (blue) upgrades the *currently selected* weapon's
 *   power level (1-3) instead of just handing out a new gun, so switching
 *   weapons is a real tactical choice, not a strict upgrade path.
 *   Enemies roll their own weapon from the same 5-family pool (see
 *   spawn_enemy_of_type / enemy_fire_weapon), so ships/bosses can fire
 *   triple-spread, mitragliatrice bursts, piercing lasers or homing
 *   missiles right back at you. Bosses always carry two weapon families
 *   and alternate between them over time.
 *
 * ENEMY TYPES: the original four fighters stay as "tipo 0..3"
 * (STRAIGHT/SINE/DIVER/SHOOTER), plus two "astronavi" classes (FRIGATE,
 * CRUISER - tougher hulls, better guns), METEOR (an environmental hazard
 * that never shoots: break it for a chance at a weapon/health bonus, or
 * let it drift past harmlessly if you dodge it) and BOSS (every 5th
 * level, always carrying two weapon families).
 *
 * Everything - which enemy spawns, how tough it is, which weapon it
 * carries, how often meteors fall - is picked by a tiny xorshift PRNG
 * seeded from get_time()/get_pid() and scaled by the current level, so the
 * whole run is different every time ("livelli generativi") from just a
 * handful of rules instead of hand-authored level scripts.
 *
 * Controls:
 *   Arrow keys  - move
 *   Space       - fire (auto-repeats while held; behaviour depends on the
 *                 selected weapon)
 *   1-5         - switch to weapon slot N (only if already unlocked)
 *   X / LCTRL   - smart bomb (starts with 2, earns more by clearing levels)
 *   R           - restart after Game Over
 *   Escape      - quit
 *
 * Every 5th level is a boss encounter. Everything else (enemy mix, spawn
 * rate, enemy HP/speed/weapon) scales with the level number.
 */
#include <os1.h>

#include "SDL.h"

/* ---------------------------------------------------------------------- */
/* Constants                                                               */
/* ---------------------------------------------------------------------- */

#define WIN_W 480
#define WIN_H 360
#define SCALE 3 /* pixel-art block size in real pixels */

#define MAX_ENEMIES 26
#define MAX_PBULLETS 30
#define MAX_EBULLETS 56
#define MAX_POWERUPS 8
#define MAX_PARTICLES 140
#define STAR_COUNT 90

enum {
  ETYPE_STRAIGHT = 0, /* "tipo 0": original straight-falling fighter */
  ETYPE_SINE,
  ETYPE_DIVER,
  ETYPE_SHOOTER,
  ETYPE_FRIGATE, /* astronave leggera: sine weave + spread gun */
  ETYPE_CRUISER, /* astronave pesante: tanky, missiles/laser */
  ETYPE_METEOR,  /* environmental hazard/bonus source, never shoots */
  ETYPE_BOSS
};

enum { PU_WEAPON = 0, PU_HEALTH, PU_BOMB, PU_TYPE_COUNT };

enum { STATE_PLAYING = 0, STATE_LEVEL_CLEAR, STATE_GAMEOVER };

/* Weapon families - shared pool for both the player and every enemy that
 * can shoot, so "ogni nemico può usare un'arma diversa" falls out of one
 * enemy_fire_weapon() switch instead of per-enemy special-casing. */
enum {
  WPN_NORMAL = 0,
  WPN_TRIPLE,
  WPN_MITRA,
  WPN_LASER,
  WPN_MISSILE,
  WPN_COUNT
};

static const char *WEAPON_NAMES[WPN_COUNT] = {"NORMALE", "TRIPLO", "MITRA",
                                              "LASER", "MISSILI"};
static const Uint8 WPN_COLOR[WPN_COUNT][3] = {{130, 230, 255},
                                              {110, 180, 255},
                                              {255, 230, 120},
                                              {230, 120, 255},
                                              {255, 150, 60}};
static const int WPN_COOLDOWN[WPN_COUNT] = {10, 13, 4, 9, 22};

/* ---------------------------------------------------------------------- */
/* Tiny xorshift PRNG - no libc rand() on this platform, so we roll our own
 * the same way the rest of user-space does when it needs randomness. */
/* ---------------------------------------------------------------------- */

static unsigned int rng_state = 0x9e3779b9u;

static unsigned int rng_next(void) {
  unsigned int x = rng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_state = x;
  return x;
}

static int rng_range(int lo, int hi) {
  if (hi <= lo)
    return lo;
  return lo + (int)(rng_next() % (unsigned)(hi - lo + 1));
}

/* ---------------------------------------------------------------------- */
/* Pixel-art sprites. Rows may be any length (draw_sprite stops at the NUL
 * terminator of each row independently), so the art below doesn't need to
 * be a perfect rectangle - it's just ASCII silhouettes. */
/* ---------------------------------------------------------------------- */

typedef struct {
  char ch;
  Uint8 r, g, b;
} SpritePalette;

static void fillblock(SDL_Surface *s, int x, int y, int w, int h, Uint8 r,
                      Uint8 g, Uint8 b) {
  if (w <= 0 || h <= 0)
    return;
  SDL_Rect rc = {x, y, w, h};
  SDL_FillRect(s, &rc, SDL_MapRGB(s->format, r, g, b));
}

static void draw_sprite(SDL_Surface *s, const char *const *rows, int nrows,
                        int x, int y, int scale, const SpritePalette *pal,
                        int npal) {
  for (int ry = 0; ry < nrows; ry++) {
    const char *row = rows[ry];
    for (int cx = 0; row[cx]; cx++) {
      char c = row[cx];
      if (c == ' ')
        continue;
      for (int p = 0; p < npal; p++) {
        if (pal[p].ch == c) {
          fillblock(s, x + cx * scale, y + ry * scale, scale, scale, pal[p].r,
                    pal[p].g, pal[p].b);
          break;
        }
      }
    }
  }
}

static const char *PLAYER_ART[9] = {"    #    ", "   ###   ", "   #o#   ",
                                    "  ##o##  ", " ##ooo## ", "#########",
                                    "## ### ##", "#   .   #", "    .    "};
static const SpritePalette PLAYER_PAL_BASE[2] = {{'#', 205, 208, 220},
                                                 {'o', 90, 220, 255}};

static const char *ENEMY_STRAIGHT_ART[6] = {"  # #  ", " ##### ", "#######",
                                            " ## ## ", "# # # #", "  # #  "};
static const SpritePalette ENEMY_STRAIGHT_PAL[1] = {{'#', 225, 70, 70}};

static const char *ENEMY_SINE_ART[7] = {" ## ## ", "#######", "# ### #",
                                        "##   ##", "# ### #", "#######",
                                        " ## ## "};
static const SpritePalette ENEMY_SINE_PAL[1] = {{'#', 70, 220, 130}};

static const char *ENEMY_DIVER_ART[7] = {"   #   ", "  ###  ", " ##### ",
                                         "#######", " # # # ", "#  #  #",
                                         "   #   "};
static const SpritePalette ENEMY_DIVER_PAL[1] = {{'#', 175, 90, 235}};

static const char *ENEMY_SHOOTER_ART[9] = {
    "   ###   ", "  #####  ", " ####### ", "#########", "## ### ##",
    "# ##o## #", "#   #   #", "  # # #  ", "   # #   "};
static const SpritePalette ENEMY_SHOOTER_PAL[2] = {{'#', 235, 150, 55},
                                                   {'o', 255, 240, 140}};

/* "Astronave" classes: bigger hulls than the fighters above. */
static const char *ENEMY_FRIGATE_ART[9] = {
    "    ###    ", "   #####   ", "  #######  ", " ###ooo### ", "###########",
    "## ##### ##", " #  ###  # ", "    # #    ", "   #   #   "};
static const SpritePalette ENEMY_FRIGATE_PAL[2] = {{'#', 90, 140, 255},
                                                   {'o', 210, 235, 255}};

static const char *ENEMY_CRUISER_ART[12] = {
    "     #####     ", "    #######    ", "   #########   ", "  ###########  ",
    " ##oo##o##oo## ", "###############", "###############", " ### ##### ### ",
    "  #   ###   #  ", "  #    #    #  ", "  #   ###   #  ", "     # # #     "};
static const SpritePalette ENEMY_CRUISER_PAL[2] = {{'#', 70, 100, 150},
                                                   {'o', 255, 210, 90}};

static const char *METEOR_ART[7] = {" ##### ", "###.###", "##...##", "#.....#",
                                    "##...##", "###.###", " ##### "};
static const SpritePalette METEOR_PAL[2] = {{'#', 130, 108, 88},
                                            {'.', 85, 70, 58}};

static const char *BOSS_ART[11] = {
    "       #####       ",   "     #########     ", "   ###############  ",
    " ################### ", "## # #  ooo  # # ##", "###################",
    " ## ### ### ### ## ",   "  #   #####   #  ",   "   ## ## ## ##   ",
    "    #   #   #    ",     "     # # # #     "};
static const SpritePalette BOSS_PAL[2] = {{'#', 150, 25, 35},
                                          {'o', 255, 225, 70}};

static const char *POWERUP_ART[5] = {" # ", "###", "#o#", "###", " # "};

/* 3x5 bitmap digits for the HUD (score / level / lives / bombs / fps). */
static const char *DIGIT_ART[10][5] = {
    {"111", "101", "101", "101", "111"}, {"010", "110", "010", "010", "111"},
    {"111", "001", "111", "100", "111"}, {"111", "001", "111", "001", "111"},
    {"101", "101", "111", "001", "001"}, {"111", "100", "111", "001", "111"},
    {"111", "100", "111", "101", "111"}, {"111", "001", "001", "001", "001"},
    {"111", "101", "111", "101", "111"}, {"111", "101", "111", "001", "111"}};

static void draw_number(SDL_Surface *s, long value, int x, int y, int scale,
                        Uint8 r, Uint8 g, Uint8 b) {
  char buf[12];
  int n = 0;
  if (value < 0)
    value = 0;
  if (value == 0) {
    buf[n++] = '0';
  } else {
    while (value > 0 && n < 11) {
      buf[n++] = (char)('0' + (value % 10));
      value /= 10;
    }
  }
  for (int i = 0; i < n / 2; i++) {
    char t = buf[i];
    buf[i] = buf[n - 1 - i];
    buf[n - 1 - i] = t;
  }
  SpritePalette pal[1] = {{'1', r, g, b}};
  int cursor = x;
  for (int i = 0; i < n; i++) {
    int d = buf[i] - '0';
    draw_sprite(s, DIGIT_ART[d], 5, cursor, y, scale, pal, 1);
    cursor += 4 * scale;
  }
}

/* ---------------------------------------------------------------------- */
/* Entities                                                                */
/* ---------------------------------------------------------------------- */

typedef struct {
  int active;
  int x, y, w, h;
  int type;
  int hp, hp_max;
  int base_x;
  int vy;
  int phase;     /* degrees, for sine movement */
  int amplitude; /* pixels, for sine movement */
  int shoot_timer;
  int flash;   /* frames remaining of hit-flash */
  int weapon;  /* WPN_* this enemy fires, -1 = never shoots */
  int weapon2; /* boss only: second weapon family */
  int boss_variant;
} Enemy;

typedef struct {
  int active;
  int x, y, vx, vy;
  int dmg;
  int from_player;
  int wtype;  /* WPN_* that fired this bullet (rendering + behaviour) */
  int pierce; /* remaining enemies this bullet can pass through */
  int homing; /* 1 = steer toward its target every frame (missiles) */
} Bullet;

typedef struct {
  int active;
  int x, y, vy;
  int type;
} Powerup;

typedef struct {
  int active;
  int x, y, vx, vy;
  int life, life_max;
  Uint8 r, g, b;
} Particle;

typedef struct {
  int x, y, layer;
} Star;

static Enemy enemies[MAX_ENEMIES];
static Bullet pbullets[MAX_PBULLETS];
static Bullet ebullets[MAX_EBULLETS];
static Powerup powerups[MAX_POWERUPS];
static Particle particles[MAX_PARTICLES];
static Star stars[STAR_COUNT];

#define PLAYER_W (9 * SCALE)
#define PLAYER_H (9 * SCALE)

static int p_x, p_y;
static int p_hp, p_hp_max;
static int p_lives;
static int p_weapon;                  /* active WPN_* slot */
static unsigned p_weapon_unlocked;    /* bitmask of unlocked WPN_* slots */
static int p_weapon_level[WPN_COUNT]; /* power tier 1..3 per weapon */
static int p_bombs;
static int p_fire_cd;
static int p_invuln;
static int p_bomb_flash;
static long score;
static long high_score;
static long enemies_killed;
static long bosses_killed;

static int level;
static int spawn_timer, spawn_interval;
static int to_spawn, spawned;
static int wave_alive;
static int meteor_alive;
static int meteor_timer;
static int boss_level;
static int state;
static int state_timer;
static int frame_count;
static int fps_value; /* updated once a second by main(), read by the HUD */

/* ---------------------------------------------------------------------- */
/* Setup helpers                                                          */
/* ---------------------------------------------------------------------- */

static void init_stars(void) {
  for (int i = 0; i < STAR_COUNT; i++) {
    stars[i].x = rng_range(0, WIN_W - 1);
    stars[i].y = rng_range(0, WIN_H - 1);
    stars[i].layer = rng_range(0, 2);
  }
}

static void spawn_particles(int x, int y, int count, Uint8 r, Uint8 g,
                            Uint8 b) {
  for (int i = 0; i < count; i++) {
    for (int j = 0; j < MAX_PARTICLES; j++) {
      if (!particles[j].active) {
        particles[j].active = 1;
        particles[j].x = x;
        particles[j].y = y;
        particles[j].vx = rng_range(-4, 4);
        particles[j].vy = rng_range(-4, 4);
        particles[j].life = rng_range(14, 28);
        particles[j].life_max = particles[j].life;
        particles[j].r = r;
        particles[j].g = g;
        particles[j].b = b;
        break;
      }
    }
  }
}

/* Removes an enemy and keeps the two "how many are still around" counters
 * (wave_alive for the scripted wave, meteor_alive for the ambient hazard)
 * in sync no matter which code path killed it. */
static void enemy_remove(Enemy *e) {
  if (!e->active)
    return;
  e->active = 0;
  if (e->type == ETYPE_METEOR)
    meteor_alive--;
  else
    wave_alive--;
}

static void start_wave(void) {
  boss_level = (level % 5 == 0);
  spawned = 0;
  wave_alive = 0;
  spawn_timer = 30;
  if (boss_level) {
    to_spawn = 3; /* two escorts, then the boss */
    spawn_interval = 60;
  } else {
    to_spawn = 5 + level * 2;
    if (to_spawn > 22)
      to_spawn = 22;
    spawn_interval = 55 - level * 3;
    if (spawn_interval < 14)
      spawn_interval = 14;
  }
}

static void reset_game(void) {
  for (int i = 0; i < MAX_ENEMIES; i++)
    enemies[i].active = 0;
  for (int i = 0; i < MAX_PBULLETS; i++)
    pbullets[i].active = 0;
  for (int i = 0; i < MAX_EBULLETS; i++)
    ebullets[i].active = 0;
  for (int i = 0; i < MAX_POWERUPS; i++)
    powerups[i].active = 0;
  for (int i = 0; i < MAX_PARTICLES; i++)
    particles[i].active = 0;

  p_x = WIN_W / 2 - PLAYER_W / 2;
  p_y = WIN_H - PLAYER_H - 20;
  p_hp_max = 100;
  p_hp = p_hp_max;
  p_lives = 3;
  p_weapon = WPN_NORMAL;
  p_weapon_unlocked = 1u << WPN_NORMAL;
  for (int i = 0; i < WPN_COUNT; i++)
    p_weapon_level[i] = 1;
  p_bombs = 2;
  p_fire_cd = 0;
  p_invuln = 90;
  p_bomb_flash = 0;
  score = 0;
  enemies_killed = 0;
  bosses_killed = 0;
  meteor_alive = 0;
  meteor_timer = rng_range(140, 220);

  level = 1;
  state = STATE_PLAYING;
  state_timer = 0;
  start_wave();
}

/* ---------------------------------------------------------------------- */
/* Spawning                                                                */
/* ---------------------------------------------------------------------- */

static void spawn_enemy_of_type(int type) {
  Enemy *e = NULL;
  for (int i = 0; i < MAX_ENEMIES; i++) {
    if (!enemies[i].active) {
      e = &enemies[i];
      break;
    }
  }
  if (!e)
    return;

  e->active = 1;
  e->type = type;
  e->flash = 0;
  e->phase = rng_range(0, 359);
  e->weapon = -1;
  e->weapon2 = -1;
  e->boss_variant = 0;

  switch (type) {
  case ETYPE_STRAIGHT:
    e->w = 7 * SCALE;
    e->h = 6 * SCALE;
    e->hp_max = 10 + level;
    e->vy = 2 + level / 4;
    e->amplitude = 0;
    e->shoot_timer = 999999; /* never shoots */
    break;
  case ETYPE_SINE:
    e->w = 7 * SCALE;
    e->h = 7 * SCALE;
    e->hp_max = 14 + level;
    e->vy = 2 + level / 4;
    e->amplitude = rng_range(30, 55);
    e->shoot_timer = 999999;
    break;
  case ETYPE_DIVER:
    e->w = 7 * SCALE;
    e->h = 7 * SCALE;
    e->hp_max = 12 + level;
    e->vy = 3 + level / 3;
    e->amplitude = 0;
    e->shoot_timer = 999999;
    break;
  case ETYPE_SHOOTER:
    e->w = 9 * SCALE;
    e->h = 9 * SCALE;
    e->hp_max = 20 + level * 2;
    e->vy = 1 + level / 5;
    e->amplitude = 0;
    e->shoot_timer = rng_range(40, 90);
    break;
  case ETYPE_FRIGATE:
    e->w = 11 * SCALE;
    e->h = 9 * SCALE;
    e->hp_max = 32 + level * 3;
    e->vy = 1 + level / 5;
    e->amplitude = rng_range(20, 45);
    e->shoot_timer = rng_range(55, 95);
    break;
  case ETYPE_CRUISER:
    e->w = 15 * SCALE;
    e->h = 12 * SCALE;
    e->hp_max = 50 + level * 4;
    e->vy = 1;
    e->amplitude = 0;
    e->shoot_timer = rng_range(70, 115);
    break;
  case ETYPE_METEOR:
    e->w = rng_range(6, 9) * SCALE;
    e->h = e->w;
    e->hp_max = 12 + level;
    e->vy = 2 + rng_range(0, 2);
    e->amplitude = 0;
    e->shoot_timer = 999999; /* meteors never shoot */
    break;
  case ETYPE_BOSS:
  default:
    e->w = 19 * SCALE;
    e->h = 11 * SCALE;
    e->hp_max = 160 + level * 45;
    e->vy = 2;
    e->amplitude = 70;
    e->shoot_timer = rng_range(50, 80);
    break;
  }
  if (e->vy > 6 && type != ETYPE_BOSS)
    e->vy = 6;

  /* Single place that decides who carries which gun: keeps the "each
   * enemy/boss can use a different weapon" rule to one small table instead
   * of scattering it across every case above. */
  switch (type) {
  case ETYPE_SHOOTER:
    e->weapon = (level >= 4 && rng_range(0, 99) < 50) ? WPN_MITRA : WPN_NORMAL;
    break;
  case ETYPE_FRIGATE:
    e->weapon = WPN_TRIPLE;
    break;
  case ETYPE_CRUISER:
    e->weapon = (level >= 8 && rng_range(0, 99) < 40) ? WPN_LASER : WPN_MISSILE;
    break;
  case ETYPE_BOSS: {
    static const int table[3][2] = {{WPN_NORMAL, WPN_TRIPLE},
                                    {WPN_MITRA, WPN_MISSILE},
                                    {WPN_LASER, WPN_TRIPLE}};
    int v = rng_range(0, 2);
    e->boss_variant = v;
    e->weapon = table[v][0];
    e->weapon2 = table[v][1];
    break;
  }
  default:
    break; /* fighters and meteors stay weapon = -1 (never shoot) */
  }

  e->hp = e->hp_max;
  e->base_x = rng_range(10, WIN_W - e->w - 10);
  e->x = e->base_x;
  e->y = -e->h;
  if (type == ETYPE_METEOR)
    meteor_alive++;
  else
    wave_alive++;
}

static int pick_normal_type(void) {
  /* Higher levels weight the pool towards nastier types; astronavi only
   * start appearing once the player has had a few levels to get a weapon
   * or two upgraded. */
  int roll = rng_range(0, 99);

  int cruiser_chance = (level >= 6) ? (5 + (level - 6) * 2) : 0;
  if (cruiser_chance > 20)
    cruiser_chance = 20;
  int frigate_chance = (level >= 3) ? (8 + (level - 3) * 2) : 0;
  if (frigate_chance > 28)
    frigate_chance = 28;
  int shooter_chance = 10 + level * 2;
  int diver_chance = 15 + level * 2;
  if (shooter_chance > 30)
    shooter_chance = 30;
  if (diver_chance > 30)
    diver_chance = 30;

  if (roll < cruiser_chance)
    return ETYPE_CRUISER;
  roll -= cruiser_chance;
  if (roll < frigate_chance)
    return ETYPE_FRIGATE;
  roll -= frigate_chance;
  if (roll < shooter_chance)
    return ETYPE_SHOOTER;
  roll -= shooter_chance;
  if (roll < diver_chance)
    return ETYPE_DIVER;
  roll -= diver_chance;
  if (roll < 35)
    return ETYPE_SINE;
  return ETYPE_STRAIGHT;
}

static void spawn_powerup(int x, int y) {
  int roll = rng_range(0, 99);
  int type = (roll < 35) ? PU_WEAPON : (roll < 80 ? PU_HEALTH : PU_BOMB);
  for (int i = 0; i < MAX_POWERUPS; i++) {
    if (!powerups[i].active) {
      powerups[i].active = 1;
      powerups[i].x = x;
      powerups[i].y = y;
      powerups[i].vy = 2;
      powerups[i].type = type;
      break;
    }
  }
}

/* ---------------------------------------------------------------------- */
/* Update                                                                  */
/* ---------------------------------------------------------------------- */

static int aabb(int ax, int ay, int aw, int ah, int bx, int by, int bw,
                int bh) {
  return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

static void spawn_pbullet(int x, int y, int vx, int vy, int dmg, int wtype,
                          int pierce, int homing) {
  for (int i = 0; i < MAX_PBULLETS; i++) {
    if (!pbullets[i].active) {
      pbullets[i].active = 1;
      pbullets[i].x = x;
      pbullets[i].y = y;
      pbullets[i].vx = vx;
      pbullets[i].vy = vy;
      pbullets[i].dmg = dmg;
      pbullets[i].from_player = 1;
      pbullets[i].wtype = wtype;
      pbullets[i].pierce = pierce;
      pbullets[i].homing = homing;
      return;
    }
  }
}

static void spawn_ebullet(int x, int y, int vx, int vy, int dmg, int wtype,
                          int homing) {
  for (int i = 0; i < MAX_EBULLETS; i++) {
    if (!ebullets[i].active) {
      ebullets[i].active = 1;
      ebullets[i].x = x;
      ebullets[i].y = y;
      ebullets[i].vx = vx;
      ebullets[i].vy = vy;
      ebullets[i].dmg = dmg;
      ebullets[i].from_player = 0;
      ebullets[i].wtype = wtype;
      ebullets[i].pierce = 0;
      ebullets[i].homing = homing;
      return;
    }
  }
}

/* Every weapon family the player can carry, gated by its current power
 * level (1-3 from PU_WEAPON pickups). */
static void fire_player_bullets(void) {
  int cx = p_x + PLAYER_W / 2;
  int top = p_y - 2;
  int lvl = p_weapon_level[p_weapon];

  switch (p_weapon) {
  case WPN_NORMAL: {
    static const int offs1[1] = {0};
    static const int offs2[2] = {-4, 4};
    static const int offs3[3] = {-7, 0, 7};
    const int *offs = (lvl == 1) ? offs1 : (lvl == 2) ? offs2 : offs3;
    for (int i = 0; i < lvl; i++)
      spawn_pbullet(cx + offs[i], top, 0, -9, 9 + lvl * 2, WPN_NORMAL, 0, 0);
    break;
  }
  case WPN_TRIPLE: {
    int n = (lvl >= 3) ? 5 : 3;
    static const int offs5[5] = {-16, -8, 0, 8, 16};
    static const int vxs5[5] = {-3, -2, 0, 2, 3};
    static const int offs3[3] = {-8, 0, 8};
    static const int vxs3[3] = {-2, 0, 2};
    for (int i = 0; i < n; i++) {
      int off = (n == 5) ? offs5[i] : offs3[i];
      int vx = (n == 5) ? vxs5[i] : vxs3[i];
      spawn_pbullet(cx + off, top, vx, -9, 6 + lvl, WPN_TRIPLE, 0, 0);
    }
    break;
  }
  case WPN_MITRA: {
    int side = ((frame_count / 3) & 1) ? -5 : 5;
    spawn_pbullet(cx + side, top, 0, -12, 4 + lvl, WPN_MITRA, 0, 0);
    break;
  }
  case WPN_LASER:
    spawn_pbullet(cx - 1, top, 0, -14, 7 + lvl * 2, WPN_LASER, 1 + lvl, 0);
    break;
  case WPN_MISSILE:
    spawn_pbullet(cx - 1, top, 0, -6, 16 + lvl * 4, WPN_MISSILE, 0, 1);
    break;
  }
}

/* Mirror of fire_player_bullets for anything hostile that shoots. One
 * switch covers every enemy type - fighters/frigate/cruiser/boss all just
 * pass in whichever WPN_* they were assigned at spawn time. */
static void enemy_fire_weapon(Enemy *e, int weapon) {
  int ex = e->x + e->w / 2;
  int ey = e->y + e->h;
  int px_c = p_x + PLAYER_W / 2;
  int aim_vx = 0;
  if (px_c < ex - 4)
    aim_vx = -1;
  else if (px_c > ex + 4)
    aim_vx = 1;

  switch (weapon) {
  case WPN_TRIPLE:
    spawn_ebullet(ex - 8, ey, -2, 5, 10, WPN_TRIPLE, 0);
    spawn_ebullet(ex - 1, ey, 0, 6, 10, WPN_TRIPLE, 0);
    spawn_ebullet(ex + 7, ey, 2, 5, 10, WPN_TRIPLE, 0);
    break;
  case WPN_MITRA:
    spawn_ebullet(ex - 5, ey, aim_vx, 7, 6, WPN_MITRA, 0);
    spawn_ebullet(ex + 4, ey, aim_vx, 7, 6, WPN_MITRA, 0);
    break;
  case WPN_LASER:
    spawn_ebullet(ex - 1, ey, 0, 8, 14, WPN_LASER, 0);
    break;
  case WPN_MISSILE:
    spawn_ebullet(ex - 2, ey, aim_vx, 4, 18, WPN_MISSILE, 1);
    break;
  case WPN_NORMAL:
  default:
    spawn_ebullet(ex - 1, ey, aim_vx, 5, 10, WPN_NORMAL, 0);
    break;
  }
}

static void do_bomb(void) {
  if (p_bombs <= 0)
    return;
  p_bombs--;
  p_bomb_flash = 20;
  for (int i = 0; i < MAX_EBULLETS; i++)
    ebullets[i].active = 0;
  for (int i = 0; i < MAX_ENEMIES; i++) {
    if (!enemies[i].active)
      continue;
    spawn_particles(enemies[i].x + enemies[i].w / 2,
                    enemies[i].y + enemies[i].h / 2, 6, 255, 220, 120);
    if (enemies[i].type == ETYPE_BOSS) {
      enemies[i].hp -= enemies[i].hp_max / 4;
    } else {
      enemies[i].hp = 0;
      enemy_remove(&enemies[i]);
    }
  }
}

static void update_player(const Uint8 *keys) {
  int speed = 4;
  if (keys[SDL_SCANCODE_LEFT])
    p_x -= speed;
  if (keys[SDL_SCANCODE_RIGHT])
    p_x += speed;
  if (keys[SDL_SCANCODE_UP])
    p_y -= speed;
  if (keys[SDL_SCANCODE_DOWN])
    p_y += speed;

  if (p_x < 4)
    p_x = 4;
  if (p_x > WIN_W - PLAYER_W - 4)
    p_x = WIN_W - PLAYER_W - 4;
  if (p_y < WIN_H / 2 - 20)
    p_y = WIN_H / 2 - 20;
  if (p_y > WIN_H - PLAYER_H - 10)
    p_y = WIN_H - PLAYER_H - 10;

  if (p_fire_cd > 0)
    p_fire_cd--;
  if (keys[SDL_SCANCODE_SPACE] && p_fire_cd == 0) {
    fire_player_bullets();
    p_fire_cd = WPN_COOLDOWN[p_weapon] - (p_weapon_level[p_weapon] - 1);
    if (p_fire_cd < 3)
      p_fire_cd = 3;
  }

  if (p_invuln > 0)
    p_invuln--;
  if (p_bomb_flash > 0)
    p_bomb_flash--;
}

static void apply_powerup(int type) {
  if (type == PU_WEAPON) {
    /* Upgrades whichever weapon is currently selected, so switching guns
     * with the number keys is a real choice about where to invest. */
    if (p_weapon_level[p_weapon] < 3) {
      p_weapon_level[p_weapon]++;
      score += 50;
    } else {
      score += 120; /* maxed out already: convert into bonus points */
    }
  } else if (type == PU_HEALTH) {
    p_hp += 30;
    if (p_hp > p_hp_max)
      p_hp = p_hp_max;
    score += 30;
  } else {
    p_bombs++;
    score += 40;
  }
}

static void player_take_hit(int dmg) {
  if (p_invuln > 0)
    return;
  p_hp -= dmg;
  p_invuln = 55;
  if (p_hp <= 0) {
    p_lives--;
    spawn_particles(p_x + PLAYER_W / 2, p_y + PLAYER_H / 2, 20, 255, 200, 90);
    if (p_lives > 0) {
      p_hp = p_hp_max;
      p_invuln = 110;
    } else {
      state = STATE_GAMEOVER;
      state_timer = 0;
    }
  }
}

/* Central "an enemy just died" handler: scoring, kill counters, particle
 * burst, powerup drop chance and (for bosses) the weapon-unlock message
 * all live here so bullets, missile splash damage and body-collisions all
 * behave identically instead of duplicating the bookkeeping three times. */
static void on_enemy_killed(Enemy *e) {
  int type = e->type;
  int ex = e->x + e->w / 2;
  int ey = e->y + e->h / 2;
  enemy_remove(e);

  long reward = (type == ETYPE_BOSS)
                    ? 500
                    : (type == ETYPE_METEOR ? 15 + level * 2 : 25 + level * 3);
  score += reward;

  if (type == ETYPE_BOSS)
    bosses_killed++;
  else if (type != ETYPE_METEOR)
    enemies_killed++;

  spawn_particles(ex, ey,
                  type == ETYPE_BOSS ? 40 : (type == ETYPE_METEOR ? 10 : 14),
                  255, 160, 60);

  if (type != ETYPE_BOSS) {
    int drop = (type == ETYPE_METEOR) ? 55 : 22; /* meteors are generous */
    if (rng_range(0, 99) < drop)
      spawn_powerup(ex, ey);
  }

  if (type == ETYPE_BOSS) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Boss sconfitto! Punteggio %ld", score);
    OS1_notify_post("NexRaptor", msg);
    if (bosses_killed < WPN_COUNT) {
      p_weapon_unlocked |= (1u << bosses_killed);
      char m2[64];
      snprintf(m2, sizeof(m2), "Nuova arma sbloccata: %s!",
               WEAPON_NAMES[bosses_killed]);
      OS1_notify_post("NexRaptor", m2);
    }
  }
}

static void update_world(void) {
  frame_count++;

  /* Wave spawning (scripted: to_spawn enemies, boss at the end of a boss
   * level). */
  if (spawned < to_spawn) {
    spawn_timer--;
    if (spawn_timer <= 0) {
      int type;
      if (boss_level && spawned == to_spawn - 1) {
        type = ETYPE_BOSS;
        char msg[64];
        snprintf(msg, sizeof(msg), "Livello %d: attenzione, arriva un boss!",
                 level);
        OS1_notify_post("NexRaptor", msg);
      } else if (boss_level) {
        type = rng_range(0, 1) ? ETYPE_SINE : ETYPE_STRAIGHT; /* escorts */
      } else {
        type = pick_normal_type();
      }
      spawn_enemy_of_type(type);
      spawned++;
      spawn_timer = spawn_interval;
    }
  }

  /* Meteors: an independent, always-on hazard/bonus stream, not part of
   * the scripted wave (so it never blocks a level from clearing). */
  if (state == STATE_PLAYING) {
    meteor_timer--;
    if (meteor_timer <= 0) {
      spawn_enemy_of_type(ETYPE_METEOR);
      int cooldown = 220 - level * 4;
      if (cooldown < 70)
        cooldown = 70;
      meteor_timer = rng_range(cooldown, cooldown + 90);
    }
  }

  /* Enemies */
  for (int i = 0; i < MAX_ENEMIES; i++) {
    Enemy *e = &enemies[i];
    if (!e->active)
      continue;

    if (e->flash > 0)
      e->flash--;

    if (e->type == ETYPE_BOSS) {
      if (e->y < 40) {
        e->y += e->vy;
      } else {
        e->phase = (e->phase + 2) % 360;
        int sinv = sin_fp(DEG_TO_FP_RAD(e->phase));
        int offset = (e->amplitude * sinv) >> FP_SHIFT;
        e->x = e->base_x + offset;
      }
      e->shoot_timer--;
      if (e->shoot_timer <= 0) {
        /* Alternates between its two weapon families every ~3 seconds. */
        int active_wpn = ((frame_count / 180) & 1) ? e->weapon2 : e->weapon;
        enemy_fire_weapon(e, active_wpn);
        e->shoot_timer = rng_range(35, 65);
      }
    } else {
      e->y += e->vy;
      if (e->amplitude != 0) {
        e->phase = (e->phase + 5) % 360;
        int sinv = sin_fp(DEG_TO_FP_RAD(e->phase));
        int offset = (e->amplitude * sinv) >> FP_SHIFT;
        e->x = e->base_x + offset;
      } else if (e->type == ETYPE_DIVER) {
        int px_c = p_x + PLAYER_W / 2;
        int ex_c = e->x + e->w / 2;
        if (px_c < ex_c)
          e->x -= 2;
        else if (px_c > ex_c)
          e->x += 2;
      }
      if (e->type == ETYPE_SHOOTER || e->type == ETYPE_FRIGATE ||
          e->type == ETYPE_CRUISER) {
        e->shoot_timer--;
        if (e->shoot_timer <= 0) {
          enemy_fire_weapon(e, e->weapon);
          e->shoot_timer = rng_range(50, 100 - level);
          if (e->shoot_timer < 30)
            e->shoot_timer = 30;
        }
      }
    }

    if (e->x < 0)
      e->x = 0;
    if (e->x > WIN_W - e->w)
      e->x = WIN_W - e->w;

    if (e->y > WIN_H + 20) {
      enemy_remove(e);
      continue;
    }

    /* Body collision with player */
    if (p_invuln == 0 &&
        aabb(p_x, p_y, PLAYER_W, PLAYER_H, e->x, e->y, e->w, e->h)) {
      if (e->type == ETYPE_BOSS) {
        player_take_hit(25);
      } else {
        int dmg = (e->type == ETYPE_METEOR) ? 20 : 15;
        player_take_hit(dmg);
        e->hp = 0;
        on_enemy_killed(e);
      }
    }
  }

  /* Player bullets */
  for (int i = 0; i < MAX_PBULLETS; i++) {
    Bullet *b = &pbullets[i];
    if (!b->active)
      continue;

    if (b->homing) {
      /* Missiles gently steer toward the nearest enemy ahead of them. */
      int bestd = 0x7fffffff, bx = 0;
      int found = 0;
      for (int j = 0; j < MAX_ENEMIES; j++) {
        if (!enemies[j].active)
          continue;
        int ex = enemies[j].x + enemies[j].w / 2;
        int ey = enemies[j].y + enemies[j].h / 2;
        int dy = ey - b->y;
        if (dy > 4)
          continue; /* only steer toward targets ahead (above the bullet) */
        int dx = ex - b->x;
        int d = dx * dx + dy * dy;
        if (d < bestd) {
          bestd = d;
          bx = dx;
          found = 1;
        }
      }
      if (found) {
        if (bx < -2)
          b->vx--;
        else if (bx > 2)
          b->vx++;
        if (b->vx > 4)
          b->vx = 4;
        if (b->vx < -4)
          b->vx = -4;
      }
    }

    b->x += b->vx;
    b->y += b->vy;
    if (b->y < -10) {
      b->active = 0;
      continue;
    }
    for (int j = 0; j < MAX_ENEMIES; j++) {
      Enemy *e = &enemies[j];
      if (!e->active || e->hp <= 0)
        continue;
      if (aabb(b->x, b->y, 3, 8, e->x, e->y, e->w, e->h)) {
        e->hp -= b->dmg;
        e->flash = 4;
        spawn_particles(b->x, b->y, 3, 255, 255, 160);

        if (b->wtype == WPN_MISSILE) {
          /* Splash damage: half the direct hit, to anything else nearby. */
          spawn_particles(b->x, b->y, 10, 255, 170, 60);
          for (int k = 0; k < MAX_ENEMIES; k++) {
            if (k == j)
              continue;
            Enemy *e2 = &enemies[k];
            if (!e2->active || e2->hp <= 0)
              continue;
            int ddx = (e2->x + e2->w / 2) - (e->x + e->w / 2);
            int ddy = (e2->y + e2->h / 2) - (e->y + e->h / 2);
            if (ddx * ddx + ddy * ddy < 42 * 42) {
              e2->hp -= b->dmg / 2;
              e2->flash = 4;
              if (e2->hp <= 0)
                on_enemy_killed(e2);
            }
          }
        }

        if (b->pierce > 0)
          b->pierce--;
        else
          b->active = 0;

        if (e->hp <= 0)
          on_enemy_killed(e);
        break;
      }
    }
  }

  /* Enemy bullets */
  for (int i = 0; i < MAX_EBULLETS; i++) {
    Bullet *b = &ebullets[i];
    if (!b->active)
      continue;
    if (b->homing) {
      int px_c = p_x + PLAYER_W / 2;
      if (b->x < px_c - 2)
        b->vx++;
      else if (b->x > px_c + 2)
        b->vx--;
      if (b->vx > 3)
        b->vx = 3;
      if (b->vx < -3)
        b->vx = -3;
    }
    b->x += b->vx;
    b->y += b->vy;
    if (b->y > WIN_H + 10) {
      b->active = 0;
      continue;
    }
    if (aabb(b->x, b->y, 3, 8, p_x, p_y, PLAYER_W, PLAYER_H)) {
      b->active = 0;
      player_take_hit(b->dmg);
    }
  }

  /* Powerups */
  for (int i = 0; i < MAX_POWERUPS; i++) {
    Powerup *pu = &powerups[i];
    if (!pu->active)
      continue;
    pu->y += pu->vy;
    if (pu->y > WIN_H + 10) {
      pu->active = 0;
      continue;
    }
    if (aabb(pu->x, pu->y, 5 * SCALE, 5 * SCALE, p_x, p_y, PLAYER_W,
             PLAYER_H)) {
      apply_powerup(pu->type);
      pu->active = 0;
    }
  }

  /* Particles */
  for (int i = 0; i < MAX_PARTICLES; i++) {
    Particle *pt = &particles[i];
    if (!pt->active)
      continue;
    pt->x += pt->vx;
    pt->y += pt->vy;
    pt->life--;
    if (pt->life <= 0)
      pt->active = 0;
  }

  /* Level progress (meteors never gate this, only the scripted wave) */
  if (spawned >= to_spawn && wave_alive <= 0 && state == STATE_PLAYING) {
    score += 100 + level * 10;
    if (level % 5 == 0)
      p_bombs++;
    state = STATE_LEVEL_CLEAR;
    state_timer = 90;
    char msg[64];
    snprintf(msg, sizeof(msg), "Livello %d superato! Punteggio %ld", level,
             score);
    OS1_notify_post("NexRaptor", msg);
  }
}

/* ---------------------------------------------------------------------- */
/* Render                                                                  */
/* ---------------------------------------------------------------------- */

static void render_background(SDL_Surface *surf) {
  Uint32 *pixels = (Uint32 *)surf->pixels;
  int pitch_px = surf->pitch / 4;
  for (int y = 0; y < WIN_H; y++) {
    int t = (y * 40) / WIN_H; /* 0..40 */
    Uint8 r = (Uint8)(6 + t / 4);
    Uint8 g = (Uint8)(6 + t / 6);
    Uint8 b = (Uint8)(22 + t);
    Uint32 col = 0xFF000000u | ((Uint32)r << 16) | ((Uint32)g << 8) | b;
    for (int x = 0; x < WIN_W; x++)
      pixels[y * pitch_px + x] = col;
  }
  for (int i = 0; i < STAR_COUNT; i++) {
    Star *s = &stars[i];
    Uint8 shade = (Uint8)(90 + s->layer * 60);
    int size = (s->layer == 2) ? 2 : 1;
    if (s->x >= 0 && s->x < WIN_W - size && s->y >= 0 && s->y < WIN_H - size)
      fillblock(surf, s->x, s->y, size, size, shade, shade, shade);
  }
}

static void update_stars(void) {
  static const int speeds[3] = {1, 2, 3};
  for (int i = 0; i < STAR_COUNT; i++) {
    stars[i].y += speeds[stars[i].layer];
    if (stars[i].y >= WIN_H) {
      stars[i].y = 0;
      stars[i].x = rng_range(0, WIN_W - 1);
    }
  }
}

static void render_hud(SDL_Surface *surf) {
  /* Health bar */
  int barw = 110, barh = 8;
  fillblock(surf, 8, 6, barw + 2, barh + 2, 40, 40, 50);
  int hp_w = (p_hp * barw) / p_hp_max;
  Uint8 hr = (Uint8)(p_hp < 30 ? 230 : (p_hp < 60 ? 220 : 60));
  Uint8 hg = (Uint8)(p_hp < 30 ? 50 : (p_hp < 60 ? 200 : 210));
  fillblock(surf, 9, 7, hp_w, barh, hr, hg, 70);

  /* Score, top right */
  draw_number(surf, score, WIN_W - 130, 6, 2, 255, 255, 255);

  /* Level, top center */
  draw_number(surf, level, WIN_W / 2 - 6, 6, 2, 255, 220, 120);

  /* Lives icons */
  for (int i = 0; i < p_lives; i++)
    fillblock(surf, 8 + i * 12, 20, 8, 8, 210, 215, 230);

  /* Bomb icons */
  for (int i = 0; i < p_bombs; i++)
    fillblock(surf, WIN_W - 20 - i * 12, 20, 8, 8, 255, 150, 60);

  /* Boss health bar */
  for (int i = 0; i < MAX_ENEMIES; i++) {
    if (enemies[i].active && enemies[i].type == ETYPE_BOSS) {
      int bw = 220;
      int x0 = WIN_W / 2 - bw / 2;
      fillblock(surf, x0 - 2, 30, bw + 4, 10, 40, 20, 20);
      int fillw = (enemies[i].hp * bw) / enemies[i].hp_max;
      if (fillw < 0)
        fillw = 0;
      fillblock(surf, x0, 32, fillw, 6, 220, 40, 50);
      break;
    }
  }

  /* Weapon slots, bottom center: greyed out = locked, white outline =
   * currently selected, small dark pips = current power level. */
  {
    int slot = 14, gap = 4;
    int totalw = WPN_COUNT * slot + (WPN_COUNT - 1) * gap;
    int wx = WIN_W / 2 - totalw / 2;
    int wy = WIN_H - 26;
    for (int i = 0; i < WPN_COUNT; i++) {
      int locked = !(p_weapon_unlocked & (1u << i));
      Uint8 r, g, b;
      if (locked) {
        r = g = b = 45;
      } else {
        r = WPN_COLOR[i][0];
        g = WPN_COLOR[i][1];
        b = WPN_COLOR[i][2];
      }
      if (i == p_weapon)
        fillblock(surf, wx - 2, wy - 2, slot + 4, slot + 4, 255, 255, 255);
      fillblock(surf, wx, wy, slot, slot, r, g, b);
      if (!locked) {
        int lvl = p_weapon_level[i];
        for (int p = 0; p < lvl; p++)
          fillblock(surf, wx + 2 + p * 4, wy + slot - 4, 3, 3, 20, 20, 20);
      }
      wx += slot + gap;
    }
  }

  /* Kill counters, bottom-left: enemies (blue marker) and bosses (red). */
  fillblock(surf, 8, WIN_H - 13, 5, 5, 130, 200, 255);
  draw_number(surf, enemies_killed, 16, WIN_H - 14, 1, 200, 220, 255);
  fillblock(surf, 60, WIN_H - 13, 5, 5, 255, 90, 90);
  draw_number(surf, bosses_killed, 68, WIN_H - 14, 1, 255, 170, 170);

  /* FPS counter, bottom-right */
  draw_number(surf, fps_value, WIN_W - 30, WIN_H - 14, 1, 140, 255, 140);

  if (p_bomb_flash > 0) {
    Uint8 a = (Uint8)(p_bomb_flash * 8);
    if (a > 255)
      a = 255;
    fillblock(surf, 0, 0, WIN_W, WIN_H, a, a, a);
  }
}

static void render(SDL_Surface *surf) {
  render_background(surf);

  for (int i = 0; i < MAX_POWERUPS; i++) {
    if (!powerups[i].active)
      continue;
    SpritePalette pal[2];
    pal[0].ch = '#';
    pal[1].ch = 'o';
    switch (powerups[i].type) {
    case PU_WEAPON:
      pal[0].r = 70;
      pal[0].g = 190;
      pal[0].b = 255;
      pal[1].r = 255;
      pal[1].g = 255;
      pal[1].b = 255;
      break;
    case PU_HEALTH:
      pal[0].r = 70;
      pal[0].g = 220;
      pal[0].b = 110;
      pal[1].r = 255;
      pal[1].g = 255;
      pal[1].b = 255;
      break;
    default:
      pal[0].r = 255;
      pal[0].g = 150;
      pal[0].b = 60;
      pal[1].r = 255;
      pal[1].g = 230;
      pal[1].b = 90;
      break;
    }
    draw_sprite(surf, POWERUP_ART, 5, powerups[i].x, powerups[i].y, SCALE, pal,
                2);
  }

  for (int i = 0; i < MAX_EBULLETS; i++) {
    if (!ebullets[i].active)
      continue;
    const Uint8 *c = WPN_COLOR[ebullets[i].wtype];
    int w = (ebullets[i].wtype == WPN_MISSILE) ? 5 : 3;
    fillblock(surf, ebullets[i].x, ebullets[i].y, w, 8, (Uint8)(200 + c[0] / 5),
              (Uint8)(c[1] / 3), (Uint8)(c[2] / 3));
  }

  for (int i = 0; i < MAX_PBULLETS; i++) {
    if (!pbullets[i].active)
      continue;
    const Uint8 *c = WPN_COLOR[pbullets[i].wtype];
    int w = (pbullets[i].wtype == WPN_MISSILE) ? 5 : 3;
    fillblock(surf, pbullets[i].x, pbullets[i].y, w, 8, c[0], c[1], c[2]);
  }

  for (int i = 0; i < MAX_ENEMIES; i++) {
    Enemy *e = &enemies[i];
    if (!e->active)
      continue;
    SpritePalette flashpal[2] = {{'#', 255, 255, 255}, {'o', 255, 255, 255}};
    switch (e->type) {
    case ETYPE_STRAIGHT:
      draw_sprite(surf, ENEMY_STRAIGHT_ART, 6, e->x, e->y, SCALE,
                  e->flash ? flashpal : ENEMY_STRAIGHT_PAL, 1);
      break;
    case ETYPE_SINE:
      draw_sprite(surf, ENEMY_SINE_ART, 7, e->x, e->y, SCALE,
                  e->flash ? flashpal : ENEMY_SINE_PAL, 1);
      break;
    case ETYPE_DIVER:
      draw_sprite(surf, ENEMY_DIVER_ART, 7, e->x, e->y, SCALE,
                  e->flash ? flashpal : ENEMY_DIVER_PAL, 1);
      break;
    case ETYPE_SHOOTER:
      draw_sprite(surf, ENEMY_SHOOTER_ART, 9, e->x, e->y, SCALE,
                  e->flash ? flashpal : ENEMY_SHOOTER_PAL, 2);
      break;
    case ETYPE_FRIGATE:
      draw_sprite(surf, ENEMY_FRIGATE_ART, 9, e->x, e->y, SCALE,
                  e->flash ? flashpal : ENEMY_FRIGATE_PAL, 2);
      break;
    case ETYPE_CRUISER:
      draw_sprite(surf, ENEMY_CRUISER_ART, 12, e->x, e->y, SCALE,
                  e->flash ? flashpal : ENEMY_CRUISER_PAL, 2);
      break;
    case ETYPE_METEOR:
      draw_sprite(surf, METEOR_ART, 7, e->x, e->y, SCALE,
                  e->flash ? flashpal : METEOR_PAL, 2);
      break;
    case ETYPE_BOSS:
      draw_sprite(surf, BOSS_ART, 11, e->x, e->y, SCALE,
                  e->flash ? flashpal : BOSS_PAL, 2);
      break;
    }
  }

  for (int i = 0; i < MAX_PARTICLES; i++) {
    Particle *pt = &particles[i];
    if (!pt->active)
      continue;
    int size = 1 + (pt->life * 3) / pt->life_max;
    fillblock(surf, pt->x, pt->y, size, size, pt->r, pt->g, pt->b);
  }

  /* Player ship: blink while invulnerable, engine flame flickers */
  if (p_invuln == 0 || (frame_count / 4) % 2 == 0) {
    SpritePalette pal[2];
    pal[0] = PLAYER_PAL_BASE[0];
    pal[1] = PLAYER_PAL_BASE[1];
    draw_sprite(surf, PLAYER_ART, 9, p_x, p_y, SCALE, pal, 2);
    Uint8 fr = (frame_count % 6 < 3) ? 255 : 255;
    Uint8 fg = (frame_count % 6 < 3) ? 170 : 220;
    fillblock(surf, p_x + PLAYER_W / 2 - SCALE / 2, p_y + PLAYER_H - SCALE,
              SCALE, SCALE, fr, fg, 60);
  }

  render_hud(surf);
}

/* ---------------------------------------------------------------------- */
/* main                                                                    */
/* ---------------------------------------------------------------------- */

int main(void) {
  int pid = get_pid();
  rng_state = (unsigned int)get_time() ^ (unsigned int)(pid * 2654435761u);
  if (rng_state == 0)
    rng_state = 0x9e3779b9u;

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    printf("[NexRaptor] SDL_Init failed: %s\n", SDL_GetError());
    exit(1);
  }

  char title[64];
  sprintf(title, "NexRaptor PID %d", pid);
  SDL_Window *window = SDL_CreateWindow(title, 60, 60, WIN_W, WIN_H, 0);
  if (!window) {
    printf("[NexRaptor] SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    exit(1);
  }

  SDL_Surface *surface = SDL_GetWindowSurface(window);
  if (!surface) {
    OS1_notify_error("NexRaptor", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    exit(1);
  }

  /* Persistent high score, kept in the OS registry - the whole point of
   * the easter egg is that it remembers you across reboots. */
  char regbuf[32];
  int rn = OS1_registry_get("raptor.highscore", regbuf, sizeof(regbuf));
  high_score = (rn > 0) ? atoi(regbuf) : 0;

  init_stars();
  reset_game();

  char msg[64];
  snprintf(msg, sizeof(msg), "NexRaptor avviato! Record: %ld", high_score);
  OS1_notify_post("NexRaptor", msg);

  int running = 1;
  int bomb_edge = 0;
  int restart_edge = 0;
  int weapon_select = -1;

  Uint64 fps_last_ms = SDL_GetTicks64();
  int fps_frames = 0;

  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT)
        running = 0;
      if (event.type == SDL_WINDOWEVENT &&
          event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        surface = SDL_GetWindowSurface(window);
        if (!surface) {
          OS1_notify_error("NexRaptor", SDL_GetError());
          running = 0;
        }
      }
      if (event.type == SDL_KEYDOWN) {
        SDL_Scancode sc = event.key.keysym.scancode;
        if (sc == SDL_SCANCODE_ESCAPE)
          running = 0;
        if (sc == SDL_SCANCODE_X || sc == SDL_SCANCODE_LCTRL)
          bomb_edge = 1;
        if (sc == SDL_SCANCODE_R && state == STATE_GAMEOVER)
          restart_edge = 1;
        if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_5)
          weapon_select = sc - SDL_SCANCODE_1;
      }
    }
    if (!running)
      break;

    if (weapon_select >= 0 && weapon_select < WPN_COUNT &&
        (p_weapon_unlocked & (1u << weapon_select)))
      p_weapon = weapon_select;
    weapon_select = -1;

    const Uint8 *keys = SDL_GetKeyboardState(NULL);

    if (state == STATE_PLAYING) {
      update_player(keys);
      if (bomb_edge) {
        do_bomb();
      }
      update_world();
      update_stars();
    } else if (state == STATE_LEVEL_CLEAR) {
      update_stars();
      for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *pt = &particles[i];
        if (pt->active) {
          pt->x += pt->vx;
          pt->y += pt->vy;
          pt->life--;
          if (pt->life <= 0)
            pt->active = 0;
        }
      }
      state_timer--;
      if (state_timer <= 0) {
        level++;
        state = STATE_PLAYING;
        p_invuln = 60;
        start_wave();
      }
    } else { /* STATE_GAMEOVER */
      update_stars();
      if (state_timer == 0) {
        if (score > high_score) {
          high_score = score;
          char buf[16];
          snprintf(buf, sizeof(buf), "%ld", high_score);
          OS1_registry_set("raptor.highscore", buf);
          char m2[64];
          snprintf(m2, sizeof(m2), "Nuovo record! %ld punti", high_score);
          OS1_notify_post("NexRaptor", m2);
        } else {
          char m2[64];
          snprintf(m2, sizeof(m2), "Game Over - Punteggio %ld (Record %ld)",
                   score, high_score);
          OS1_notify_post("NexRaptor", m2);
        }
      }
      state_timer++;
      if (restart_edge) {
        reset_game();
      }
    }

    bomb_edge = 0;
    restart_edge = 0;

    render(surface);

    if (SDL_UpdateWindowSurface(window) != 0) {
      surface = SDL_GetWindowSurface(window);
      if (!surface) {
        OS1_notify_error("NexRaptor", SDL_GetError());
        running = 0;
      }
    }

    /* FPS counter: recomputed once a second from real elapsed time via the
     * NexsOS SDL timer backend, independent of the fixed ~60Hz frame pacing
     * below so it reflects actual throughput, not the target rate. */
    fps_frames++;
    Uint64 now_ms = SDL_GetTicks64();
    if (now_ms - fps_last_ms >= 1000) {
      fps_value = fps_frames;
      fps_frames = 0;
      fps_last_ms = now_ms;
    }

    SDL_Delay(16); /* ~60 FPS via the NexsOS SDL timer backend */
  }

  SDL_DestroyWindow(window);
  SDL_Quit();
  exit(0);
  return 0;
}