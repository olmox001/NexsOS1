/*
 * user/bin/nexempire.c
 * "NexEmpire" — Age of Empires 2 inspired RTS for NexsOS/SDL2
 *
 * A single-file real-time strategy game exercising the SDL2 software
 * framebuffer path exactly like sdltest.c and raptor.c.
 *
 * Features:
 *   - Scrollable tile map (80x60) with terrain and resources
 *   - 4 resource types: Wood, Food, Gold, Stone
 *   - 4 unit types: Villager, Militia, Spearman, Archer
 *   - 4 building types: Town Center, House, Barracks, Farm
 *   - Click/drag selection, right-click commands
 *   - Building placement and unit training
 *   - Simple AI opponent
 *   - Minimap, HUD, win/lose conditions
 *
 * Controls:
 *   Arrow keys  - Scroll camera
 *   Left click   - Select unit/building, drag for box select
 *   Right click  - Move / Gather / Attack
 *   H            - Place House mode
 *   B            - Place Barracks mode
 *   F            - Place Farm mode
 *   V            - Train Villager (TC selected)
 *   M            - Train Militia (Barracks selected)
 *   P            - Train Spearman (Barracks selected)
 *   A            - Train Archer (Barracks selected)
 *   DEL          - Delete selected building
 *   R            - Restart (game over)
 *   ESC          - Cancel placement / Quit
 */
#include "SDL.h"
#include <os1.h>

/* ---------------------------------------------------------------------- */
/* Constants                                                                */
/* ---------------------------------------------------------------------- */

#define WIN_W 800
#define WIN_H 600
#define TILE 32
#define MAP_W 80
#define MAP_H 60
#define HUD_H 76
#define TOP_H 30
#define VIEW_H (WIN_H - HUD_H - TOP_H)
#define MINI_W 140
#define MINI_H 105
#define MAX_U 100
#define MAX_B 40
#define MAX_SEL 20
/* particles removed for memory */
#define CAM_SPD 6
#define EDGE_Z 16
#define GATHER_TIME 90 /* frames to gather 10 resource */
#define CARRY_MAX 10

/* Terrain */
enum {
  T_GRASS = 0,
  T_WATER,
  T_FOREST,
  T_GOLD,
  T_STONE,
  T_BERRY,
  T_FARM_PLOT,
  T_SAND
};

/* Unit types */
enum { U_VILLAGER = 0, U_MILITIA, U_SPEARMAN, U_ARCHER, U_COUNT };

/* Unit states */
enum { S_IDLE = 0, S_MOVE, S_GATHER, S_BUILD, S_ATTACK, S_RETURN };

/* Building types */
enum { B_TC = 0, B_HOUSE, B_BARRACKS, B_FARM, B_COUNT };

/* Building states */
enum { BS_BUILD = 0, BS_DONE };

/* Placement modes */
enum { PM_NONE = 0, PM_HOUSE, PM_BARRACKS, PM_FARM };

/* Game states */
enum { GS_START = 0, GS_PLAY, GS_WIN, GS_LOSE };

/* Building sizes in tiles */
static const int BSIZE_W[B_COUNT] = {3, 2, 3, 2};
static const int BSIZE_H[B_COUNT] = {3, 2, 3, 2};
static const int BHP[B_COUNT] = {300, 100, 200, 50};
static const char *BNAME[B_COUNT] = {"Town Center", "House", "Barracks",
                                     "Farm"};
static const char *UNAME[U_COUNT] = {"Villager", "Militia", "Spearman",
                                     "Archer"};

/* Unit stats: hp, attack, range(tiles), speed(px/frame), attack_cd(frames) */
static const int UHP[U_COUNT] = {25, 45, 35, 30};
static const int UATK[U_COUNT] = {3, 6, 5, 4};
static const int URANGE[U_COUNT] = {1, 1, 1, 5};
static const int USPD[U_COUNT] = {2, 2, 2, 2};
static const int UATKCD[U_COUNT] = {30, 25, 25, 40};

/* Training costs: food, wood, gold, time(frames) */
/* Villager: 50f / Militia: 60f 20g / Spearman: 35f 25w / Archer: 30f 40w 25g */
static const int TRAIN_FOOD[B_COUNT][U_COUNT] = {
    {50, 0, 0, 0},   /* TC */
    {0, 0, 0, 0},    /* House */
    {0, 60, 35, 30}, /* Barracks */
    {0, 0, 0, 0},    /* Farm */
};
static const int TRAIN_WOOD[B_COUNT][U_COUNT] = {
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 25, 40},
    {0, 0, 0, 0},
};
static const int TRAIN_GOLD[B_COUNT][U_COUNT] = {
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 20, 0, 25},
    {0, 0, 0, 0},
};
static const int TRAIN_TIME[B_COUNT][U_COUNT] = {
    {150, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 120, 100, 140},
    {0, 0, 0, 0},
};

/* Building build costs */
static const int BCOST_FOOD[B_COUNT] = {0, 0, 0, 0};
static const int BCOST_WOOD[B_COUNT] = {100, 30, 150, 60};
static const int BCOST_GOLD[B_COUNT] = {0, 0, 0, 0};
static const int BCOST_STONE[B_COUNT] = {0, 0, 0, 0};
static const int BCOST_TIME[B_COUNT] = {300, 120, 240, 80};

/* Pop provided by TC=5, House=5, Barracks=0, Farm=0 */
static const int BPOP[B_COUNT] = {5, 5, 0, 0};

/* Terrain colors (R,G,B) */
static const Uint8 TCOLOR[][3] = {
    {82, 148, 58},   /* GRASS */
    {40, 90, 180},   /* WATER */
    {34, 100, 34},   /* FOREST */
    {200, 180, 50},  /* GOLD */
    {140, 140, 140}, /* STONE */
    {180, 60, 60},   /* BERRY */
    {120, 90, 40},   /* FARM_PLOT */
    {194, 178, 128}, /* SAND */
};

/* Unit colors per owner */
static const Uint8 UCOL[2][U_COUNT][3] = {
    /* Player (blue shades) */
    {{60, 120, 220}, {50, 100, 200}, {40, 90, 190}, {70, 140, 230}},
    /* AI (red shades) */
    {{220, 60, 60}, {200, 50, 50}, {190, 40, 40}, {230, 70, 70}},
};

/* Building colors per owner */
static const Uint8 BCOL[2][B_COUNT][3] = {
    /* Player */
    {{140, 120, 80}, {160, 130, 80}, {130, 100, 70}, {120, 80, 40}},
    /* AI */
    {{160, 80, 80}, {180, 90, 90}, {150, 70, 70}, {140, 60, 50}},
};

/* ---------------------------------------------------------------------- */
/* XORShift PRNG                                                           */
/* ---------------------------------------------------------------------- */
static unsigned int rng_s = 0x9e3779b9u;
static unsigned int rng_next(void) {
  unsigned int x = rng_s;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_s = x;
  return x;
}
static int rng_range(int lo, int hi) {
  if (hi <= lo)
    return lo;
  return lo + (int)(rng_next() % (unsigned)(hi - lo + 1));
}

/* ---------------------------------------------------------------------- */
/* Data Structures                                                          */
/* ---------------------------------------------------------------------- */
typedef struct {
  int wood, food, gold, stone;
} Res;

typedef struct {
  int terrain;
  int res_amt;  /* remaining resource on this tile */
  int res_type; /* 0=wood,1=food,2=gold,3=stone */
  int bid;      /* building index on this tile, -1=none */
} Tile;

typedef struct {
  int active;
  int x, y; /* pixel center */
  int type; /* U_* */
  int owner;
  int hp, hp_max;
  int state;
  int tx, ty;     /* target pixel */
  int tid;        /* target unit/building id for gather/attack */
  int carry_type; /* 0-3 or -1 */
  int carry_amt;
  int atk_cd;
  int gather_t;
  int flash;
} Unit;

typedef struct {
  int active;
  int tx, ty; /* tile grid top-left */
  int type;
  int owner;
  int hp, hp_max;
  int state;      /* BS_BUILD or BS_DONE */
  int progress;   /* 0-100 build progress */
  int train_type; /* U_* or -1 */
  int train_timer;
  int sw, sh;   /* size in tiles */
  int builders; /* number of villagers building this */
} Building;

/* ---------------------------------------------------------------------- */
/* Global State                                                             */
/* ---------------------------------------------------------------------- */
static Tile map[MAP_W * MAP_H];
static Unit units[MAX_U];
static Building blds[MAX_B];
static Res res[2]; /* per-player resources */

static int cam_x, cam_y;
static int mx, my;       /* mouse position in window */
static int mwx, mwy;     /* mouse world position */
static int sel[MAX_SEL]; /* selected unit indices, -1 = none */
static int sel_count;
static int sel_bid; /* selected building index, -1 = none */

static int dragging;
static int drag_sx, drag_sy;

static int place_mode; /* PM_* */
static int game_state;
static int frame;
static int game_time; /* seconds */

static int ai_timer;
static int ai_attack_sent;

static int wins, losses;
static Uint32 notif_timer;
static char notif_text[80];

/* Pop tracking */
static int pop[2];     /* current unit count */
static int pop_cap[2]; /* max population */

/* ---------------------------------------------------------------------- */
/* Helpers                                                                  */
/* ---------------------------------------------------------------------- */
static int tile_idx(int x, int y) { return y * MAP_W + x; }

static int tile_at(int px, int py) {
  int tx = px / TILE, ty = py / TILE;
  if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= MAP_H)
    return -1;
  return tile_idx(tx, ty);
}

static int walkable(int tx, int ty) {
  if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= MAP_H)
    return 0;
  Tile *t = &map[tile_idx(tx, ty)];
  return t->terrain != T_WATER;
}

static int dist2(int x1, int y1, int x2, int y2) {
  int dx = x1 - x2, dy = y1 - y2;
  return dx * dx + dy * dy;
}

static int dist_px(int x1, int y1, int x2, int y2) {
  int dx = x1 - x2, dy = y1 - y2;
  if (dx < 0)
    dx = -dx;
  if (dy < 0)
    dy = -dy;
  /* approximate distance for speed */
  return dx + dy;
}

static void show_notif(const char *msg) {
  int i;
  for (i = 0; msg[i] && i < 79; i++)
    notif_text[i] = msg[i];
  notif_text[i] = 0;
  notif_timer = 180;
  OS1_notify_post("NexEmpire", msg);
}

static void clamp_cam(void) {
  int max_x = MAP_W * TILE - WIN_W;
  int max_y = MAP_H * TILE - (WIN_H - HUD_H - TOP_H);
  if (max_x < 0)
    max_x = 0;
  if (max_y < 0)
    max_y = 0;
  if (cam_x < 0)
    cam_x = 0;
  if (cam_y < 0)
    cam_y = 0;
  if (cam_x > max_x)
    cam_x = max_x;
  if (cam_y > max_y)
    cam_y = max_y;
}

static int spawn_unit(int type, int owner, int px, int py) {
  for (int i = 0; i < MAX_U; i++) {
    if (!units[i].active) {
      Unit *u = &units[i];
      u->active = 1;
      u->x = px;
      u->y = py;
      u->type = type;
      u->owner = owner;
      u->hp = UHP[type];
      u->hp_max = UHP[type];
      u->state = S_IDLE;
      u->tx = px;
      u->ty = py;
      u->tid = -1;
      u->carry_type = -1;
      u->carry_amt = 0;
      u->atk_cd = 0;
      u->gather_t = 0;
      u->flash = 0;
      pop[owner]++;
      return i;
    }
  }
  return -1;
}

static int spawn_building(int type, int owner, int tx, int ty) {
  for (int i = 0; i < MAX_B; i++) {
    if (!blds[i].active) {
      Building *b = &blds[i];
      b->active = 1;
      b->tx = tx;
      b->ty = ty;
      b->type = type;
      b->owner = owner;
      b->hp = 1;
      b->hp_max = BHP[type];
      b->state = (type == B_TC) ? BS_DONE : BS_BUILD;
      b->progress = (type == B_TC) ? 100 : 0;
      b->train_type = -1;
      b->train_timer = 0;
      b->sw = BSIZE_W[type];
      b->sh = BSIZE_H[type];
      b->builders = 0;
      /* Mark tiles */
      for (int dy = 0; dy < b->sh; dy++)
        for (int dx = 0; dx < b->sw; dx++) {
          int mx2 = tx + dx, my2 = ty + dy;
          if (mx2 < MAP_W && my2 < MAP_H)
            map[tile_idx(mx2, my2)].bid = i;
        }
      if (b->state == BS_DONE) {
        pop_cap[owner] += BPOP[type];
      }
      return i;
    }
  }
  return -1;
}

static void remove_building(int i) {
  Building *b = &blds[i];
  if (!b->active)
    return;
  for (int dy = 0; dy < b->sh; dy++)
    for (int dx = 0; dx < b->sw; dx++) {
      int mx2 = b->tx + dx, my2 = b->ty + dy;
      if (mx2 < MAP_W && my2 < MAP_H)
        map[tile_idx(mx2, my2)].bid = -1;
    }
  if (b->state == BS_DONE) {
    pop_cap[b->owner] -= BPOP[b->type];
    if (pop_cap[b->owner] < 0)
      pop_cap[b->owner] = 0;
  }
  b->active = 0;
}

static void remove_unit(int i) {
  if (!units[i].active)
    return;
  pop[units[i].owner]--;
  if (pop[units[i].owner] < 0)
    pop[units[i].owner] = 0;
  units[i].active = 0;
}

static int find_nearest_tc(int owner, int px, int py) {
  int best = -1, best_d = 9999999;
  for (int i = 0; i < MAX_B; i++) {
    Building *b = &blds[i];
    if (!b->active || b->owner != owner || b->type != B_TC ||
        b->state != BS_DONE)
      continue;
    int bx = (b->tx + b->sw / 2) * TILE;
    int by = (b->ty + b->sh / 2) * TILE;
    int d = dist_px(px, py, bx, by);
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

static int unit_at(int px, int py, int owner) {
  int best = -1, best_d = 256; /* 16px radius */
  for (int i = 0; i < MAX_U; i++) {
    if (!units[i].active)
      continue;
    if (owner >= 0 && units[i].owner != owner)
      continue;
    int d = dist_px(px, py, units[i].x, units[i].y);
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

static int building_at_tile(int tx, int ty) {
  if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= MAP_H)
    return -1;
  return map[tile_idx(tx, ty)].bid;
}

/* ---------------------------------------------------------------------- */
/* Map Generation                                                           */
/* ---------------------------------------------------------------------- */
static void generate_map(void) {
  int x, y, i;
  /* Clear */
  for (y = 0; y < MAP_H; y++)
    for (x = 0; x < MAP_W; x++) {
      Tile *t = &map[tile_idx(x, y)];
      t->terrain = T_GRASS;
      t->res_amt = 0;
      t->res_type = -1;
      t->bid = -1;
    }

  /* River: winding from top to bottom */
  {
    int rx = 35 + rng_range(-5, 5);
    for (y = 0; y < MAP_H; y++) {
      int rw = 2 + rng_range(0, 2);
      for (int dx = -rw; dx <= rw; dx++) {
        int wx = rx + dx;
        if (wx >= 0 && wx < MAP_W)
          map[tile_idx(wx, y)].terrain = T_WATER;
      }
      rx += rng_range(-1, 1);
      if (rx < 20)
        rx = 20;
      if (rx > 60)
        rx = 60;
    }
  }

  /* Forests */
  for (i = 0; i < 25; i++) {
    int fx = rng_range(2, MAP_W - 10);
    int fy = rng_range(2, MAP_H - 10);
    int fs = rng_range(3, 7);
    for (int dy = 0; dy < fs; dy++)
      for (int dx = 0; dx < fs; dx++) {
        int tx = fx + dx, ty = fy + dy;
        if (tx >= MAP_W || ty >= MAP_H)
          continue;
        if (map[tile_idx(tx, ty)].terrain == T_WATER)
          continue;
        if (rng_range(0, 100) < 60) {
          map[tile_idx(tx, ty)].terrain = T_FOREST;
          map[tile_idx(tx, ty)].res_amt = 100;
          map[tile_idx(tx, ty)].res_type = 0; /* wood */
        }
      }
  }

  /* Gold mines */
  for (i = 0; i < 5; i++) {
    int gx = rng_range(3, MAP_W - 8);
    int gy = rng_range(3, MAP_H - 8);
    int gs = rng_range(2, 4);
    for (int dy = 0; dy < gs; dy++)
      for (int dx = 0; dx < gs; dx++) {
        int tx = gx + dx, ty = gy + dy;
        if (tx >= MAP_W || ty >= MAP_H)
          continue;
        if (map[tile_idx(tx, ty)].terrain == T_WATER)
          continue;
        map[tile_idx(tx, ty)].terrain = T_GOLD;
        map[tile_idx(tx, ty)].res_amt = 200;
        map[tile_idx(tx, ty)].res_type = 2;
      }
  }

  /* Stone mines */
  for (i = 0; i < 4; i++) {
    int sx = rng_range(3, MAP_W - 7);
    int sy = rng_range(3, MAP_H - 7);
    int ss = rng_range(2, 3);
    for (int dy = 0; dy < ss; dy++)
      for (int dx = 0; dx < ss; dx++) {
        int tx = sx + dx, ty = sy + dy;
        if (tx >= MAP_W || ty >= MAP_H)
          continue;
        if (map[tile_idx(tx, ty)].terrain == T_WATER)
          continue;
        map[tile_idx(tx, ty)].terrain = T_STONE;
        map[tile_idx(tx, ty)].res_amt = 150;
        map[tile_idx(tx, ty)].res_type = 3;
      }
  }

  /* Clear starting areas and add resources */
  /* Player 1: bottom-left */
  for (int dy = -3; dy <= 6; dy++)
    for (int dx = -3; dx <= 6; dx++) {
      int tx = 5 + dx, ty = MAP_H - 8 + dy;
      if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= MAP_H)
        continue;
      map[tile_idx(tx, ty)].terrain = T_GRASS;
      map[tile_idx(tx, ty)].res_amt = 0;
    }
  /* Trees near P1 */
  for (i = 0; i < 12; i++) {
    int tx = rng_range(9, 20), ty = rng_range(MAP_H - 15, MAP_H - 5);
    if (tx < MAP_W && ty >= 0 && ty < MAP_H &&
        map[tile_idx(tx, ty)].terrain == T_GRASS) {
      map[tile_idx(tx, ty)].terrain = T_FOREST;
      map[tile_idx(tx, ty)].res_amt = 100;
      map[tile_idx(tx, ty)].res_type = 0;
    }
  }
  /* Gold near P1 */
  {
    int gx = 12, gy = MAP_H - 12;
    for (int dy = 0; dy < 3; dy++)
      for (int dx = 0; dx < 3; dx++) {
        if (gx + dx < MAP_W && gy + dy >= 0 && gy + dy < MAP_H) {
          map[tile_idx(gx + dx, gy + dy)].terrain = T_GOLD;
          map[tile_idx(gx + dx, gy + dy)].res_amt = 300;
          map[tile_idx(gx + dx, gy + dy)].res_type = 2;
        }
      }
  }
  /* Berries near P1 */
  for (i = 0; i < 6; i++) {
    int tx = rng_range(2, 6), ty = rng_range(MAP_H - 14, MAP_H - 10);
    if (tx >= 0 && tx < MAP_W && ty >= 0 && ty < MAP_H &&
        map[tile_idx(tx, ty)].terrain == T_GRASS) {
      map[tile_idx(tx, ty)].terrain = T_BERRY;
      map[tile_idx(tx, ty)].res_amt = 125;
      map[tile_idx(tx, ty)].res_type = 1;
    }
  }

  /* Player 2 (AI): top-right */
  for (int dy = -3; dy <= 6; dy++)
    for (int dx = -3; dx <= 6; dx++) {
      int tx = MAP_W - 8 + dx, ty = 2 + dy;
      if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= MAP_H)
        continue;
      map[tile_idx(tx, ty)].terrain = T_GRASS;
      map[tile_idx(tx, ty)].res_amt = 0;
    }
  for (i = 0; i < 12; i++) {
    int tx = rng_range(MAP_W - 22, MAP_W - 9), ty = rng_range(5, 18);
    if (tx >= 0 && tx < MAP_W && ty < MAP_H &&
        map[tile_idx(tx, ty)].terrain == T_GRASS) {
      map[tile_idx(tx, ty)].terrain = T_FOREST;
      map[tile_idx(tx, ty)].res_amt = 100;
      map[tile_idx(tx, ty)].res_type = 0;
    }
  }
  {
    int gx = MAP_W - 15, gy = 8;
    for (int dy = 0; dy < 3; dy++)
      for (int dx = 0; dx < 3; dx++) {
        if (gx + dx >= 0 && gx + dx < MAP_W && gy + dy < MAP_H) {
          map[tile_idx(gx + dx, gy + dy)].terrain = T_GOLD;
          map[tile_idx(gx + dx, gy + dy)].res_amt = 300;
          map[tile_idx(gx + dx, gy + dy)].res_type = 2;
        }
      }
  }
  for (i = 0; i < 6; i++) {
    int tx = rng_range(MAP_W - 7, MAP_W - 2), ty = rng_range(8, 14);
    if (tx >= 0 && tx < MAP_W && ty < MAP_H &&
        map[tile_idx(tx, ty)].terrain == T_GRASS) {
      map[tile_idx(tx, ty)].terrain = T_BERRY;
      map[tile_idx(tx, ty)].res_amt = 125;
      map[tile_idx(tx, ty)].res_type = 1;
    }
  }

  /* Stone near both bases */
  {
    int sx = 8, sy = MAP_H - 15;
    for (int dy = 0; dy < 2; dy++)
      for (int dx = 0; dx < 2; dx++) {
        if (sx + dx < MAP_W && sy + dy >= 0 && sy + dy < MAP_H) {
          map[tile_idx(sx + dx, sy + dy)].terrain = T_STONE;
          map[tile_idx(sx + dx, sy + dy)].res_amt = 200;
          map[tile_idx(sx + dx, sy + dy)].res_type = 3;
        }
      }
  }
  {
    int sx = MAP_W - 10, sy = 12;
    for (int dy = 0; dy < 2; dy++)
      for (int dx = 0; dx < 2; dx++) {
        if (sx + dx >= 0 && sx + dx < MAP_W && sy + dy < MAP_H) {
          map[tile_idx(sx + dx, sy + dy)].terrain = T_STONE;
          map[tile_idx(sx + dx, sy + dy)].res_amt = 200;
          map[tile_idx(sx + dx, sy + dy)].res_type = 3;
        }
      }
  }
}

/* ---------------------------------------------------------------------- */
/* Game Init                                                                */
/* ---------------------------------------------------------------------- */
static void init_game(void) {
  int i;
  for (i = 0; i < MAX_U; i++)
    units[i].active = 0;
  for (i = 0; i < MAX_B; i++)
    blds[i].active = 0;
  for (i = 0; i < 2; i++) {
    res[i].wood = 200;
    res[i].food = 200;
    res[i].gold = 100;
    res[i].stone = 0;
    pop[i] = 0;
    pop_cap[i] = 0;
  }

  rng_s = (unsigned int)(get_time() ^ get_pid());
  if (rng_s == 0)
    rng_s = 0x12345678u;

  generate_map();

  /* Player 1 town center at tile (5, MAP_H-8) */
  spawn_building(B_TC, 0, 5, MAP_H - 8);
  /* 3 villagers */
  spawn_unit(U_VILLAGER, 0, 7 * TILE, (MAP_H - 5) * TILE);
  spawn_unit(U_VILLAGER, 0, 9 * TILE, (MAP_H - 5) * TILE);
  spawn_unit(U_VILLAGER, 0, 7 * TILE, (MAP_H - 3) * TILE);

  /* AI town center at tile (MAP_W-8, 2) */
  spawn_building(B_TC, 1, MAP_W - 8, 2);
  /* 3 villagers */
  spawn_unit(U_VILLAGER, 1, (MAP_W - 6) * TILE, 5 * TILE);
  spawn_unit(U_VILLAGER, 1, (MAP_W - 4) * TILE, 5 * TILE);
  spawn_unit(U_VILLAGER, 1, (MAP_W - 6) * TILE, 7 * TILE);

  cam_x = 0;
  cam_y = (MAP_H * TILE) - WIN_H + TOP_H + HUD_H;
  clamp_cam();

  sel_count = 0;
  sel_bid = -1;
  dragging = 0;
  place_mode = PM_NONE;
  frame = 0;
  game_time = 0;
  ai_timer = 0;
  ai_attack_sent = 0;
  notif_timer = 0;
  game_state = GS_START;
}

/* ---------------------------------------------------------------------- */
/* Can place building check                                                 */
/* ---------------------------------------------------------------------- */
static int can_place(int type, int owner, int tx, int ty) {
  int sw = BSIZE_W[type], sh = BSIZE_H[type];
  for (int dy = 0; dy < sh; dy++)
    for (int dx = 0; dx < sw; dx++) {
      int cx = tx + dx, cy = ty + dy;
      if (cx < 0 || cx >= MAP_W || cy < 0 || cy >= MAP_H)
        return 0;
      Tile *t = &map[tile_idx(cx, cy)];
      if (t->terrain == T_WATER)
        return 0;
      if (t->bid >= 0)
        return 0;
      if (t->terrain == T_FOREST || t->terrain == T_GOLD ||
          t->terrain == T_STONE || t->terrain == T_BERRY)
        return 0;
    }
  /* Must be near an owned building */
  int near = 0;
  for (int i = 0; i < MAX_B && !near; i++) {
    if (!blds[i].active || blds[i].owner != owner)
      continue;
    int bx1 = blds[i].tx - 5, by1 = blds[i].ty - 5;
    int bx2 = blds[i].tx + blds[i].sw + 4, by2 = blds[i].ty + blds[i].sh + 4;
    if (tx + sw > bx1 && tx < bx2 && ty + sh > by1 && ty < by2)
      near = 1;
  }
  return near;
}

/* ---------------------------------------------------------------------- */
/* Unit AI                                                                  */
/* ---------------------------------------------------------------------- */
static void update_unit(int idx) {
  Unit *u = &units[idx];
  if (!u->active)
    return;

  if (u->flash > 0)
    u->flash--;
  if (u->atk_cd > 0)
    u->atk_cd--;

  switch (u->state) {
  case S_IDLE:
    /* Auto-attack nearby enemies */
    if (u->atk_cd == 0) {
      int best = -1, best_d = URANGE[u->type] * TILE;
      best_d *= best_d;
      for (int i = 0; i < MAX_U; i++) {
        if (i == idx || !units[i].active || units[i].owner == u->owner)
          continue;
        int d = dist2(u->x, u->y, units[i].x, units[i].y);
        if (d < best_d) {
          best_d = d;
          best = i;
        }
      }
      if (best < 0) {
        /* Also check buildings */
        for (int i = 0; i < MAX_B; i++) {
          if (!blds[i].active || blds[i].owner == u->owner)
            continue;
          int bx = (blds[i].tx + blds[i].sw / 2) * TILE;
          int by = (blds[i].ty + blds[i].sh / 2) * TILE;
          int d = dist2(u->x, u->y, bx, by);
          if (d < best_d) {
            best_d = d;
            best = -(i + 1);
          }
        }
      }
      if (best >= 0) {
        u->state = S_ATTACK;
        u->tid = best;
      } else if (best < -1) {
        /* attack building: move toward it */
        int bi = -(best + 1);
        int bx = (blds[bi].tx + blds[bi].sw / 2) * TILE;
        int by = (blds[bi].ty + blds[bi].sh / 2) * TILE;
        if (dist_px(u->x, u->y, bx, by) <= URANGE[u->type] * TILE) {
          u->state = S_ATTACK;
          u->tid = best; /* negative = building index */
        }
      }
    }
    break;

  case S_MOVE: {
    int dx = u->tx - u->x, dy = u->ty - u->y;
    int d = dist_px(u->x, u->y, u->tx, u->ty);
    if (d < USPD[u->type] + 1) {
      u->x = u->tx;
      u->y = u->ty;
      u->state = S_IDLE;
    } else {
      /* Simple normalized movement */
      int step = USPD[u->type];
      if (d > 0) {
        u->x += (dx * step) / d;
        u->y += (dy * step) / d;
      }
    }
    /* Clamp to map */
    if (u->x < 8)
      u->x = 8;
    if (u->y < 8)
      u->y = 8;
    if (u->x > (MAP_W - 1) * TILE)
      u->x = (MAP_W - 1) * TILE;
    if (u->y > (MAP_H - 1) * TILE)
      u->y = (MAP_H - 1) * TILE;
    break;
  }

  case S_GATHER: {
    if (u->tid < 0) {
      u->state = S_IDLE;
      break;
    }
    Tile *t = &map[u->tid];
    if (t->res_amt <= 0) {
      u->state = S_IDLE;
      u->tid = -1;
      break;
    }
    /* Move to resource if not adjacent */
    int rx = (u->tid % MAP_W) * TILE + TILE / 2;
    int ry = (u->tid / MAP_W) * TILE + TILE / 2;
    int d = dist_px(u->x, u->y, rx, ry);
    if (d > TILE + 4) {
      u->tx = rx;
      u->ty = ry;
      /* Inline movement */
      int dx = u->tx - u->x, dy = u->ty - u->y;
      int dd = dist_px(u->x, u->y, u->tx, u->ty);
      if (dd > 0) {
        int step = USPD[u->type];
        u->x += (dx * step) / dd;
        u->y += (dy * step) / dd;
      }
    } else {
      /* Gather */
      u->gather_t++;
      if (u->gather_t >= GATHER_TIME) {
        u->gather_t = 0;
        int take = 10;
        if (take > t->res_amt)
          take = t->res_amt;
        t->res_amt -= take;
        u->carry_type = t->res_type;
        u->carry_amt += take;
        if (t->res_amt <= 0) {
          t->terrain = T_GRASS;
          t->res_type = -1;
        }
        /* Return to TC if at carry capacity */
        if (u->carry_amt >= CARRY_MAX) {
          u->state = S_RETURN;
        }
      }
    }
    break;
  }

  case S_RETURN: {
    int tci = find_nearest_tc(u->owner, u->x, u->y);
    if (tci < 0) {
      u->state = S_IDLE;
      break;
    }
    Building *tc = &blds[tci];
    int bx = (tc->tx + tc->sw / 2) * TILE;
    int by = (tc->ty + tc->sh / 2) * TILE;
    int d = dist_px(u->x, u->y, bx, by);
    if (d > TILE * 2) {
      u->tx = bx;
      u->ty = by;
      int dx = u->tx - u->x, dy = u->ty - u->y;
      int dd = dist_px(u->x, u->y, u->tx, u->ty);
      if (dd > 0) {
        int step = USPD[u->type];
        u->x += (dx * step) / dd;
        u->y += (dy * step) / dd;
      }
    } else {
      /* Drop off resources */
      Res *r = &res[u->owner];
      if (u->carry_type == 0)
        r->wood += u->carry_amt;
      else if (u->carry_type == 1)
        r->food += u->carry_amt;
      else if (u->carry_type == 2)
        r->gold += u->carry_amt;
      else if (u->carry_type == 3)
        r->stone += u->carry_amt;
      u->carry_amt = 0;
      u->carry_type = -1;
      /* Go back to gathering if resource still exists */
      if (u->tid >= 0 && map[u->tid].res_amt > 0) {
        u->state = S_GATHER;
      } else {
        u->state = S_IDLE;
        u->tid = -1;
      }
    }
    break;
  }

  case S_BUILD: {
    if (u->tid < 0) {
      u->state = S_IDLE;
      break;
    }
    Building *b = &blds[u->tid];
    if (!b->active || b->state != BS_BUILD) {
      u->state = S_IDLE;
      break;
    }
    /* Move to building */
    int bx = (b->tx + b->sw / 2) * TILE;
    int by = (b->ty + b->sh / 2) * TILE;
    int d = dist_px(u->x, u->y, bx, by);
    if (d > TILE * 3) {
      u->tx = bx;
      u->ty = by;
      int dx = u->tx - u->x, dy = u->ty - u->y;
      int dd = dist_px(u->x, u->y, u->tx, u->ty);
      if (dd > 0) {
        int step = USPD[u->type];
        u->x += (dx * step) / dd;
        u->y += (dy * step) / dd;
      }
    } else {
      /* Build */
      b->progress += 1;
      if (b->progress >= 100) {
        b->state = BS_DONE;
        b->hp = b->hp_max;
        pop_cap[b->owner] += BPOP[b->type];
        if (b->owner == 0) {
          char msg[64];
          snprintf(msg, sizeof(msg), "%s completato!", BNAME[b->type]);
          show_notif(msg);
        }
      }
    }
    break;
  }

  case S_ATTACK: {
    if (u->atk_cd > 0)
      break;
    if (u->tid >= 0) {
      /* Attack unit */
      if (u->tid < MAX_U && !units[u->tid].active) {
        u->state = S_IDLE;
        break;
      }
      Unit *target = &units[u->tid];
      int d = dist_px(u->x, u->y, target->x, target->y);
      if (u->type == U_ARCHER) {
        /* Archer: stop and shoot */
        if (d <= URANGE[U_ARCHER] * TILE) {
          target->hp -= UATK[u->type];
          target->flash = 6;
          u->atk_cd = UATKCD[u->type];
          if (target->hp <= 0)
            remove_unit(u->tid);
          u->state = S_IDLE;
        } else {
          u->tx = target->x;
          u->ty = target->y;
          int dx = u->tx - u->x, dy = u->ty - u->y;
          int dd = dist_px(u->x, u->y, u->tx, u->ty);
          if (dd > 0) {
            int step = USPD[u->type];
            u->x += (dx * step) / dd;
            u->y += (dy * step) / dd;
          }
        }
      } else {
        /* Melee */
        if (d <= TILE + 4) {
          target->hp -= UATK[u->type];
          target->flash = 6;
          u->atk_cd = UATKCD[u->type];
          if (target->hp <= 0)
            remove_unit(u->tid);
          u->state = S_IDLE;
        } else {
          u->tx = target->x;
          u->ty = target->y;
          int dx = u->tx - u->x, dy = u->ty - u->y;
          int dd = dist_px(u->x, u->y, u->tx, u->ty);
          if (dd > 0) {
            int step = USPD[u->type];
            u->x += (dx * step) / dd;
            u->y += (dy * step) / dd;
          }
        }
      }
    } else if (u->tid < 0 && u->tid != -1) {
      /* Attack building (tid is -(bi+1)) */
      int bi = -(u->tid + 1);
      if (bi < 0 || bi >= MAX_B || !blds[bi].active) {
        u->state = S_IDLE;
        break;
      }
      Building *bld = &blds[bi];
      int bx = (bld->tx + bld->sw / 2) * TILE;
      int by = (bld->ty + bld->sh / 2) * TILE;
      int d = dist_px(u->x, u->y, bx, by);
      int range = URANGE[u->type] * TILE;
      if (u->type == U_ARCHER)
        range += TILE;
      if (d <= range) {
        bld->hp -= UATK[u->type];
        u->atk_cd = UATKCD[u->type];
        if (bld->hp <= 0) {
          if (bld->type == B_TC) {
            /* Check win/lose */
            int other_tc = 0;
            for (int j = 0; j < MAX_B; j++)
              if (blds[j].active && blds[j].type == B_TC &&
                  blds[j].owner != bld->owner)
                other_tc = 1;
            if (!other_tc) {
              game_state = (bld->owner == 1) ? GS_WIN : GS_LOSE;
              if (game_state == GS_WIN) {
                wins++;
                show_notif("Vittoria!");
              } else {
                losses++;
                show_notif("Sconfitta!");
              }
            }
          }
          remove_building(bi);
          if (u->owner == 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Edificio nemico distrutto!");
            show_notif(msg);
          }
        }
        u->state = S_IDLE;
      } else {
        u->tx = bx;
        u->ty = by;
        int dx = u->tx - u->x, dy = u->ty - u->y;
        int dd = dist_px(u->x, u->y, u->tx, u->ty);
        if (dd > 0) {
          int step = USPD[u->type];
          u->x += (dx * step) / dd;
          u->y += (dy * step) / dd;
        }
      }
    }
    break;
  }
  }
}

/* ---------------------------------------------------------------------- */
/* Building AI (training)                                                   */
/* ---------------------------------------------------------------------- */
static void update_buildings(void) {
  for (int i = 0; i < MAX_B; i++) {
    Building *b = &blds[i];
    if (!b->active || b->state != BS_DONE)
      continue;
    if (b->train_type < 0)
      continue;

    b->train_timer--;
    if (b->train_timer <= 0) {
      /* Spawn unit near building */
      int sx = (b->tx + b->sw) * TILE + 8;
      int sy = (b->ty + b->sh / 2) * TILE;
      if (pop[b->owner] < pop_cap[b->owner]) {
        int ui = spawn_unit(b->train_type, b->owner, sx, sy);
        if (ui >= 0 && b->owner == 0) {
          char msg[64];
          snprintf(msg, sizeof(msg), "%s addestrato!", UNAME[b->train_type]);
          show_notif(msg);
        }
      }
      b->train_type = -1;
      b->train_timer = 0;
    }
  }
}

/* ---------------------------------------------------------------------- */
/* AI Opponent                                                              */
/* ---------------------------------------------------------------------- */
static void ai_update(void) {
  int owner = 1;
  Res *r = &res[owner];
  ai_timer++;

  if (ai_timer % 60 == 0) {
    /* Count units by type */
    int villagers = 0, military = 0;
    for (int i = 0; i < MAX_U; i++) {
      if (!units[i].active || units[i].owner != owner)
        continue;
      if (units[i].type == U_VILLAGER)
        villagers++;
      else
        military++;
    }
    int house_count = 0, barracks_count = 0;
    for (int i = 0; i < MAX_B; i++) {
      if (!blds[i].active || blds[i].owner != owner || blds[i].state != BS_DONE)
        continue;
      if (blds[i].type == B_HOUSE)
        house_count++;
      if (blds[i].type == B_BARRACKS)
        barracks_count++;
    }

    /* Build house if near pop cap */
    if (pop[owner] >= pop_cap[owner] - 2 && r->wood >= BCOST_WOOD[B_HOUSE]) {
      /* Find place near TC */
      int tc = -1;
      for (int i = 0; i < MAX_B; i++)
        if (blds[i].active && blds[i].owner == owner && blds[i].type == B_TC) {
          tc = i;
          break;
        }
      if (tc >= 0) {
        Building *tcb = &blds[tc];
        for (int attempt = 0; attempt < 20; attempt++) {
          int tx = tcb->tx + rng_range(-4, tcb->sw + 3);
          int ty = tcb->ty + rng_range(-4, tcb->sh + 3);
          if (can_place(B_HOUSE, owner, tx, ty)) {
            r->wood -= BCOST_WOOD[B_HOUSE];
            int bi = spawn_building(B_HOUSE, owner, tx, ty);
            /* Assign idle villagers to build */
            for (int j = 0; j < MAX_U && blds[bi].builders < 2; j++) {
              if (units[j].active && units[j].owner == owner &&
                  units[j].type == U_VILLAGER && units[j].state == S_IDLE) {
                units[j].state = S_BUILD;
                units[j].tid = bi;
                blds[bi].builders++;
              }
            }
            break;
          }
        }
      }
    }

    /* Build barracks if none */
    if (barracks_count == 0 && r->wood >= BCOST_WOOD[B_BARRACKS]) {
      int tc = -1;
      for (int i = 0; i < MAX_B; i++)
        if (blds[i].active && blds[i].owner == owner && blds[i].type == B_TC) {
          tc = i;
          break;
        }
      if (tc >= 0) {
        Building *tcb = &blds[tc];
        for (int attempt = 0; attempt < 20; attempt++) {
          int tx = tcb->tx + rng_range(-5, tcb->sw + 4);
          int ty = tcb->ty + rng_range(-5, tcb->sh + 4);
          if (can_place(B_BARRACKS, owner, tx, ty)) {
            r->wood -= BCOST_WOOD[B_BARRACKS];
            int bi = spawn_building(B_BARRACKS, owner, tx, ty);
            for (int j = 0; j < MAX_U && blds[bi].builders < 3; j++) {
              if (units[j].active && units[j].owner == owner &&
                  units[j].type == U_VILLAGER && units[j].state == S_IDLE) {
                units[j].state = S_BUILD;
                units[j].tid = bi;
                blds[bi].builders++;
              }
            }
            break;
          }
        }
      }
    }

    /* Train villagers at TC */
    for (int i = 0; i < MAX_B; i++) {
      if (!blds[i].active || blds[i].owner != owner || blds[i].type != B_TC ||
          blds[i].train_type >= 0)
        continue;
      if (r->food >= 50 && pop[owner] < pop_cap[owner] && villagers < 8) {
        r->food -= 50;
        blds[i].train_type = U_VILLAGER;
        blds[i].train_timer = 150;
        break;
      }
    }

    /* Train military at barracks */
    for (int i = 0; i < MAX_B; i++) {
      if (!blds[i].active || blds[i].owner != owner ||
          blds[i].type != B_BARRACKS || blds[i].train_type >= 0)
        continue;
      if (r->food >= 60 && r->gold >= 20 && pop[owner] < pop_cap[owner]) {
        int utype = rng_range(1, 3);
        if (utype == U_MILITIA && r->food >= 60 && r->gold >= 20) {
          r->food -= 60;
          r->gold -= 20;
          blds[i].train_type = U_MILITIA;
          blds[i].train_timer = 120;
        } else if (utype == U_SPEARMAN && r->food >= 35 && r->wood >= 25) {
          r->food -= 35;
          r->wood -= 25;
          blds[i].train_type = U_SPEARMAN;
          blds[i].train_timer = 100;
        } else if (r->food >= 30 && r->wood >= 40 && r->gold >= 25) {
          r->food -= 30;
          r->wood -= 40;
          r->gold -= 25;
          blds[i].train_type = U_ARCHER;
          blds[i].train_timer = 140;
        }
        break;
      }
    }

    /* Send idle villagers to gather */
    for (int i = 0; i < MAX_U; i++) {
      if (!units[i].active || units[i].owner != owner)
        continue;
      if (units[i].type != U_VILLAGER || units[i].state != S_IDLE)
        continue;
      /* Find nearest resource */
      int best_t = -1, best_d = 999999;
      for (int ty2 = 0; ty2 < MAP_H; ty2++)
        for (int tx2 = 0; tx2 < MAP_W; tx2++) {
          Tile *t = &map[tile_idx(tx2, ty2)];
          if (t->res_amt <= 0)
            continue;
          int rx = tx2 * TILE + TILE / 2;
          int ry = ty2 * TILE + TILE / 2;
          int d = dist_px(units[i].x, units[i].y, rx, ry);
          if (d < best_d) {
            best_d = d;
            best_t = tile_idx(tx2, ty2);
          }
        }
      if (best_t >= 0) {
        units[i].state = S_GATHER;
        units[i].tid = best_t;
        units[i].gather_t = 0;
      }
    }

    /* Attack with military */
    if (military >= 5 && !ai_attack_sent && game_time > 120) {
      ai_attack_sent = 1;
      /* Find player TC or nearest building */
      int target_b = -1;
      for (int i = 0; i < MAX_B; i++) {
        if (blds[i].active && blds[i].owner == 0) {
          if (blds[i].type == B_TC) {
            target_b = i;
            break;
          }
          if (target_b < 0)
            target_b = i;
        }
      }
      if (target_b >= 0) {
        int bx = (blds[target_b].tx + blds[target_b].sw / 2) * TILE;
        int by = (blds[target_b].ty + blds[target_b].sh / 2) * TILE;
        for (int i = 0; i < MAX_U; i++) {
          if (!units[i].active || units[i].owner != owner)
            continue;
          if (units[i].type == U_VILLAGER)
            continue;
          units[i].state = S_ATTACK;
          units[i].tid = -(target_b + 1);
        }
        show_notif("Il nemico ti attacca!");
      }
    }

    /* Reset attack flag if military drops low */
    if (military < 3)
      ai_attack_sent = 0;
  }
}

/* ---------------------------------------------------------------------- */
/* Rendering                                                                */
/* ---------------------------------------------------------------------- */
static void fill_rect(SDL_Surface *s, int x, int y, int w, int h, Uint8 r,
                      Uint8 g, Uint8 b) {
  if (w <= 0 || h <= 0)
    return;
  SDL_Rect rc = {x, y, w, h};
  SDL_FillRect(s, &rc, SDL_MapRGB(s->format, r, g, b));
}

static void draw_hp_bar(SDL_Surface *s, int x, int y, int w, int hp,
                        int hp_max) {
  int bar_h = 4;
  if (w < 8)
    w = 8;
  /* Background */
  fill_rect(s, x, y, w, bar_h, 40, 40, 40);
  /* Fill */
  int fill_w = (hp * w) / hp_max;
  if (fill_w < 0)
    fill_w = 0;
  if (fill_w > w)
    fill_w = w;
  Uint8 gr, gg, gb;
  int pct = (hp * 100) / hp_max;
  if (pct > 60) {
    gr = 40;
    gg = 200;
    gb = 40;
  } else if (pct > 30) {
    gr = 220;
    gg = 200;
    gb = 40;
  } else {
    gr = 220;
    gg = 40;
    gb = 40;
  }
  fill_rect(s, x, y, fill_w, bar_h, gr, gg, gb);
}

static void render_terrain(SDL_Surface *s) {
  int start_tx = cam_x / TILE;
  int start_ty = cam_y / TILE;
  int end_tx = start_tx + (WIN_W / TILE) + 2;
  int end_ty = start_ty + (VIEW_H / TILE) + 2;
  if (start_tx < 0)
    start_tx = 0;
  if (start_ty < 0)
    start_ty = 0;
  if (end_tx > MAP_W)
    end_tx = MAP_W;
  if (end_ty > MAP_H)
    end_ty = MAP_H;

  for (int ty = start_ty; ty < end_ty; ty++)
    for (int tx = start_tx; tx < end_tx; tx++) {
      Tile *t = &map[tile_idx(tx, ty)];
      int sx = tx * TILE - cam_x;
      int sy = ty * TILE - cam_y + TOP_H;
      Uint8 *c = TCOLOR[t->terrain];
      /* Add slight variation */
      Uint8 variation = (Uint8)((tx * 7 + ty * 13) % 16) - 8;
      fill_rect(s, sx, sy, TILE, TILE, (Uint8)(c[0] + variation),
                (Uint8)(c[1] + variation), (Uint8)(c[2] + variation));

      /* Draw resource indicators */
      if (t->terrain == T_FOREST && t->res_amt > 0) {
        /* Tree: dark green circle with brown trunk */
        int cx2 = sx + TILE / 2, cy2 = sy + TILE / 2;
        fill_rect(s, cx2 - 8, cy2 - 10, 16, 14, 20, 80, 20);
        fill_rect(s, cx2 - 2, cy2 + 2, 4, 8, 100, 60, 20);
      } else if (t->terrain == T_GOLD && t->res_amt > 0) {
        /* Gold: yellow sparkles */
        fill_rect(s, sx + 6, sy + 8, 20, 16, 220, 200, 50);
        fill_rect(s, sx + 10, sy + 12, 12, 8, 255, 240, 100);
      } else if (t->terrain == T_STONE && t->res_amt > 0) {
        /* Stone: gray rocks */
        fill_rect(s, sx + 4, sy + 6, 24, 20, 120, 120, 120);
        fill_rect(s, sx + 8, sy + 10, 16, 12, 160, 160, 160);
      } else if (t->terrain == T_BERRY && t->res_amt > 0) {
        /* Berry bush: green bush with red dots */
        fill_rect(s, sx + 4, sy + 4, 24, 24, 40, 120, 40);
        fill_rect(s, sx + 8, sy + 8, 4, 4, 200, 50, 50);
        fill_rect(s, sx + 16, sy + 10, 4, 4, 200, 50, 50);
        fill_rect(s, sx + 12, sy + 16, 4, 4, 200, 50, 50);
      } else if (t->terrain == T_FARM_PLOT) {
        /* Farm: brown soil with green rows */
        fill_rect(s, sx + 2, sy + 4, 28, 4, 100, 160, 50);
        fill_rect(s, sx + 2, sy + 14, 28, 4, 100, 160, 50);
        fill_rect(s, sx + 2, sy + 24, 28, 4, 100, 160, 50);
      }
    }
}

static void render_buildings(SDL_Surface *s) {
  for (int i = 0; i < MAX_B; i++) {
    Building *b = &blds[i];
    if (!b->active)
      continue;
    int sx = b->tx * TILE - cam_x;
    int sy = b->ty * TILE - cam_y + TOP_H;
    int sw = b->sw * TILE;
    int sh = b->sh * TILE;

    /* Cull */
    if (sx + sw < 0 || sx > WIN_W || sy + sh < TOP_H || sy > WIN_H - HUD_H)
      continue;

    Uint8 *c = BCOL[b->owner][b->type];
    if (b->state == BS_BUILD) {
      /* Darker when building */
      fill_rect(s, sx, sy, sw, sh, c[0] / 2, c[1] / 2, c[2] / 2);
      /* Progress bar */
      fill_rect(s, sx, sy + sh + 2, sw, 3, 40, 40, 40);
      fill_rect(s, sx, sy + sh + 2, (sw * b->progress) / 100, 3, 200, 200, 50);
    } else {
      fill_rect(s, sx, sy, sw, sh, c[0], c[1], c[2]);
      /* Building detail */
      if (b->type == B_TC) {
        /* Town center: door and roof line */
        fill_rect(s, sx + sw / 2 - 6, sy + sh - 12, 12, 12, 80, 60, 30);
        fill_rect(s, sx + 2, sy + 2, sw - 4, 6, (Uint8)(c[0] * 0.7),
                  (Uint8)(c[1] * 0.7), (Uint8)(c[2] * 0.7));
        /* Flag */
        fill_rect(s, sx + sw / 2, sy - 8, 2, 10, 60, 60, 60);
        fill_rect(s, sx + sw / 2 + 2, sy - 8, 8, 6, b->owner == 0 ? 60 : 220,
                  b->owner == 0 ? 120 : 60, b->owner == 0 ? 220 : 60);
      } else if (b->type == B_HOUSE) {
        fill_rect(s, sx + sw / 2 - 4, sy + sh - 10, 8, 10, 80, 60, 30);
        fill_rect(s, sx, sy, sw, 5, (Uint8)(c[0] * 0.7), (Uint8)(c[1] * 0.7),
                  (Uint8)(c[2] * 0.7));
      } else if (b->type == B_BARRACKS) {
        fill_rect(s, sx + 4, sy + sh - 10, sw - 8, 10, 60, 40, 20);
        /* Swords crossed */
        fill_rect(s, sx + sw / 2 - 1, sy + 4, 2, sh - 14, 180, 180, 180);
        fill_rect(s, sx + 4, sy + sh / 2 - 1, sw - 8, 2, 180, 180, 180);
      } else if (b->type == B_FARM) {
        /* Already rendered as terrain */
        continue;
      }
    }

    /* HP bar */
    if (b->hp < b->hp_max)
      draw_hp_bar(s, sx, sy - 7, sw, b->hp, b->hp_max);

    /* Selection highlight */
    if (i == sel_bid) {
      /* White border */
      fill_rect(s, sx - 1, sy - 1, sw + 2, 2, 255, 255, 255);
      fill_rect(s, sx - 1, sy + sh - 1, sw + 2, 2, 255, 255, 255);
      fill_rect(s, sx - 1, sy, 2, sh, 255, 255, 255);
      fill_rect(s, sx + sw - 1, sy, 2, sh, 255, 255, 255);
    }

    /* Training progress */
    if (b->train_type >= 0) {
      int pct = (b->train_timer * 100) / TRAIN_TIME[b->type][b->train_type];
      pct = 100 - pct;
      fill_rect(s, sx, sy + sh + (b->state == BS_BUILD ? 6 : 2), sw, 3, 40, 40,
                40);
      fill_rect(s, sx, sy + sh + (b->state == BS_BUILD ? 6 : 2),
                (sw * pct) / 100, 3, 100, 200, 255);
    }
  }
}

static void render_units(SDL_Surface *s) {
  for (int i = 0; i < MAX_U; i++) {
    Unit *u = &units[i];
    if (!u->active)
      continue;
    int sx = u->x - cam_x;
    int sy = u->y - cam_y + TOP_H;

    /* Cull */
    if (sx < -16 || sx > WIN_W + 16 || sy < TOP_H - 16 ||
        sy > WIN_H - HUD_H + 16)
      continue;

    Uint8 *c = UCOL[u->owner][u->type];
    if (u->flash > 0 && (u->flash % 2) == 0) {
      /* Hit flash: white */
      c = (Uint8[]){255, 255, 255};
    }

    int sz = 7;
    switch (u->type) {
    case U_VILLAGER:
      /* Circle */
      fill_rect(s, sx - sz + 2, sy - sz, (sz - 2) * 2, 2, c[0], c[1], c[2]);
      fill_rect(s, sx - sz, sy - sz + 2, sz * 2, (sz - 2) * 2, c[0], c[1],
                c[2]);
      fill_rect(s, sx - sz + 2, sy + sz - 2, (sz - 2) * 2, 2, c[0], c[1], c[2]);
      /* Carrying indicator */
      if (u->carry_amt > 0) {
        Uint8 rc[3] = {200, 180, 50}; /* gold color for carrying */
        if (u->carry_type == 0) {
          rc[0] = 120;
          rc[1] = 80;
          rc[2] = 30;
        } /* wood */
        else if (u->carry_type == 1) {
          rc[0] = 200;
          rc[1] = 60;
          rc[2] = 60;
        } /* food */
        fill_rect(s, sx - 3, sy + sz - 1, 6, 4, rc[0], rc[1], rc[2]);
      }
      break;
    case U_MILITIA:
      /* Square */
      fill_rect(s, sx - sz, sy - sz, sz * 2, sz * 2, c[0], c[1], c[2]);
      /* Sword line */
      fill_rect(s, sx + sz - 2, sy - sz - 3, 2, 6, 200, 200, 200);
      break;
    case U_SPEARMAN:
      /* Triangle (approximate with rect arrangement) */
      fill_rect(s, sx - 1, sy - sz, 3, sz * 2, c[0], c[1], c[2]);
      fill_rect(s, sx - sz + 2, sy - 2, (sz - 2) * 2, 3, c[0], c[1], c[2]);
      fill_rect(s, sx - 4, sy - 4, 8, 8, c[0], c[1], c[2]);
      /* Spear */
      fill_rect(s, sx + 4, sy - sz - 4, 1, sz + 4, 200, 200, 200);
      break;
    case U_ARCHER:
      /* Diamond */
      fill_rect(s, sx - 1, sy - sz, 3, sz * 2, c[0], c[1], c[2]);
      fill_rect(s, sx - sz, sy - 1, sz * 2, 3, c[0], c[1], c[2]);
      /* Bow */
      fill_rect(s, sx + 3, sy - 6, 2, 12, 160, 120, 60);
      break;
    }

    /* Owner indicator ring */
    if (u->owner == 0)
      fill_rect(s, sx - sz - 1, sy + sz, sz * 2 + 2, 1, 60, 60, 255);
    else
      fill_rect(s, sx - sz - 1, sy + sz, sz * 2 + 2, 1, 255, 60, 60);

    /* Selection circle */
    int is_sel = 0;
    for (int j = 0; j < sel_count; j++)
      if (sel[j] == i) {
        is_sel = 1;
        break;
      }
    if (is_sel) {
      fill_rect(s, sx - sz - 2, sy - sz - 2, 1, sz * 2 + 4, 255, 255, 255);
      fill_rect(s, sx + sz + 1, sy - sz - 2, 1, sz * 2 + 4, 255, 255, 255);
      fill_rect(s, sx - sz - 2, sy - sz - 2, sz * 2 + 4, 1, 255, 255, 255);
      fill_rect(s, sx - sz - 2, sy + sz + 1, sz * 2 + 4, 1, 255, 255, 255);
    }

    /* HP bar */
    if (u->hp < u->hp_max)
      draw_hp_bar(s, sx - sz, sy - sz - 7, sz * 2, u->hp, u->hp_max);
  }
}

static void render_hud(SDL_Surface *s) {
  /* Top bar background */
  fill_rect(s, 0, 0, WIN_W, TOP_H, 20, 20, 30);

  /* Resources */
  /* Wood bar */
  fill_rect(s, 6, 4, 8, 22, 100, 70, 30); /* wood icon */
  fill_rect(s, 18, 4, 60, 10, 30, 30, 40);
  fill_rect(s, 18, 4, (res[0].wood > 500 ? 60 : (res[0].wood * 60) / 500), 10,
            100, 70, 30);

  /* Food bar */
  fill_rect(s, 84, 4, 8, 22, 200, 50, 50); /* food icon */
  fill_rect(s, 96, 4, 60, 10, 30, 30, 40);
  fill_rect(s, 96, 4, (res[0].food > 500 ? 60 : (res[0].food * 60) / 500), 10,
            200, 50, 50);

  /* Gold bar */
  fill_rect(s, 162, 4, 8, 22, 220, 200, 50); /* gold icon */
  fill_rect(s, 174, 4, 60, 10, 30, 30, 40);
  fill_rect(s, 174, 4, (res[0].gold > 500 ? 60 : (res[0].gold * 60) / 500), 10,
            220, 200, 50);

  /* Stone bar */
  fill_rect(s, 240, 4, 8, 22, 140, 140, 140); /* stone icon */
  fill_rect(s, 252, 4, 60, 10, 30, 30, 40);
  fill_rect(s, 252, 4, (res[0].stone > 500 ? 60 : (res[0].stone * 60) / 500),
            10, 140, 140, 140);

  /* Population */
  fill_rect(s, 320, 6, 60, 18, 40, 40, 50);
  fill_rect(s, 322, 8, 56, 14, 60, 60, 80);
  fill_rect(s, 322, 8, (pop_cap[0] > 0 ? (pop[0] * 56) / pop_cap[0] : 0), 14,
            pop[0] >= pop_cap[0] ? 200 : 80, pop[0] >= pop_cap[0] ? 60 : 180,
            80);
  /* Pop numbers: filled portion = current, outline = cap */
  /* Use simple block numbers: each digit is 3x5 blocks */

  /* Resource number display using simple 3x5 pixel digits */
  /* We'll use a helper that draws a number at a given position */

  /* Game time */
  {
    int mins = game_time / 60;
    /* Simple clock display */
    fill_rect(s, WIN_W - 80, 6, 70, 18, 30, 30, 40);
  }

  /* Notification */
  if (notif_timer > 0) {
    notif_timer--;
    Uint8 alpha = notif_timer > 30 ? 255 : (Uint8)(notif_timer * 8);
    (void)alpha;
    fill_rect(s, WIN_W / 2 - 140, TOP_H + 8, 280, 20, 20, 20, 60);
    fill_rect(s, WIN_W / 2 - 140, TOP_H + 8, 280, 20, 40, 40, 80);
    /* Can't render text easily, but the notification popup covers it via
     * OS1_notify_post */
  }
}

/* Simple 3-pixel-wide digit rendering */
static void draw_digit(SDL_Surface *s, int d, int x, int y, Uint8 r, Uint8 g,
                       Uint8 b) {
  /* 3x5 pixel digits */
  static const char digits[10][5][4] = {
      {"111", "101", "101", "101", "111"}, {"010", "110", "010", "010", "111"},
      {"111", "001", "111", "100", "111"}, {"111", "001", "111", "001", "111"},
      {"101", "101", "111", "001", "001"}, {"111", "100", "111", "001", "111"},
      {"111", "100", "111", "101", "111"}, {"111", "001", "001", "001", "001"},
      {"111", "101", "111", "101", "111"}, {"111", "101", "111", "001", "111"},
  };
  for (int row = 0; row < 5; row++)
    for (int col = 0; col < 3; col++)
      if (digits[d][row][col] == '1')
        fill_rect(s, x + col * 3, y + row * 3, 3, 3, r, g, b);
}

static void draw_num(SDL_Surface *s, int val, int x, int y, Uint8 r, Uint8 g,
                     Uint8 b) {
  char buf[8];
  int n = 0;
  if (val == 0) {
    buf[n++] = '0';
  } else {
    int v = val;
    while (v > 0 && n < 7) {
      buf[n++] = '0' + (v % 10);
      v /= 10;
    }
    /* Reverse */
    for (int i = 0; i < n / 2; i++) {
      char t = buf[i];
      buf[i] = buf[n - 1 - i];
      buf[n - 1 - i] = t;
    }
  }
  for (int i = 0; i < n; i++) {
    draw_digit(s, buf[i] - '0', x + i * 12, y, r, g, b);
  }
}

static void render_hud_detailed(SDL_Surface *s) {
  /* Override the simple HUD with detailed one including numbers */
  render_hud(s);

  /* Draw resource numbers on top of bars */
  draw_num(s, res[0].wood, 20, 2, 230, 210, 150);
  draw_num(s, res[0].food, 98, 2, 230, 210, 150);
  draw_num(s, res[0].gold, 176, 2, 230, 210, 150);
  draw_num(s, res[0].stone, 254, 2, 230, 210, 150);

  /* Population text */
  draw_num(s, pop[0], 324, 2, 220, 220, 240);
  /* "/" separator */
  fill_rect(s, 348, 4, 2, 10, 150, 150, 170);
  draw_num(s, pop_cap[0], 354, 2, 150, 150, 170);

  /* Game time */
  {
    int mins = game_time / 60;
    int secs = game_time % 60;
    draw_num(s, mins, WIN_W - 70, 2, 200, 200, 220);
    fill_rect(s, WIN_W - 46, 4, 2, 10, 150, 150, 170);
    if (secs < 10)
      draw_num(s, 0, WIN_W - 40, 2, 200, 200, 220);
    draw_num(s, secs, WIN_W - 28, 2, 200, 200, 220);
  }

  /* Bottom HUD */
  int hud_y = WIN_H - HUD_H;
  fill_rect(s, 0, hud_y, WIN_W, HUD_H, 25, 25, 35);
  fill_rect(s, 0, hud_y, WIN_W, 1, 60, 60, 80);

  if (sel_count > 0 && sel[0] >= 0) {
    Unit *u = &units[sel[0]];
    /* Unit info */
    fill_rect(s, 8, hud_y + 6, 200, 40, 35, 35, 50);
    Uint8 *c = UCOL[u->owner][u->type];
    fill_rect(s, 14, hud_y + 12, 28, 28, c[0], c[1], c[2]);
    draw_hp_bar(s, 50, hud_y + 12, 150, u->hp, u->hp_max);

    /* State text via colored indicators */
    if (u->state == S_GATHER) {
      fill_rect(s, 50, hud_y + 24, 50, 6, 100, 200, 100);
    } else if (u->state == S_ATTACK) {
      fill_rect(s, 50, hud_y + 24, 50, 6, 200, 80, 80);
    } else if (u->state == S_BUILD) {
      fill_rect(s, 50, hud_y + 24, 50, 6, 200, 200, 80);
    } else if (u->state == S_MOVE) {
      fill_rect(s, 50, hud_y + 24, 50, 6, 80, 80, 200);
    }

    if (sel_count > 1) {
      draw_num(s, sel_count, 50, hud_y + 34, 220, 220, 240);
      fill_rect(s, 62, hud_y + 38, 4, 4, 220, 220,
                240); /* "selected" indicator */
    }

    /* Show carry amount */
    if (u->carry_amt > 0) {
      draw_num(s, u->carry_amt, 110, hud_y + 34, 220, 200, 100);
    }

  } else if (sel_bid >= 0) {
    Building *b = &blds[sel_bid];
    fill_rect(s, 8, hud_y + 6, 200, 40, 35, 35, 50);
    Uint8 *c = BCOL[b->owner][b->type];
    fill_rect(s, 14, hud_y + 12, 28, 28, c[0], c[1], c[2]);
    draw_hp_bar(s, 50, hud_y + 12, 150, b->hp, b->hp_max);
    if (b->state == BS_BUILD) {
      fill_rect(s, 50, hud_y + 24, 80, 6, 200, 200, 80);
      fill_rect(s, 50, hud_y + 24, (80 * b->progress) / 100, 6, 240, 240, 100);
    }
    if (b->train_type >= 0) {
      fill_rect(s, 50, hud_y + 34, 80, 6, 80, 160, 240);
      int pct =
          100 - (b->train_timer * 100) / TRAIN_TIME[b->type][b->train_type];
      fill_rect(s, 50, hud_y + 34, (80 * pct) / 100, 6, 120, 200, 255);
    }

    /* Hotkey hints */
    if (b->type == B_TC && b->owner == 0 && b->state == BS_DONE &&
        b->train_type < 0) {
      fill_rect(s, 220, hud_y + 8, 100, 24, 40, 60, 40);
      fill_rect(s, 224, hud_y + 14, 16, 12, 80, 140, 80);
      draw_num(s, 50, 244, hud_y + 10, 200, 100, 100); /* food cost */
    }
    if (b->type == B_BARRACKS && b->owner == 0 && b->state == BS_DONE &&
        b->train_type < 0) {
      fill_rect(s, 220, hud_y + 8, 280, 24, 40, 50, 60);
      /* M - Militia */
      fill_rect(s, 224, hud_y + 10, 14, 18, 50, 100, 200);
      fill_rect(s, 242, hud_y + 14, 40, 10, 30, 30, 40);
      /* S - Spearman */
      fill_rect(s, 288, hud_y + 10, 14, 18, 40, 90, 190);
      fill_rect(s, 306, hud_y + 14, 40, 10, 30, 30, 40);
      /* A - Archer */
      fill_rect(s, 352, hud_y + 10, 14, 18, 70, 140, 230);
      fill_rect(s, 370, hud_y + 14, 40, 10, 30, 30, 40);
    }
  }

  /* Building placement buttons (right side) */
  fill_rect(s, WIN_W - 180, hud_y + 8, 170, 30, 35, 35, 50);
  /* H = House */
  fill_rect(s, WIN_W - 174, hud_y + 12, 20, 20, 160, 130, 80);
  /* B = Barracks */
  fill_rect(s, WIN_W - 148, hud_y + 12, 20, 20, 130, 100, 70);
  /* F = Farm */
  fill_rect(s, WIN_W - 122, hud_y + 12, 20, 20, 120, 80, 40);

  /* Place mode indicator */
  if (place_mode != PM_NONE) {
    fill_rect(s, WIN_W / 2 - 60, hud_y + 4, 120, 16, 200, 100, 40);
  }

  /* Controls help */
  fill_rect(s, WIN_W - 180, hud_y + 42, 170, 12, 35, 35, 50);
  fill_rect(s, WIN_W - 174, hud_y + 44, 164, 8, 50, 50, 65);
}

static void render_minimap(SDL_Surface *s) {
  int mx0 = WIN_W - MINI_W - 6;
  int my0 = TOP_H + 6;
  /* Background */
  fill_rect(s, mx0 - 1, my0 - 1, MINI_W + 2, MINI_H + 2, 20, 20, 30);

  int sx = MINI_W / MAP_W;
  int sy = MINI_H / MAP_H;

  /* Terrain (every other tile for performance) */
  for (int ty = 0; ty < MAP_H; ty += 1)
    for (int tx = 0; tx < MAP_W; tx += 1) {
      Tile *t = &map[tile_idx(tx, ty)];
      Uint8 *c = TCOLOR[t->terrain];
      fill_rect(s, mx0 + tx * sx, my0 + ty * sy, sx, sy, c[0], c[1], c[2]);
    }

  /* Buildings */
  for (int i = 0; i < MAX_B; i++) {
    if (!blds[i].active)
      continue;
    Uint8 r2, g2, b2;
    if (blds[i].owner == 0) {
      r2 = 180;
      g2 = 180;
      b2 = 220;
    } else {
      r2 = 220;
      g2 = 80;
      b2 = 80;
    }
    fill_rect(s, mx0 + blds[i].tx * sx, my0 + blds[i].ty * sy, blds[i].sw * sx,
              blds[i].sh * sy, r2, g2, b2);
  }

  /* Units */
  for (int i = 0; i < MAX_U; i++) {
    if (!units[i].active)
      continue;
    int ux = units[i].x / TILE;
    int uy = units[i].y / TILE;
    if (units[i].owner == 0)
      fill_rect(s, mx0 + ux * sx, my0 + uy * sy, 2, 2, 240, 240, 255);
    else
      fill_rect(s, mx0 + ux * sx, my0 + uy * sy, 2, 2, 255, 80, 80);
  }

  /* Camera viewport rectangle */
  int vx = (cam_x * MINI_W) / (MAP_W * TILE);
  int vy = (cam_y * MINI_H) / (MAP_H * TILE);
  int vw = (WIN_W * MINI_W) / (MAP_W * TILE);
  int vh = (VIEW_H * MINI_H) / (MAP_H * TILE);
  fill_rect(s, mx0 + vx, my0 + vy, vw, vh, 0, 0, 0);
  /* White border on viewport */
  fill_rect(s, mx0 + vx, my0 + vy, vw, 1, 255, 255, 255);
  fill_rect(s, mx0 + vx, my0 + vy + vh - 1, vw, 1, 255, 255, 255);
  fill_rect(s, mx0 + vx, my0 + vy, 1, vh, 255, 255, 255);
  fill_rect(s, mx0 + vx + vw - 1, my0 + vy, 1, vh, 255, 255, 255);
}

static void render_selection_box(SDL_Surface *s) {
  if (!dragging)
    return;
  int x1 = drag_sx, y1 = drag_sy;
  int x2 = mx, y2 = my;
  if (y1 > TOP_H && y2 > TOP_H) {
    if (x1 > x2) {
      int t = x1;
      x1 = x2;
      x2 = t;
    }
    if (y1 > y2) {
      int t = y1;
      y1 = y2;
      y2 = t;
    }
    /* Clamp to view area */
    if (y1 < TOP_H)
      y1 = TOP_H;
    if (y2 > WIN_H - HUD_H)
      y2 = WIN_H - HUD_H;
    fill_rect(s, x1, y1, x2 - x1, 1, 255, 255, 255);
    fill_rect(s, x1, y2 - 1, x2 - x1, 1, 255, 255, 255);
    fill_rect(s, x1, y1, 1, y2 - y1, 255, 255, 255);
    fill_rect(s, x2 - 1, y1, 1, y2 - y1, 255, 255, 255);
  }
}

static void render_placement_ghost(SDL_Surface *s) {
  if (place_mode == PM_NONE)
    return;
  int btype = -1;
  if (place_mode == PM_HOUSE)
    btype = B_HOUSE;
  else if (place_mode == PM_BARRACKS)
    btype = B_BARRACKS;
  else if (place_mode == PM_FARM)
    btype = B_FARM;
  if (btype < 0)
    return;

  int tx = mwx / TILE, ty = (mwy - TOP_H + cam_y) / TILE;
  int sw = BSIZE_W[btype], sh = BSIZE_H[btype];
  int sx = tx * TILE - cam_x;
  int sy = ty * TILE - cam_y + TOP_H;

  int ok = can_place(btype, 0, tx, ty);
  /* Checkerboard ghost */
  for (int dy2 = 0; dy2 < sh; dy2++)
    for (int dx2 = 0; dx2 < sw; dx2++) {
      int chk = (dx2 + dy2) % 2;
      if (ok)
        fill_rect(s, sx + dx2 * TILE + chk * TILE / 2, sy + dy2 * TILE,
                  TILE / 2, TILE, chk ? 100 : 150, chk ? 255 : 200,
                  chk ? 100 : 150);
      else
        fill_rect(s, sx + dx2 * TILE + chk * TILE / 2, sy + dy2 * TILE,
                  TILE / 2, TILE, chk ? 255 : 180, chk ? 80 : 50,
                  chk ? 80 : 50);
    }
  /* Border */
  Uint8 bc2 = ok ? 150 : 255;
  Uint8 bg2 = ok ? 255 : 60;
  Uint8 bb2 = ok ? 150 : 60;
  fill_rect(s, sx, sy, sw * TILE, 2, bc2, bg2, bb2);
  fill_rect(s, sx, sy + sh * TILE - 2, sw * TILE, 2, bc2, bg2, bb2);
  fill_rect(s, sx, sy, 2, sh * TILE, bc2, bg2, bb2);
  fill_rect(s, sx + sw * TILE - 2, sy, 2, sh * TILE, bc2, bg2, bb2);
}

static void render_overlay(SDL_Surface *s) {
  if (game_state == GS_START) {
    /* Darken */
    for (int y = 0; y < WIN_H; y += 4)
      fill_rect(s, 0, y, WIN_W, 2, 0, 0, 0);
    /* Title */
    fill_rect(s, WIN_W / 2 - 120, WIN_H / 2 - 60, 240, 40, 40, 30, 20);
    /* "NexEmpire" - render as colored blocks */
    fill_rect(s, WIN_W / 2 - 100, WIN_H / 2 - 50, 200, 20, 200, 180, 100);
    fill_rect(s, WIN_W / 2 - 80, WIN_H / 2 + 0, 160, 20, 60, 60, 80);
    /* Click to start */
    if ((frame / 30) % 2 == 0)
      fill_rect(s, WIN_W / 2 - 80, WIN_H / 2 + 40, 160, 16, 100, 180, 100);
  }
  if (game_state == GS_WIN) {
    for (int y = 0; y < WIN_H; y += 4)
      fill_rect(s, 0, y, WIN_W, 2, 0, 0, 0);
    fill_rect(s, WIN_W / 2 - 80, WIN_H / 2 - 30, 160, 30, 40, 80, 40);
    fill_rect(s, WIN_W / 2 - 60, WIN_H / 2 - 24, 120, 18, 100, 255, 100);
    fill_rect(s, WIN_W / 2 - 80, WIN_H / 2 + 10, 160, 20, 60, 60, 80);
  }
  if (game_state == GS_LOSE) {
    for (int y = 0; y < WIN_H; y += 4)
      fill_rect(s, 0, y, WIN_W, 2, 0, 0, 0);
    fill_rect(s, WIN_W / 2 - 80, WIN_H / 2 - 30, 160, 30, 80, 40, 40);
    fill_rect(s, WIN_W / 2 - 60, WIN_H / 2 - 24, 120, 18, 255, 100, 100);
    fill_rect(s, WIN_W / 2 - 80, WIN_H / 2 + 10, 160, 20, 60, 60, 80);
  }
}

/* ---------------------------------------------------------------------- */
/* Input Handling                                                           */
/* ---------------------------------------------------------------------- */
static void do_select(int wx, int wy) {
  /* Convert window to world */
  int wwx = wx + cam_x;
  int wwy = wy - TOP_H + cam_y;

  sel_count = 0;
  sel_bid = -1;

  /* Try to select a player unit */
  int ui = unit_at(wwx, wwy, 0);
  if (ui >= 0) {
    sel[0] = ui;
    sel_count = 1;
    return;
  }

  /* Try to select a player building */
  int tx = wwx / TILE, ty = wwy / TILE;
  int bi = building_at_tile(tx, ty);
  if (bi >= 0 && blds[bi].active && blds[bi].owner == 0) {
    sel_bid = bi;
    return;
  }
}

static void do_box_select(int x1, int y1, int x2, int y2) {
  /* Convert to world coords */
  int wx1 = x1 + cam_x, wy1 = y1 - TOP_H + cam_y;
  int wx2 = x2 + cam_x, wy2 = y2 - TOP_H + cam_y;
  if (wx1 > wx2) {
    int t = wx1;
    wx1 = wx2;
    wx2 = t;
  }
  if (wy1 > wy2) {
    int t = wy1;
    wy1 = wy2;
    wy2 = t;
  }

  sel_count = 0;
  sel_bid = -1;

  for (int i = 0; i < MAX_U && sel_count < MAX_SEL; i++) {
    if (!units[i].active || units[i].owner != 0)
      continue;
    if (units[i].x >= wx1 && units[i].x <= wx2 && units[i].y >= wy1 &&
        units[i].y <= wy2) {
      sel[sel_count++] = i;
    }
  }
}

static void do_right_click(int wx, int wy) {
  if (sel_count == 0)
    return;

  int wwx = wx + cam_x;
  int wwy = wy - TOP_H + cam_y;

  /* Check if clicking on enemy unit */
  int eui = unit_at(wwx, wwy, 1);
  if (eui >= 0) {
    for (int i = 0; i < sel_count; i++) {
      Unit *u = &units[sel[i]];
      u->state = S_ATTACK;
      u->tid = eui;
    }
    return;
  }

  /* Check if clicking on enemy building */
  int tx = wwx / TILE, ty = wwy / TILE;
  int ebi = building_at_tile(tx, ty);
  if (ebi >= 0 && blds[ebi].active && blds[ebi].owner == 1) {
    for (int i = 0; i < sel_count; i++) {
      Unit *u = &units[sel[i]];
      u->state = S_ATTACK;
      u->tid = -(ebi + 1);
    }
    return;
  }

  /* Check if clicking on own building under construction (to build) */
  if (ebi >= 0 && blds[ebi].active && blds[ebi].owner == 0 &&
      blds[ebi].state == BS_BUILD) {
    for (int i = 0; i < sel_count; i++) {
      if (units[sel[i]].type == U_VILLAGER) {
        units[sel[i]].state = S_BUILD;
        units[sel[i]].tid = ebi;
        blds[ebi].builders++;
      }
    }
    return;
  }

  /* Check if clicking on a resource tile (for villagers) */
  if (tx >= 0 && tx < MAP_W && ty >= 0 && ty < MAP_H) {
    Tile *t = &map[tile_idx(tx, ty)];
    if (t->res_amt > 0) {
      for (int i = 0; i < sel_count; i++) {
        if (units[sel[i]].type == U_VILLAGER) {
          units[sel[i]].state = S_GATHER;
          units[sel[i]].tid = tile_idx(tx, ty);
          units[sel[i]].gather_t = 0;
          units[sel[i]].carry_amt = 0;
          units[sel[i]].carry_type = -1;
        }
      }
      return;
    }
  }

  /* Move command */
  for (int i = 0; i < sel_count; i++) {
    Unit *u = &units[sel[i]];
    /* Spread units slightly */
    int offset_x = (i % 4 - 1) * 12;
    int offset_y = (i / 4 - 1) * 12;
    u->state = S_MOVE;
    u->tx = wwx + offset_x;
    u->ty = wwy + offset_y;
    u->tid = -1;
  }
}

static void do_place_building(int wx, int wy) {
  int btype = -1;
  if (place_mode == PM_HOUSE)
    btype = B_HOUSE;
  else if (place_mode == PM_BARRACKS)
    btype = B_BARRACKS;
  else if (place_mode == PM_FARM)
    btype = B_FARM;
  if (btype < 0)
    return;

  int wwx = wx + cam_x;
  int wwy = wy - TOP_H + cam_y;
  int tx = wwx / TILE, ty = wwy / TILE;

  if (!can_place(btype, 0, tx, ty)) {
    show_notif("Impossibile costruire qui!");
    return;
  }

  /* Check resources */
  if (res[0].wood < BCOST_WOOD[btype]) {
    show_notif("Non abbastanza legno!");
    return;
  }

  res[0].wood -= BCOST_WOOD[btype];
  int bi = spawn_building(btype, 0, tx, ty);
  if (bi >= 0) {
    /* Auto-assign selected villagers to build */
    for (int i = 0; i < sel_count; i++) {
      if (units[sel[i]].type == U_VILLAGER) {
        units[sel[i]].state = S_BUILD;
        units[sel[i]].tid = bi;
        blds[bi].builders++;
      }
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "%s in costruzione...", BNAME[btype]);
    show_notif(msg);
  }

  place_mode = PM_NONE;
}

static void train_unit(int utype) {
  if (sel_bid < 0)
    return;
  Building *b = &blds[sel_bid];
  if (!b->active || b->owner != 0 || b->state != BS_DONE)
    return;
  if (b->train_type >= 0)
    return; /* already training */
  if (pop[0] >= pop_cap[0]) {
    show_notif("Popolazione al massimo!");
    return;
  }

  int food_cost = TRAIN_FOOD[b->type][utype];
  int wood_cost = TRAIN_WOOD[b->type][utype];
  int gold_cost = TRAIN_GOLD[b->type][utype];

  if (res[0].food < food_cost || res[0].wood < wood_cost ||
      res[0].gold < gold_cost) {
    show_notif("Risorse insufficienti!");
    return;
  }

  res[0].food -= food_cost;
  res[0].wood -= wood_cost;
  res[0].gold -= gold_cost;
  b->train_type = utype;
  b->train_timer = TRAIN_TIME[b->type][utype];

  char msg[64];
  snprintf(msg, sizeof(msg), "Addestramento %s...", UNAME[utype]);
  show_notif(msg);
}

/* ---------------------------------------------------------------------- */
/* Main                                                                     */
/* ---------------------------------------------------------------------- */
int main(void) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    OS1_notify_error("NexEmpire", "SDL_Init fallito");
    exit(1);
  }
  printf("[NexEmpire] SDL inizializzato\n");

  SDL_Window *window = SDL_CreateWindow("NexEmpire", 40, 20, WIN_W, WIN_H, 0);
  if (!window) {
    OS1_notify_error("NexEmpire", "Finestra non creata");
    SDL_Quit();
    exit(1);
  }

  SDL_Surface *surface = SDL_GetWindowSurface(window);
  if (!surface) {
    OS1_notify_error("NexEmpire", "Superficie non ottenuta");
    SDL_DestroyWindow(window);
    SDL_Quit();
    exit(1);
  }

  int win_w = surface->w, win_h = surface->h;

  /* Load stats from registry */
  char reg_buf[32];
  if (OS1_registry_get("nexempire_wins", reg_buf, sizeof(reg_buf)) > 0)
    wins = atoi(reg_buf);
  if (OS1_registry_get("nexempire_losses", reg_buf, sizeof(reg_buf)) > 0)
    losses = atoi(reg_buf);

  init_game();
  OS1_notify_post("NexEmpire", "Clicca per iniziare!");

  int running = 1;
  const Uint8 *keys = SDL_GetKeyboardState(NULL);

  Uint64 last_sec = SDL_GetTicks64();
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
          running = 0;
          break;
        }
        win_w = surface->w;
        win_h = surface->h;
      }

      if (game_state == GS_START) {
        if (event.type == SDL_MOUSEBUTTONDOWN &&
            event.button.button == SDL_BUTTON_LEFT) {
          game_state = GS_PLAY;
        }
        continue;
      }

      if (game_state == GS_WIN || game_state == GS_LOSE) {
        if (event.type == SDL_KEYDOWN) {
          if (event.key.keysym.scancode == SDL_SCANCODE_R) {
            /* Save stats */
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", wins);
            OS1_registry_set("nexempire_wins", buf);
            snprintf(buf, sizeof(buf), "%d", losses);
            OS1_registry_set("nexempire_losses", buf);
            init_game();
            game_state = GS_PLAY;
          }
          if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
            running = 0;
        }
        continue;
      }

      /* GS_PLAY input handling */
      if (event.type == SDL_MOUSEMOTION) {
        mx = event.motion.x;
        my = event.motion.y;
        mwx = mx + cam_x;
        mwy = my - TOP_H + cam_y;

        if (dragging) {
          /* Continue dragging */
        }
      }

      if (event.type == SDL_MOUSEBUTTONDOWN) {
        if (event.button.button == SDL_BUTTON_LEFT) {
          if (my > TOP_H && my < WIN_H - HUD_H) {
            /* Check minimap click */
            int mini_x0 = WIN_W - MINI_W - 6;
            int mini_y0 = TOP_H + 6;
            if (mx >= mini_x0 && mx <= mini_x0 + MINI_W && my >= mini_y0 &&
                my <= mini_y0 + MINI_H) {
              /* Move camera to minimap click */
              cam_x = ((mx - mini_x0) * MAP_W * TILE) / MINI_W;
              cam_y = ((my - mini_y0) * MAP_H * TILE) / MINI_H;
              clamp_cam();
            } else if (place_mode != PM_NONE) {
              do_place_building(mx, my);
            } else {
              dragging = 1;
              drag_sx = mx;
              drag_sy = my;
            }
          }
        } else if (event.button.button == SDL_BUTTON_RIGHT) {
          if (place_mode != PM_NONE) {
            place_mode = PM_NONE;
          } else if (my > TOP_H && my < WIN_H - HUD_H) {
            do_right_click(mx, my);
          }
        }
      }

      if (event.type == SDL_MOUSEBUTTONUP) {
        if (event.button.button == SDL_BUTTON_LEFT && dragging) {
          dragging = 0;
          int dx = mx - drag_sx, dy = my - drag_sy;
          if (dx * dx + dy * dy < 25) {
            /* Single click */
            do_select(mx, my);
          } else {
            /* Box select */
            do_box_select(drag_sx, drag_sy, mx, my);
          }
        }
      }

      if (event.type == SDL_KEYDOWN) {
        SDL_Scancode sc = event.key.keysym.scancode;
        if (sc == SDL_SCANCODE_ESCAPE) {
          if (place_mode != PM_NONE)
            place_mode = PM_NONE;
          else
            running = 0;
        }
        if (sc == SDL_SCANCODE_H)
          place_mode = PM_HOUSE;
        if (sc == SDL_SCANCODE_B)
          place_mode = PM_BARRACKS;
        if (sc == SDL_SCANCODE_F)
          place_mode = PM_FARM;
        if (sc == SDL_SCANCODE_V)
          train_unit(U_VILLAGER);
        if (sc == SDL_SCANCODE_M)
          train_unit(U_MILITIA);
        if (sc == SDL_SCANCODE_P)
          train_unit(U_SPEARMAN);
        if (sc == SDL_SCANCODE_A)
          train_unit(U_ARCHER);
        if (sc == SDL_SCANCODE_DELETE || sc == SDL_SCANCODE_BACKSPACE) {
          if (sel_bid >= 0 && blds[sel_bid].active &&
              blds[sel_bid].owner == 0) {
            /* Refund 50% */
            res[0].wood += BCOST_WOOD[blds[sel_bid].type] / 2;
            remove_building(sel_bid);
            sel_bid = -1;
            show_notif("Edificio eliminato");
          }
        }
      }
    }

    /* Camera scrolling */
    if (game_state == GS_PLAY) {
      if (keys[SDL_SCANCODE_LEFT])
        cam_x -= CAM_SPD;
      if (keys[SDL_SCANCODE_RIGHT])
        cam_x += CAM_SPD;
      if (keys[SDL_SCANCODE_UP])
        cam_y -= CAM_SPD;
      if (keys[SDL_SCANCODE_DOWN])
        cam_y += CAM_SPD;

      /* Edge scrolling */
      if (mx < EDGE_Z)
        cam_x -= CAM_SPD;
      if (mx > WIN_W - EDGE_Z)
        cam_x += CAM_SPD;
      if (my > TOP_H && my < TOP_H + EDGE_Z)
        cam_y -= CAM_SPD;
      if (my > WIN_H - HUD_H - EDGE_Z && my < WIN_H - HUD_H)
        cam_y += CAM_SPD;

      clamp_cam();

      /* Update world */
      frame++;
      if (frame % 60 == 0)
        game_time++;

      for (int i = 0; i < MAX_U; i++)
        update_unit(i);
      update_buildings();
      ai_update();

      /* Check lose condition */
      if (game_state == GS_PLAY) {
        int has_units = 0, has_tc = 0;
        for (int i = 0; i < MAX_U; i++)
          if (units[i].active && units[i].owner == 0) {
            has_units = 1;
            break;
          }
        for (int i = 0; i < MAX_B; i++)
          if (blds[i].active && blds[i].type == B_TC && blds[i].owner == 0) {
            has_tc = 1;
            break;
          }
        if (!has_tc && !has_units) {
          game_state = GS_LOSE;
          losses++;
          show_notif("Sconfitta!");
        }
      }
    }

    /* Render */
    if (surface) {
      /* Clear */
      fill_rect(surface, 0, 0, win_w, win_h, 0, 0, 0);

      if (game_state == GS_START) {
        /* Render a preview of the map */
        render_terrain(surface);
        render_buildings(surface);
        render_units(surface);
      } else {
        render_terrain(surface);
        render_buildings(surface);
        render_units(surface);
        render_selection_box(surface);
        render_placement_ghost(surface);
        render_hud_detailed(surface);
        render_minimap(surface);
      }
      render_overlay(surface);

      if (SDL_UpdateWindowSurface(window) != 0) {
        surface = SDL_GetWindowSurface(window);
        if (!surface) {
          running = 0;
        }
      }
    }

    /* FPS counter */
    fps_frames++;
    Uint64 now = SDL_GetTicks64();
    if (now - last_sec >= 1000) {
      last_sec = now;
      fps_frames = 0;
    }

    SDL_Delay(16); /* ~60 FPS */
  }

  /* Save stats on exit */
  {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", wins);
    OS1_registry_set("nexempire_wins", buf);
    snprintf(buf, sizeof(buf), "%d", losses);
    OS1_registry_set("nexempire_losses", buf);
  }

  SDL_DestroyWindow(window);
  SDL_Quit();
  printf("[NexEmpire] chiusura\n");
  exit(0);
  return 0;
}