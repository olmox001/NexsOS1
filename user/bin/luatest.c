/*
 * user/bin/luatest.c
 * Standalone verification test harness for Lua 5.4 on NexsOS1.
 *
 * Exercises Lua VM lifecycle, standard libraries (base, string, table,
 * math, utf8, coroutine, io, os), NexsOS1 os1 module, memory management,
 * and error handling.
 *
 * Output conforms to the tools/nxrun.sh test harness protocol:
 *   [luatest] PASS <case>
 *   [luatest] done: N/N passed, 0 failure(s)
 */

#include <os1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

/* Forward declaration for NexsOS1 native Lua module */
int luaopen_os1(lua_State *L);

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char *name) {
    g_pass++;
    printf("[luatest] PASS %s\n", name);
}

static void bad(const char *name, const char *why) {
    g_fail++;
    printf("[luatest] FAIL %s: %s\n", name, why ? why : "unknown error");
}

static void run_lua_snippet(lua_State *L, const char *name, const char *code) {
    int rc = luaL_dostring(L, code);
    if (rc == LUA_OK) {
        ok(name);
    } else {
        const char *err = lua_tostring(L, -1);
        bad(name, err);
        lua_pop(L, 1);
    }
}

static void test_vm_lifecycle(void) {
    lua_State *L = luaL_newstate();
    if (!L) {
        bad("vm_lifecycle", "luaL_newstate returned NULL");
        return;
    }
    lua_close(L);
    ok("vm_lifecycle");
}

static void test_openlibs(void) {
    lua_State *L = luaL_newstate();
    if (!L) {
        bad("openlibs", "failed to create state");
        return;
    }
    luaL_openlibs(L);
    lua_getglobal(L, "_G");
    int is_tab = lua_istable(L, -1);
    lua_pop(L, 1);
    lua_close(L);

    if (is_tab) {
        ok("openlibs");
    } else {
        bad("openlibs", "_G is not a table");
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("=== NexsOS1 Lua 5.4 Conformance Suite ===\n");

    test_vm_lifecycle();
    test_openlibs();

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[luatest] FAIL: could not create primary lua state\n");
        return 1;
    }
    luaL_openlibs(L);

    /* Pre-load os1 module so require('os1') works */
    luaL_requiref(L, "os1", luaopen_os1, 1);
    lua_pop(L, 1);

    run_lua_snippet(L, "basic_eval",
        "assert(1 + 1 == 2, 'arithmetic failed'); "
        "assert(5 * 6 == 30, 'multiply failed'); "
        "assert('a' .. 'b' == 'ab', 'concat failed'); "
        "assert(not false, 'boolean logic failed')");

    run_lua_snippet(L, "string_library",
        "local s = string.format('hex: 0x%04X, str: %s', 255, 'ok'); "
        "assert(s == 'hex: 0x00FF, str: ok', 'string.format failed'); "
        "assert(string.sub('hello world', 1, 5) == 'hello', 'string.sub failed'); "
        "assert(string.gsub('banana', 'a', 'o') == 'bonono', 'string.gsub failed'); "
        "assert(string.byte('A') == 65 and string.char(66) == 'B', 'byte/char failed')");

    run_lua_snippet(L, "table_library",
        "local t = { 30, 10, 20 }; "
        "table.insert(t, 40); "
        "assert(#t == 4, 'insert failed'); "
        "table.sort(t); "
        "assert(t[1] == 10 and t[2] == 20 and t[3] == 30 and t[4] == 40, 'sort failed'); "
        "assert(table.concat(t, '-') == '10-20-30-40', 'concat failed'); "
        "local r = table.remove(t, 1); "
        "assert(r == 10 and #t == 3, 'remove failed')");

    run_lua_snippet(L, "math_library",
        "assert(math.abs(-12) == 12, 'abs failed'); "
        "assert(math.floor(4.9) == 4, 'floor failed'); "
        "assert(math.ceil(4.1) == 5, 'ceil failed'); "
        "assert(math.sqrt(49) == 7, 'sqrt failed'); "
        "assert(17 // 5 == 3, 'idiv failed'); "
        "assert(17 % 5 == 2, 'mod failed'); "
        "assert(math.max(1, 10, 5) == 10, 'max failed'); "
        "assert(math.min(1, 10, 5) == 1, 'min failed')");

    run_lua_snippet(L, "utf8_library",
        "assert(utf8.len('hello') == 5, 'utf8.len ascii failed'); "
        "assert(utf8.char(65) == 'A', 'utf8.char failed'); "
        "assert(utf8.codepoint('A') == 65, 'utf8.codepoint failed')");

    run_lua_snippet(L, "coroutines",
        "local co = coroutine.create(function(a, b) "
        "  coroutine.yield(a + b); "
        "  return a * b; "
        "end); "
        "local ok1, r1 = coroutine.resume(co, 4, 5); "
        "assert(ok1 and r1 == 9, 'first resume failed'); "
        "local ok2, r2 = coroutine.resume(co); "
        "assert(ok2 and r2 == 20, 'second resume failed')");

    run_lua_snippet(L, "error_handling",
        "local ok, err = pcall(function() error('sample_fail') end); "
        "assert(not ok, 'pcall should have failed'); "
        "assert(string.find(err, 'sample_fail'), 'error message mismatch')");

    run_lua_snippet(L, "gc_collection",
        "collectgarbage('collect'); "
        "local before = collectgarbage('count'); "
        "local junk = {}; "
        "for i = 1, 500 do junk[i] = string.rep('x', 64) end; "
        "junk = nil; "
        "collectgarbage('collect'); "
        "local after = collectgarbage('count'); "
        "assert(after <= before + 50, 'gc reclaiming failed')");

    run_lua_snippet(L, "os_time",
        "local t = os.time(); "
        "assert(type(t) == 'number' and t > 0, 'os.time failed'); "
        "local c = os.clock(); "
        "assert(type(c) == 'number' and c >= 0, 'os.clock failed')");

    run_lua_snippet(L, "os1_module",
        "local os1 = require('os1'); "
        "assert(type(os1.get_pid) == 'function', 'os1.get_pid missing'); "
        "local pid = os1.get_pid(); "
        "assert(pid > 0, 'invalid pid'); "
        "local tm = os1.get_time(); "
        "assert(tm > 0, 'invalid time'); "
        "os1.sleep(5)");

    run_lua_snippet(L, "file_io",
        "local f = assert(io.open('/home/.luatest.tmp', 'w')); "
        "f:write('NexsOS1_Lua_File_IO\\nLine2\\n'); "
        "f:close(); "
        "local f2 = assert(io.open('/home/.luatest.tmp', 'r')); "
        "local l1 = f2:read('*l'); "
        "local l2 = f2:read('*l'); "
        "f2:close(); "
        "assert(l1 == 'NexsOS1_Lua_File_IO', 'first line mismatch'); "
        "assert(l2 == 'Line2', 'second line mismatch'); "
        "os.remove('/home/.luatest.tmp')");

    run_lua_snippet(L, "os_getenv_execute",
        "local path = os.getenv('PATH'); "
        "assert(path ~= nil, 'PATH not set'); "
        "local ok, how, code = os.execute('true'); "
        "assert(ok == true, 'execute true failed')");

    lua_close(L);

    int total = g_pass + g_fail;
    printf("[luatest] done: %d/%d passed, %d failure(s)\n", g_pass, total, g_fail);
    printf("luatest] done: %d/%d passed, %d failure(s)\n", g_pass, total, g_fail);

    return g_fail == 0 ? 0 : 1;
}
