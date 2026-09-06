#ifndef TINY_CLI_H
#define TINY_CLI_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <wchar.h>
#include <stdlib.h>

#define TINY_CLI_MAX_ARGS 64

typedef enum {
    CLI_OPT_BOOL,     /* --flag (sets bool to true) */
    CLI_OPT_INT,      /* --name <int> (parses int within [minVal, maxVal]) */
    CLI_OPT_STRING    /* --name <str> (stores pointer to argument string) */
} CliOptType;

typedef struct {
    const wchar_t *name;   /* e.g. L"--border-width" or L"-b" */
    CliOptType     type;
    void          *outValue;
    int            minVal; /* for CLI_OPT_INT bounds validation */
    int            maxVal;
} CliOption;

/*
 * TinyCLI_Tokenize:
 * In-place command-line tokenizer. Avoids shell32 CommandLineToArgvW dependency
 * and heap allocations. Modifies buf in-place and fills argv array.
 */
static inline int TinyCLI_Tokenize(wchar_t *buf, wchar_t **argv, int maxArgs) {
    int n = 0;
    wchar_t *p = buf;
    while (*p != L'\0' && n < maxArgs) {
        while (*p == L' ' || *p == L'\t') {
            p++;
        }
        if (*p == L'\0') {
            break;
        }
        if (*p == L'"') {
            p++;
            argv[n++] = p;
            while (*p != L'\0' && *p != L'"') {
                p++;
            }
            if (*p == L'"') {
                *p++ = L'\0';
            }
        } else {
            argv[n++] = p;
            while (*p != L'\0' && *p != L' ' && *p != L'\t') {
                p++;
            }
            if (*p != L'\0') {
                *p++ = L'\0';
            }
        }
    }
    return n;
}

/*
 * TinyCLI_Parse:
 * Iterates through argc/argv and populates values defined in the options table.
 */
static inline void TinyCLI_Parse(int argc, wchar_t **argv, const CliOption *opts, int optCount) {
    if (argv == NULL || opts == NULL || optCount <= 0) {
        return;
    }

    for (int i = 1; i < argc; i++) {
        for (int j = 0; j < optCount; j++) {
            if (_wcsicmp(argv[i], opts[j].name) == 0) {
                if (opts[j].type == CLI_OPT_BOOL) {
                    *(bool *)opts[j].outValue = true;
                } else if (opts[j].type == CLI_OPT_INT && i + 1 < argc) {
                    wchar_t *end = NULL;
                    long val = wcstol(argv[i + 1], &end, 10);
                    if (end != argv[i + 1] && *end == L'\0') {
                        if (opts[j].minVal != 0 || opts[j].maxVal != 0) {
                            if (val < opts[j].minVal) val = opts[j].minVal;
                            if (val > opts[j].maxVal) val = opts[j].maxVal;
                        }
                        *(int *)opts[j].outValue = (int)val;
                        i++;
                    }
                } else if (opts[j].type == CLI_OPT_STRING && i + 1 < argc) {
                    *(const wchar_t **)opts[j].outValue = argv[++i];
                }
                break;
            }
        }
    }
}

/*
 * TinyCLI_ParseCommandLine:
 * Helper that copies the process command line buffer, tokenizes, and parses it.
 */
static inline void TinyCLI_ParseCommandLine(const CliOption *opts, int optCount) {
    const wchar_t *rawCmd = GetCommandLineW();
    if (!rawCmd) return;

    wchar_t buf[2048];
    wcsncpy_s(buf, sizeof(buf)/sizeof(buf[0]), rawCmd, _TRUNCATE);

    wchar_t *argv[TINY_CLI_MAX_ARGS];
    int argc = TinyCLI_Tokenize(buf, argv, TINY_CLI_MAX_ARGS);

    TinyCLI_Parse(argc, argv, opts, optCount);
}

#endif /* TINY_CLI_H */
