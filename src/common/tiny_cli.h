#ifndef TINY_CLI_H
#define TINY_CLI_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h>

#define TINY_CLI_MAX_ARGS 64

typedef enum {
    CLI_OPT_BOOL,     /* --flag (sets bool to true) */
    CLI_OPT_INT,      /* --name <int> (parses int within [minVal, maxVal]) */
    CLI_OPT_STRING    /* --name <str> (stores pointer to argument string) */
} CliOptType;

typedef struct {
    const wchar_t *name;        /* e.g. L"--border-width" or L"-b" */
    CliOptType     type;
    void          *outValue;
    int            minVal;      /* for CLI_OPT_INT bounds validation */
    int            maxVal;
    const wchar_t *valHint;     /* e.g. L"<1-20>", L"<path>", L"png|bmp" or NULL */
    const wchar_t *description; /* e.g. L"Border overlay thickness" or NULL */
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
 * Supports both space-separated ("--flag val") and equals-separated ("--flag=val").
 */
static inline void TinyCLI_Parse(int argc, wchar_t **argv, const CliOption *opts, int optCount) {
    if (argv == NULL || opts == NULL || optCount <= 0) {
        return;
    }

    for (int i = 1; i < argc; i++) {
        for (int j = 0; j < optCount; j++) {
            size_t optLen = wcslen(opts[j].name);
            const wchar_t *valStr = NULL;
            bool matched = false;

            if (_wcsicmp(argv[i], opts[j].name) == 0) {
                matched = true;
                if (opts[j].type != CLI_OPT_BOOL && i + 1 < argc) {
                    valStr = argv[++i];
                }
            } else if (argv[i][optLen] == L'=' && _wcsnicmp(argv[i], opts[j].name, optLen) == 0) {
                matched = true;
                valStr = argv[i] + optLen + 1;
            }

            if (matched) {
                if (opts[j].type == CLI_OPT_BOOL) {
                    if (valStr && (_wcsicmp(valStr, L"false") == 0 || wcscmp(valStr, L"0") == 0)) {
                        if (opts[j].outValue) *(bool *)opts[j].outValue = false;
                    } else {
                        if (opts[j].outValue) *(bool *)opts[j].outValue = true;
                    }
                } else if (opts[j].type == CLI_OPT_INT && valStr) {
                    wchar_t *end = NULL;
                    long val = wcstol(valStr, &end, 10);
                    if (end != valStr && *end == L'\0') {
                        if (opts[j].minVal != 0 || opts[j].maxVal != 0) {
                            if (val < opts[j].minVal) val = opts[j].minVal;
                            if (val > opts[j].maxVal) val = opts[j].maxVal;
                        }
                        if (opts[j].outValue) *(int *)opts[j].outValue = (int)val;
                    }
                } else if (opts[j].type == CLI_OPT_STRING && valStr) {
                    if (opts[j].outValue) *(const wchar_t **)opts[j].outValue = valStr;
                }
                break;
            }
        }
    }
}

/*
 * TinyCLI_HasHelp:
 * Returns true if --help, -h, or /? appears in argv.
 */
static inline bool TinyCLI_HasHelp(int argc, wchar_t **argv) {
    if (!argv) return false;
    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--help") == 0 ||
            _wcsicmp(argv[i], L"-h") == 0 ||
            wcscmp(argv[i], L"/?") == 0) {
            return true;
        }
    }
    return false;
}

/*
 * TinyCLI_FormatHelp:
 * Formats a clean, aligned help text block into outBuf.
 */
static inline void TinyCLI_FormatHelp(const wchar_t *appName, const wchar_t *summary,
                                     const wchar_t *usage, const CliOption *opts,
                                     int optCount, wchar_t *outBuf, size_t maxBuf) {
    if (!outBuf || maxBuf == 0) return;
    outBuf[0] = L'\0';

    wchar_t line[512];
    if (appName && summary) {
        swprintf_s(line, sizeof(line)/sizeof(line[0]), L"%s — %s\r\n\r\n", appName, summary);
        wcsncat_s(outBuf, maxBuf, line, _TRUNCATE);
    }

    if (usage) {
        swprintf_s(line, sizeof(line)/sizeof(line[0]), L"Usage:\r\n  %s\r\n\r\n", usage);
        wcsncat_s(outBuf, maxBuf, line, _TRUNCATE);
    }

    wcsncat_s(outBuf, maxBuf, L"Options:\r\n", _TRUNCATE);

    for (int i = 0; i < optCount; i++) {
        if (!opts[i].description) continue; /* Skip alias entries without explicit description */

        wchar_t optName[128];
        if (opts[i].valHint) {
            swprintf_s(optName, sizeof(optName)/sizeof(optName[0]), L"%s %s", opts[i].name, opts[i].valHint);
        } else {
            swprintf_s(optName, sizeof(optName)/sizeof(optName[0]), L"%s", opts[i].name);
        }

        swprintf_s(line, sizeof(line)/sizeof(line[0]), L"  %-30s %s\r\n", optName, opts[i].description);
        wcsncat_s(outBuf, maxBuf, line, _TRUNCATE);
    }

    wcsncat_s(outBuf, maxBuf, L"  -h, --help, /?                 Show this help manual\r\n", _TRUNCATE);
}

/*
 * TinyCLI_OutputHelp:
 * Outputs text to console if attached/attachable; otherwise shows native MessageBoxW.
 */
static inline void TinyCLI_OutputHelp(const wchar_t *appName, const wchar_t *text) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    bool createdConOut = false;

    if (!hOut || hOut == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        }
    }

    if (!hOut || hOut == INVALID_HANDLE_VALUE) {
        hOut = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hOut && hOut != INVALID_HANDLE_VALUE) {
            createdConOut = true;
        }
    }

    if (hOut && hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            DWORD written = 0;
            WriteConsoleW(hOut, L"\r\n", 2, &written, NULL);
            WriteConsoleW(hOut, text, (DWORD)wcslen(text), &written, NULL);
            WriteConsoleW(hOut, L"\r\n", 2, &written, NULL);
        } else {
            DWORD len = (DWORD)wcslen(text);
            char *utf8 = (char *)malloc(len * 4 + 16);
            if (utf8) {
                int ulen = WideCharToMultiByte(CP_UTF8, 0, text, (int)len, utf8, (int)(len * 4), NULL, NULL);
                if (ulen > 0) {
                    DWORD written = 0;
                    WriteFile(hOut, "\r\n", 2, &written, NULL);
                    WriteFile(hOut, utf8, (DWORD)ulen, &written, NULL);
                    WriteFile(hOut, "\r\n", 2, &written, NULL);
                }
                free(utf8);
            }
        }
        if (createdConOut) {
            CloseHandle(hOut);
        }
    } else {
        MessageBoxW(NULL, text, appName ? appName : L"Help", MB_OK | MB_ICONINFORMATION);
    }
}

/*
 * TinyCLI_CheckHelp:
 * Checks if help was requested; if so, displays help and returns true.
 */
static inline bool TinyCLI_CheckHelp(int argc, wchar_t **argv, const wchar_t *appName,
                                     const wchar_t *summary, const wchar_t *usage,
                                     const CliOption *opts, int optCount) {
    if (!TinyCLI_HasHelp(argc, argv)) {
        return false;
    }
    wchar_t buf[4096];
    TinyCLI_FormatHelp(appName, summary, usage, opts, optCount, buf, sizeof(buf)/sizeof(buf[0]));
    TinyCLI_OutputHelp(appName, buf);
    return true;
}

/*
 * TinyCLI_CheckHelpCommandLine:
 * Parses GetCommandLineW() and displays help if requested.
 */
static inline bool TinyCLI_CheckHelpCommandLine(const wchar_t *appName, const wchar_t *summary,
                                                const wchar_t *usage, const CliOption *opts,
                                                int optCount) {
    const wchar_t *rawCmd = GetCommandLineW();
    if (!rawCmd) return false;

    wchar_t buf[2048];
    wcsncpy_s(buf, sizeof(buf)/sizeof(buf[0]), rawCmd, _TRUNCATE);

    wchar_t *argv[TINY_CLI_MAX_ARGS];
    int argc = TinyCLI_Tokenize(buf, argv, TINY_CLI_MAX_ARGS);

    return TinyCLI_CheckHelp(argc, argv, appName, summary, usage, opts, optCount);
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
