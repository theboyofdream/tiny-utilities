#ifndef TINY_CONFIG_H
#define TINY_CONFIG_H

#include <stdio.h>
#include <string.h>
#include <ctype.h>

/*
 * TinyConfig_Load:
 * Read-only "key = value" config file loader (plain ASCII, C99, win32 + mac).
 *
 * Format: one "key = value" pair per line; leading '#' comments; lines without
 * '=' are skipped silently. Keys and values are trimmed of surrounding blanks
 * and lower-cased before the callback sees them.
 *
 * The loader NEVER writes: callers honoring the repo's no-persistence rule
 * treat the file as user-managed input only (see mwb / PRD carve-out).
 *
 * Returns the number of lines passed to the callback; *existed (optional) is 1
 * when the file was opened, 0 when it could not be read.
 */
typedef void (*TinyConfigFn)(void *user, const char *key, const char *value);

static inline void TinyConfig_Trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
    char *p = s;
    while (*p == ' ' || *p == '\t')
        p++;
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
}

static inline void TinyConfig_Lower(char *s) {
    for (; *s; s++) {
        *s = (char)tolower((unsigned char)*s);
    }
}

static inline int TinyConfig_Load(const char *path, TinyConfigFn fn, void *user, int *existed) {
    if (existed) {
        *existed = 0;
    }
    if (!path || !*path || !fn) {
        return 0;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    if (existed) {
        *existed = 1;
    }
    char line[512];
    int count = 0;
    while (fgets(line, sizeof line, f)) {
        TinyConfig_Trim(line);
        if (!line[0] || line[0] == '#') {
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        TinyConfig_Trim(line);        TinyConfig_Lower(line);      /* key */
        TinyConfig_Trim(eq + 1);      TinyConfig_Lower(eq + 1);    /* value */
        fn(user, line, eq + 1);
        count++;
    }
    fclose(f);
    return count;
}

#endif /* TINY_CONFIG_H */
