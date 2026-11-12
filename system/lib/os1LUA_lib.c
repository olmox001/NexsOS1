#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <os1.h>
#include <stdlib.h>
#include <string.h>
#include "os1LUA_lib.h"

/* Helper to convert return values to (status, error_msg) or just status */
static int push_result(lua_State *L, long res) {
    if (res < 0) {
        lua_pushnil(L);
        lua_pushinteger(L, res);
        return 2;
    }
    lua_pushinteger(L, res);
    return 1;
}

/* 1. os1.print(str) */
static int l_print(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    print(s);
    return 0;
}

/* 2. os1.sleep(ms) */
static int l_sleep(lua_State *L) {
    int ms = (int)luaL_checkinteger(L, 1);
    OS1_sleep(ms);
    return 0;
}

/* 3. os1.get_time() */
static int l_get_time(lua_State *L) {
    lua_pushinteger(L, get_time());
    return 1;
}

/* 4. os1.get_pid() */
static int l_get_pid(lua_State *L) {
    lua_pushinteger(L, get_pid());
    return 1;
}

/* 5. os1.yield() */
static int l_yield(lua_State *L) {
    yield();
    return 0;
}

/* 6. os1.exit(status) */
static int l_exit(lua_State *L) {
    int status = (int)luaL_optinteger(L, 1, 0);
    exit(status);
    return 0;
}

/* 7. os1.spawn(path, [arg1, arg2, ...]) */
static int l_spawn(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int n = lua_gettop(L);
    if (n <= 1) {
        int pid = spawn(path);
        return push_result(L, pid);
    }
    
    int argc = n;
    char **argv = malloc((argc + 1) * sizeof(char *));
    if (!argv) {
        return luaL_error(L, "out of memory");
    }
    argv[0] = (char *)path;
    for (int i = 2; i <= n; i++) {
        argv[i - 1] = (char *)luaL_checkstring(L, i);
    }
    argv[argc] = NULL;
    
    int pid = spawn_args(path, argc, argv);
    free(argv);
    return push_result(L, pid);
}

/* 8. os1.kill(pid) */
static int l_kill(lua_State *L) {
    int pid = (int)luaL_checkinteger(L, 1);
    int r = kill_process(pid);
    return push_result(L, r);
}

/* 9. os1.wait(pid) */
static int l_wait(lua_State *L) {
    int pid = (int)luaL_checkinteger(L, 1);
    int r = wait(pid);
    return push_result(L, r);
}

/* 10. os1.create_window(x, y, w, h, title) */
static int l_create_window(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    const char *title = luaL_checkstring(L, 5);
    int win_id = create_window(x, y, w, h, title);
    return push_result(L, win_id);
}

/* 11. os1.destroy_window(win_id) */
static int l_destroy_window(lua_State *L) {
    int win_id = (int)luaL_checkinteger(L, 1);
    destroy_window(win_id);
    return 0;
}

/* 12. os1.window_draw(win_id, x, y, w, h, color) */
static int l_window_draw(lua_State *L) {
    int win_id = (int)luaL_checkinteger(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    int w = (int)luaL_checkinteger(L, 4);
    int h = (int)luaL_checkinteger(L, 5);
    unsigned int color = (unsigned int)luaL_checkinteger(L, 6);
    window_draw(win_id, x, y, w, h, color);
    return 0;
}

/* 13. os1.window_write(win_id, text) */
static int l_window_write(lua_State *L) {
    int win_id = (int)luaL_checkinteger(L, 1);
    const char *buf = luaL_checkstring(L, 2);
    window_write(win_id, buf, strlen(buf));
    return 0;
}

/* 14. os1.compositor_render() */
static int l_compositor_render(lua_State *L) {
    compositor_render();
    return 0;
}

/* 15. os1.set_window_flags(win_id, flags) */
static int l_set_window_flags(lua_State *L) {
    int win_id = (int)luaL_checkinteger(L, 1);
    int flags = (int)luaL_checkinteger(L, 2);
    set_window_flags(win_id, flags);
    return 0;
}

/* 16. os1.registry_get(key) */
static int l_registry_get(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    char buf[512];
    int r = OS1_registry_get(key, buf, sizeof(buf));
    if (r >= 0) {
        lua_pushstring(L, buf);
        return 1;
    }
    lua_pushnil(L);
    lua_pushinteger(L, r);
    return 2;
}

/* 17. os1.registry_set(key, value) */
static int l_registry_set(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    const char *val = luaL_checkstring(L, 2);
    int r = OS1_registry_set(key, val);
    return push_result(L, r);
}

/* 18. os1.registry_del(key) */
static int l_registry_del(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    int r = OS1_registry_del(key);
    return push_result(L, r);
}

/* 19. os1.fs_read(path, size, [offset]) */
static int l_fs_read(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int size = (int)luaL_checkinteger(L, 2);
    int offset = (int)luaL_optinteger(L, 3, 0);
    if (size <= 0) {
        lua_pushstring(L, "");
        return 1;
    }
    
    char *buf = malloc(size);
    if (!buf) {
        return luaL_error(L, "out of memory");
    }
    
    int r = file_read(path, buf, size, offset);
    if (r >= 0) {
        lua_pushlstring(L, buf, r);
        free(buf);
        return 1;
    }
    
    free(buf);
    lua_pushnil(L);
    lua_pushinteger(L, r);
    return 2;
}

/* 20. os1.fs_write(path, data, [offset]) */
static int l_fs_write(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    int offset = (int)luaL_optinteger(L, 3, 0);
    
    int r = file_write(path, data, (int)len, offset);
    return push_result(L, r);
}

/* 21. os1.list_dir(path) */
static int l_list_dir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    char *buf = malloc(8198);
    if (!buf) {
        return luaL_error(L, "out of memory");
    }
    
    int r = list_dir(path, buf, 8192);
    if (r >= 0) {
        lua_newtable(L);
        int idx = 1;
        const char *p = buf;
        while (*p) {
            const char *end = p;
            while (*end && *end != '\n') end++;
            lua_pushlstring(L, p, end - p);
            lua_rawseti(L, -2, idx++);
            if (*end == '\n') p = end + 1;
            else p = end;
        }
        free(buf);
        return 1;
    }
    
    free(buf);
    lua_pushnil(L);
    lua_pushinteger(L, r);
    return 2;
}

/* 22. os1.chdir(path) */
static int l_chdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int r = chdir(path);
    return push_result(L, r);
}

/* 23. os1.getcwd() */
static int l_getcwd(lua_State *L) {
    char buf[256];
    int r = getcwd(buf, sizeof(buf));
    if (r >= 0) {
        lua_pushstring(L, buf);
        return 1;
    }
    lua_pushnil(L);
    lua_pushinteger(L, r);
    return 2;
}

/* 24. os1.unlink(path) */
static int l_unlink(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int r = OS1_fs_unlink(path);
    return push_result(L, r);
}

/* 25. os1.notify(title, message) */
static int l_notify(lua_State *L) {
    const char *title = luaL_checkstring(L, 1);
    const char *msg = luaL_checkstring(L, 2);
    int r = OS1_notify_post(title, msg);
    return push_result(L, r);
}

/* 26. os1.sysstats() */
static int l_sysstats(lua_State *L) {
    struct os1_sysstats stats;
    long r = OS1_sys_stats(&stats);
    if (r >= 0) {
        lua_newtable(L);
        lua_pushinteger(L, stats.version);
        lua_setfield(L, -2, "version");
        lua_pushinteger(L, stats.uptime_ns);
        lua_setfield(L, -2, "uptime_ns");
        
        /* PMM */
        lua_pushinteger(L, stats.pmm_total_pages);
        lua_setfield(L, -2, "pmm_total_pages");
        lua_pushinteger(L, stats.pmm_free_pages);
        lua_setfield(L, -2, "pmm_free_pages");
        lua_pushinteger(L, stats.pmm_largest_contig_run);
        lua_setfield(L, -2, "pmm_largest_contig_run");
        lua_pushinteger(L, stats.pmm_free_run_count);
        lua_setfield(L, -2, "pmm_free_run_count");
        
        /* Kmalloc */
        lua_pushinteger(L, stats.km_heap_total_bytes);
        lua_setfield(L, -2, "km_heap_total_bytes");
        lua_pushinteger(L, stats.km_bytes_in_use);
        lua_setfield(L, -2, "km_bytes_in_use");
        lua_pushinteger(L, stats.km_high_water_bytes);
        lua_setfield(L, -2, "km_high_water_bytes");
        
        /* Scheduler */
        lua_pushinteger(L, stats.sched_ctx_switches);
        lua_setfield(L, -2, "sched_ctx_switches");
        lua_pushinteger(L, stats.sched_nproc);
        lua_setfield(L, -2, "sched_nproc");
        lua_pushinteger(L, stats.sched_zombie_count);
        lua_setfield(L, -2, "sched_zombie_count");
        lua_pushinteger(L, stats.sched_ncpu);
        lua_setfield(L, -2, "sched_ncpu");
        lua_pushinteger(L, stats.sched_runnable);
        lua_setfield(L, -2, "sched_runnable");
        lua_pushinteger(L, stats.sched_runq_max);
        lua_setfield(L, -2, "sched_runq_max");
        
        return 1;
    }
    
    lua_pushnil(L);
    lua_pushinteger(L, r);
    return 2;
}

/* Registry of functions */
static const luaL_Reg os1lib[] = {
    {"print", l_print},
    {"sleep", l_sleep},
    {"get_time", l_get_time},
    {"get_pid", l_get_pid},
    {"yield", l_yield},
    {"exit", l_exit},
    {"spawn", l_spawn},
    {"kill", l_kill},
    {"wait", l_wait},
    {"create_window", l_create_window},
    {"destroy_window", l_destroy_window},
    {"window_draw", l_window_draw},
    {"window_write", l_window_write},
    {"compositor_render", l_compositor_render},
    {"set_window_flags", l_set_window_flags},
    {"registry_get", l_registry_get},
    {"registry_set", l_registry_set},
    {"registry_del", l_registry_del},
    {"fs_read", l_fs_read},
    {"fs_write", l_fs_write},
    {"list_dir", l_list_dir},
    {"chdir", l_chdir},
    {"getcwd", l_getcwd},
    {"unlink", l_unlink},
    {"notify", l_notify},
    {"sysstats", l_sysstats},
    {NULL, NULL}
};

/* Module entry point */
int luaopen_os1(lua_State *L) {
    luaL_newlib(L, os1lib);
    return 1;
}
