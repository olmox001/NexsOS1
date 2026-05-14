#include <kernel/types.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/printk.h>

/*
 * Path Resolution:
 * - If path starts with '/', it's absolute.
 * - Otherwise, it's relative to current_process->cwd.
 * - Normalizes . and ..
 */
void vfs_resolve_path(const char *in, char *out, size_t size) {
    char temp[256];
    if (in[0] == '/') {
        strncpy(temp, in, sizeof(temp));
    } else {
        strncpy(temp, current_process->cwd, sizeof(temp));
        size_t len = strlen(temp);
        if (len > 0 && temp[len-1] != '/') {
            strncat(temp, "/", sizeof(temp) - len - 1);
        }
        strncat(temp, in, sizeof(temp) - strlen(temp) - 1);
    }
    temp[sizeof(temp)-1] = '\0';

    /* Normalize path: remove redundant /./ and handle /../ */
    char normalized[256];
    char *parts[32];
    int part_count = 0;
    
    char *s = temp;
    if (*s == '/') s++;
    
    char *token = s;
    while (token && *token) {
        char *next = strchr(token, '/');
        if (next) *next = '\0';
        
        if (strcmp(token, "..") == 0) {
            if (part_count > 0) part_count--;
        } else if (strcmp(token, ".") != 0 && strlen(token) > 0) {
            if (part_count < 32) parts[part_count++] = token;
        }
        
        if (next) token = next + 1;
        else break;
    }
    
    strcpy(normalized, "/");
    for (int i = 0; i < part_count; i++) {
        strncat(normalized, parts[i], sizeof(normalized) - strlen(normalized) - 1);
        if (i < part_count - 1) strncat(normalized, "/", sizeof(normalized) - strlen(normalized) - 1);
    }
    
    strncpy(out, normalized, size);
    out[size-1] = '\0';
}
