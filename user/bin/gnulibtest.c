/*
 * user/bin/gnulibtest.c
 * Verification test harness for GNU Gnulib on NexsOS1.
 */

#include <os1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "gnulib_os1_glue.h"
#include "basename-lgpl.h"
#include "dirname.h"
#include "c-ctype.h"
#include "c-strcase.h"
#include "inttostr.h"
#include "bitrotate.h"
#include "gl_array_list.h"
#include "gl_linked_list.h"


static int g_passed = 0;
static int g_failed = 0;

static void check_bool(const char *name, int cond) {
    /* Each real check is visible in mdebug without a controlling window. */
    printf_win(0, "[GNULIB][%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (cond) {
        printf("  [PASS] %s\n", name);
        g_passed++;
    } else {
        printf("  [FAIL] %s\n", name);
        g_failed++;
    }
}

static void test_basename_dirname(void) {
    const char *p1 = "/sbin/nxinit";
    const char *b1 = last_component(p1);
    check_bool("basename last_component", strcmp(b1, "nxinit") == 0);

    char *d1 = mdir_name(p1);
    check_bool("dirname mdir_name", d1 && strcmp(d1, "/sbin") == 0);
    if (d1) free(d1);

    char path_buf[64];
    strncpy(path_buf, "/home/shared/test///", sizeof(path_buf));
    strip_trailing_slashes(path_buf);
    check_bool("strip_trailing_slashes", strcmp(path_buf, "/home/shared/test") == 0);
}

static void test_c_ctype_and_strcase(void) {
    check_bool("c_isalnum('A')", c_isalnum('A'));
    check_bool("c_isdigit('9')", c_isdigit('9'));
    check_bool("c_isalpha('z')", c_isalpha('z'));
    check_bool("c_tolower('G')", c_tolower('G') == 'g');
    check_bool("c_toupper('m')", c_toupper('m') == 'M');
    check_bool("c_strcasecmp", c_strcasecmp("NexsOS1", "nexsos1") == 0);
    check_bool("c_strncasecmp", c_strncasecmp("NexsOS1_kernel", "NEXSOS1_user", 7) == 0);
}

static void test_inttostr(void) {
    char buf[INT_BUFSIZE_BOUND(intmax_t)];
    char *res1 = inttostr(12345, buf);
    check_bool("inttostr positive", res1 && strcmp(res1, "12345") == 0);

    char *res2 = inttostr(-9876, buf);
    check_bool("inttostr negative", res2 && strcmp(res2, "-9876") == 0);

    char *res3 = imaxtostr(0, buf);
    check_bool("imaxtostr zero", res3 && strcmp(res3, "0") == 0);
}

static void test_bitrotate(void) {
    uint32_t val32 = 0x12345678;
    uint32_t rot1 = rotl32(val32, 4);
    uint32_t rot2 = rotr32(rot1, 4);
    check_bool("bitrotate 32-bit rotl/rotr symmetry", rot2 == val32);

    uint64_t val64 = 0x0123456789ABCDEFULL;
    uint64_t rot64_1 = rotl64(val64, 8);
    uint64_t rot64_2 = rotr64(rot64_1, 8);
    check_bool("bitrotate 64-bit rotl/rotr symmetry", rot64_2 == val64);
}

static void test_gl_list(void) {
    gl_list_t al = gl_list_nx_create_empty(GL_ARRAY_LIST, NULL, NULL, NULL, true);
    check_bool("gl_array_list create", al != NULL);
    if (al) {
        check_bool("gl_array_list add 1", gl_list_nx_add_last(al, (void *)"item1") != NULL);
        check_bool("gl_array_list add 2", gl_list_nx_add_last(al, (void *)"item2") != NULL);
        check_bool("gl_array_list add 3", gl_list_nx_add_last(al, (void *)"item3") != NULL);
        check_bool("gl_array_list size", gl_list_size(al) == 3);
        const char *elem = (const char *)gl_list_get_at(al, 1);
        check_bool("gl_array_list get_at", elem && strcmp(elem, "item2") == 0);
        gl_list_free(al);
    }

    gl_list_t ll = gl_list_nx_create_empty(GL_LINKED_LIST, NULL, NULL, NULL, true);
    check_bool("gl_linked_list create", ll != NULL);
    if (ll) {
        check_bool("gl_linked_list add 1", gl_list_nx_add_last(ll, (void *)"nodeA") != NULL);
        check_bool("gl_linked_list add 2", gl_list_nx_add_last(ll, (void *)"nodeB") != NULL);
        check_bool("gl_linked_list size", gl_list_size(ll) == 2);

        gl_list_free(ll);
    }

}

static void test_gnulib_os1_glue(void) {
    check_bool("glue getpagesize", gnulib_os1_getpagesize() == 4096);
    check_bool("glue getdtablesize", gnulib_os1_getdtablesize() == 64);

    gnulib_os1_setprogname("/sys/bin/gnulibtest");
    check_bool("glue progname", strcmp(gnulib_os1_getprogname(), "gnulibtest") == 0);

    const char *str = "hello gnulib world";
    void *m1 = gnulib_os1_rawmemchr(str, 'g');
    check_bool("glue rawmemchr", m1 == (str + 6));

    void *m2 = gnulib_os1_memrchr(str, 'o', strlen(str));
    check_bool("glue memrchr", m2 == (str + 14));
}

static void test_gnulib_vfs_io(void) {
    /* Validate the real on-image machine-owned service manifest, not an
     * obsolete /etc fixture; user read access remains supported. */
    int source = open("/system/boot/services.conf", O_RDONLY);
    check_bool("vfs service manifest open", source >= 3);
    if (source >= 3) {
        char header[64] = {0};
        ssize_t count = gnulib_os1_safe_read(source, header,
                                             sizeof(header) - 1U);
        check_bool("vfs gnulib safe_read", count > 0 &&
                   strstr(header, "name|mode") != NULL);
        check_bool("vfs service manifest close", close(source) == 0);
    }

    /* A distinct real writable ext4 file tests safe_write and readback.
     * This is not a synthetic success or reuse of fdtest's live data. */
    int fd = open("/home/shared/gnulib.fixture", O_RDWR);
    check_bool("vfs gnulib ext4 fixture open", fd >= 3);
    if (fd >= 3) {
        const char payload[] = "GNULIB-RW-1234";
        char verify[sizeof(payload)] = {0};
        ssize_t n = gnulib_os1_safe_write(fd, payload,
                                          sizeof(payload) - 1U);
        check_bool("vfs gnulib safe_write",
                   n == (ssize_t)(sizeof(payload) - 1U));
        check_bool("vfs gnulib seek rewind", lseek(fd, 0, SEEK_SET) == 0);
        ssize_t readback = gnulib_os1_safe_read(fd, verify,
                                                sizeof(payload) - 1U);
        check_bool("vfs gnulib byte roundtrip",
                   readback == (ssize_t)(sizeof(payload) - 1U) &&
                   memcmp(verify, payload, sizeof(payload) - 1U) == 0);
        check_bool("vfs gnulib file close", close(fd) == 0);
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("=== NexsOS1 GNU Gnulib Test Suite ===\n");

    test_basename_dirname();
    test_c_ctype_and_strcase();
    test_inttostr();
    test_bitrotate();
    test_gl_list();
    test_gnulib_os1_glue();
    test_gnulib_vfs_io();

    /* Report individual checks, not seven artificial suite counts. */
    printf_win(0, "[gnulibtest] done: %d/%d passed, %d failure(s)\n",
               g_passed, g_passed + g_failed, g_failed);
    if (printf("[gnulibtest] done: %d/%d passed, %d failure(s)\n",
               g_passed, g_passed + g_failed, g_failed) < 0)
        return 1;
    return g_failed == 0 ? 0 : 1;
}
