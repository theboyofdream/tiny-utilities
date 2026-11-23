/*
 * Bindy — a clean, readable keymapper / hotkey scripting language for Windows.
 * Source: bindy.c
 *
 * Spec: docs/bindy-language.md (language) + docs/tools/bindy.md (tool).
 * Guide: AGENTS.md / docs/PRD.md (hard rules + §21 carve-out registry).
 *
 * Runtime model (granted by PRD §21, bindy row):
 *   bindy <script.bindy>  -> parse script, run on_start, register global hotkeys,
 *                            stay resident on a thread message loop, exit cleanly
 *                            on IPC toggle (second launch) or Ctrl+C.
 *   Remaps are hotkey-style (RegisterHotKey + SendInput); the triggering key is not
 *   suppressed. No low-level hooks, no GUI windows, no disk writes, no config files.
 *
 * Hard rules kept: C99, Win32 only, wide APIs, static helpers + g_ globals,
 * one process per invocation, no services/registry/persistence/global hooks.
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "common/tiny_cli.h"
#include "common/tiny_ipc.h"

#define APPMUTEX_NAME    L"Global\\TinyBindyMutex"
#define APPEVENT_NAME    L"Global\\TinyBindyEvent"

#define SCRIPT_CAP       (1 << 20)   /* 1 MiB of wide characters                 */
#define MAX_LINES        4096
#define MAX_VALUE        1024
#define MAX_TEXT_CHARS   8192        /* max text of one statement (cmd/ps1 body) */
#define MAX_TEXT_POOL    (1 << 20)   /* wide chars for all statement texts       */
#define MAX_NAME         64
#define MAX_STMTS        1024
#define MAX_VARS         128
#define MAX_ARGS         512
#define MAX_FUNCS        64
#define MAX_PARAMS       8
#define MAX_HOTKEYS      256
#define MAX_ARMS         256
#define MAX_SCOPES       8
#define MAX_TOKENS       8
#define MAX_IDS          1024
#define OUTPUT_CAP       8192
#define CMD_TIMEOUT_MS   60000

/* ------------------------------------------------------------------ */
/* Data model                                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    ST_NOP = 0,
    ST_VARSET,   /* name = text (isConst)                                */
    ST_KEYS,     /* text = key combo to send                             */
    ST_CMD,      /* text = command line (targetVar >= 0 -> capture out)  */
    ST_PS1,      /* text = PowerShell source (EncodedCommand)            */
    ST_CALL,     /* name = function, argStart/argCount into g_args       */
    ST_MATCH     /* armStart/armCount into g_arms                        */
} StmtKind;

typedef struct {
    StmtKind kind;
    int      textOff, textLen;        /* slice of g_textPool (see StmtText) */
    wchar_t  name[MAX_NAME];
    int      targetVar;      /* 1 = capture stdout into name, else 0     */
    int      isConst;
    int      funcIndex;      /* resolved at runtime                      */
    int      argStart, argCount;
    int      armStart, armCount;
    int      skipCount;       /* nested body statements following this one */
} Stmt;

typedef struct {
    wchar_t cond[MAX_VALUE]; /* empty = default arm                      */
    int     bodyStart, bodyCount;
} Arm;

typedef struct {
    wchar_t name[MAX_NAME];
    wchar_t params[MAX_PARAMS][MAX_NAME];
    int     paramCount;
    int     bodyStart, bodyCount;
} Func;

typedef struct {
    wchar_t name[MAX_NAME];
    wchar_t value[MAX_VALUE];
    bool    isConst;
    bool    set;
} Var;

typedef struct {
    wchar_t names[MAX_PARAMS][MAX_NAME];
    wchar_t values[MAX_PARAMS][MAX_VALUE];
    int     count;
} Scope;

typedef struct {
    UINT mods;               /* MOD_* flags incl. MOD_NOREPEAT           */
    UINT vk;
    int  tapMs;              /* 0 = single press, >0 = double-tap window */
    UINT tapMods;            /* modifiers required when the tap key goes down */
    bool prevDown;           /* previous GetAsyncKeyState edge               */
    int  stmtStart, stmtCount;
    int  id;
    bool registered;
    bool reserved;           /* kept for diagnostics; hook handles all combos */
    bool passThrough;        /* AHK '~' prefix: do not suppress the key       */
    bool shortPress;         /* single-key (Nms): fire on key-up within Nms  */
    bool onUp;               /* double-tap: fire on second key-up             */
    wchar_t text[MAX_VALUE]; /* original combo text, for diagnostics        */
} Hotkey;

typedef struct { const wchar_t *name; UINT vk; UINT mod; bool ext; } KeyDef;
typedef struct { UINT vk; UINT mod; bool ext; } KeyToken;

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static wchar_t  g_script[SCRIPT_CAP];
static wchar_t *g_lines[MAX_LINES];
static int      g_lineCount;

static Stmt     g_stmts[MAX_STMTS];
static int      g_stmtCount;
static Var      g_vars[MAX_VARS];
static int      g_varCount;
static Func     g_funcs[MAX_FUNCS];
static int      g_funcCount;
static Arm      g_arms[MAX_ARMS];
static int      g_armCount;
static wchar_t  g_args[MAX_ARGS][MAX_VALUE];
static int      g_argCount;
static wchar_t  g_textPool[MAX_TEXT_POOL];   /* all statement texts live here */
static size_t   g_textPoolLen = 0;
static int      g_maxChars = MAX_VALUE;     /* per-statement text limit (default 1024) */

static int      g_initList[MAX_STMTS];                /* top-level var/const   */
static int      g_initListCount = 0;
static int      g_startStart = 0, g_startCount = -1; /* on_start body         */

static Hotkey   g_hotkeys[MAX_HOTKEYS];
static int      g_hotkeyCount;
static int      g_idMap[MAX_IDS];
static ULONGLONG g_lastTap[MAX_IDS];

static Scope    g_scopes[MAX_SCOPES];
static int      g_scopeDepth;

static HANDLE   g_toggleEvent = NULL;
static HANDLE   g_wakeEvent = NULL;
static bool     g_running = true;
static bool     g_hasTapHotkeys = false;

/* Low-level keyboard hook state (granted by PRD §21, bindy row). */
static HHOOK    g_keyboardHook = NULL;
static HWND     g_hiddenWnd = NULL;
static bool     g_keyDown[256];
static ULONGLONG g_keyDownAt[256];
static ULONGLONG g_lastUp[MAX_IDS];
static bool     g_suppressUp[256];
static bool     g_skipTapUp[256];   /* modifier was consumed by another hotkey */
static bool     g_syntheticMods = false;  /* ignore release/restore key events */

/* Forward declarations (used before their definitions). */
static bool IsIdentStart(wchar_t c);
static bool IsIdentChar(wchar_t c);
static void StripQuotes(wchar_t *s);
static bool IsModifierVk(UINT vk);

/* ------------------------------------------------------------------ */
/* Tiny helpers                                                        */
/* ------------------------------------------------------------------ */

static void Warn(const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(stderr, fmt, ap);
    va_end(ap);
}

static void Trim(wchar_t *s) {
    if (!s) return;
    wchar_t *start = s;
    while (*start == L' ' || *start == L'\t' || *start == L'\r') start++;
    wchar_t *end = start + wcslen(start);
    while (end > start && (end[-1] == L' ' || end[-1] == L'\t' || end[-1] == L'\r')) end--;
    *end = L'\0';
    if (start != s) wmemmove(s, start, (size_t)(end - start) + 1);
}

static void StripCommentsLine(wchar_t *line, bool *inBlock) {
    wchar_t *out = line;
    bool inQuote = false;
    for (wchar_t *p = line; *p; ) {
        if (*inBlock) {
            if (p[0] == L'*' && p[1] == L'/') { *inBlock = false; p += 2; }
            else p++;
            continue;
        }
        if (!inQuote && p[0] == L'/' && p[1] == L'/') { *out = L'\0'; return; }
        if (!inQuote && p[0] == L'/' && p[1] == L'*') { *inBlock = true; p += 2; continue; }
        if (*p == L'"') inQuote = !inQuote;
        *out++ = *p++;
    }
    *out = L'\0';
}

static bool StartsWithI(const wchar_t *s, const wchar_t *prefix) {
    return _wcsnicmp(s, prefix, wcslen(prefix)) == 0;
}

static bool IsEndLine(const wchar_t *s)   { return _wcsicmp(s, L":end") == 0; }
static bool EndsWithColon(const wchar_t *s) {
    size_t n = wcslen(s);
    return n > 0 && s[n - 1] == L':';
}
static bool IsArmHeader(const wchar_t *s) {
    return StartsWithI(s, L"when ") || _wcsicmp(s, L"default:") == 0 ||
           StartsWithI(s, L"default:");
}
/* Arm headers auto-close; they never own a :end, so never count them as openers. */
static bool IsOpener(const wchar_t *s) {
    return EndsWithColon(s) && !IsEndLine(s) && !IsArmHeader(s);
}

/* Index of the matching :end line, or -1. */
static int FindBlockEnd(int start, int limit) {
    int depth = 1;
    for (int i = start; i < limit; i++) {
        const wchar_t *l = g_lines[i];
        if (IsEndLine(l)) {
            if (--depth == 0) return i;
        } else if (IsOpener(l)) {
            depth++;
        }
    }
    return -1;
}

/* First line of the next arm / enclosing :end, or limit. */
static int FindArmEnd(int start, int limit) {
    int depth = 0;
    for (int i = start; i < limit; i++) {
        const wchar_t *l = g_lines[i];
        if (depth == 0 && IsArmHeader(l)) return i;
        if (IsEndLine(l)) {
            if (depth == 0) return i;
            depth--;
        } else if (IsOpener(l)) {
            depth++;
        }
    }
    return limit;
}

/* Statement texts are pooled so a multi-line cmd/ps1 body can exceed MAX_VALUE. */
static wchar_t *StmtText(const Stmt *s) {
    return g_textPool + s->textOff;
}

static void SetStmtText(Stmt *s, const wchar_t *text) {
    size_t n = wcslen(text);
    if (n >= (size_t)g_maxChars) n = (size_t)g_maxChars - 1;
    if (g_textPoolLen + n + 1 > MAX_TEXT_POOL) {
        Warn(L"bindy: text pool full\n");
        n = 0;
    }
    s->textOff = (int)g_textPoolLen;
    s->textLen = (int)n;
    wmemcpy(g_textPool + g_textPoolLen, text, n);
    g_textPoolLen += n;
    g_textPool[g_textPoolLen++] = L'\0';
}

static void JoinLines(int start, int count, wchar_t *out, size_t cap) {
    out[0] = L'\0';
    size_t len = 0;
    for (int i = 0; i < count; i++) {
        const wchar_t *l = g_lines[start + i];
        size_t n = wcslen(l);
        if (len + n + 2 >= cap) break;
        if (len) out[len++] = L'\n';
        wmemcpy(out + len, l, n);
        len += n;
        out[len] = L'\0';
    }
}

/* First top-level occurrence of ch, ignoring quoted sections. */
static wchar_t *FindTopLevelChar(wchar_t *s, wchar_t ch) {
    bool q = false;
    for (; *s; s++) {
        if (*s == L'"') { q = !q; continue; }
        if (!q && *s == ch) return s;
    }
    return NULL;
}

/* First assignment '=' that is not part of ==, !=, <= or >= (quotes skipped). */
static wchar_t *FindAssignSep(wchar_t *s) {
    bool q = false;
    wchar_t prev = 0;
    for (; *s; s++) {
        wchar_t c = *s;
        if (c == L'"') { q = !q; prev = c; continue; }
        if (!q && c == L'=') {
            if (s[1] == L'=' || prev == L'!' || prev == L'<' || prev == L'>' || prev == L'=') {
                prev = c;
                continue;
            }
            return s;
        }
        prev = c;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Key names, combos and SendInput                                     */
/* ------------------------------------------------------------------ */

static const KeyDef g_keys[] = {
    { L"alt",        VK_MENU,       MOD_ALT,     false },
    { L"ctrl",       VK_CONTROL,    MOD_CONTROL, false },
    { L"shift",      VK_SHIFT,      MOD_SHIFT,   false },
    { L"win",        VK_LWIN,       MOD_WIN,     true  },
    { L"lctrl",      VK_LCONTROL,   MOD_CONTROL, false },
    { L"rctrl",      VK_RCONTROL,   MOD_CONTROL, true  },
    { L"lalt",       VK_LMENU,      MOD_ALT,     true  },
    { L"ralt",       VK_RMENU,      MOD_ALT,     true  },
    { L"lshift",     VK_LSHIFT,     MOD_SHIFT,   false },
    { L"rshift",     VK_RSHIFT,     MOD_SHIFT,   false },
    { L"lwin",       VK_LWIN,       MOD_WIN,     true  },
    { L"rwin",       VK_RWIN,       MOD_WIN,     true  },
    { L"home",       VK_HOME,       0,           true  },
    { L"end",        VK_END,        0,           true  },
    { L"apps",       VK_APPS,       0,           true  },
    { L"printscreen",VK_SNAPSHOT,   0,           false },
    { L"prtsc",      VK_SNAPSHOT,   0,           false },
    { L"tab",        VK_TAB,        0,           false },
    { L"esc",        VK_ESCAPE,     0,           false },
    { L"escape",     VK_ESCAPE,     0,           false },
    { L"enter",      VK_RETURN,     0,           false },
    { L"return",     VK_RETURN,     0,           false },
    { L"space",      VK_SPACE,      0,           false },
    { L"backspace",  VK_BACK,       0,           false },
    { L"delete",     VK_DELETE,     0,           true  },
    { L"del",        VK_DELETE,     0,           true  },
    { L"insert",     VK_INSERT,     0,           true  },
    { L"ins",        VK_INSERT,     0,           true  },
    { L"pgup",       VK_PRIOR,      0,           true  },
    { L"pgdn",       VK_NEXT,       0,           true  },
    { L"up",         VK_UP,         0,           true  },
    { L"down",       VK_DOWN,       0,           true  },
    { L"left",       VK_LEFT,       0,           true  },
    { L"right",      VK_RIGHT,      0,           true  },
    { L"f1",  VK_F1,  0, false }, { L"f2",  VK_F2,  0, false },
    { L"f3",  VK_F3,  0, false }, { L"f4",  VK_F4,  0, false },
    { L"f5",  VK_F5,  0, false }, { L"f6",  VK_F6,  0, false },
    { L"f7",  VK_F7,  0, false }, { L"f8",  VK_F8,  0, false },
    { L"f9",  VK_F9,  0, false }, { L"f10", VK_F10, 0, false },
    { L"f11", VK_F11, 0, false }, { L"f12", VK_F12, 0, false },
    { L"f13", VK_F13, 0, false }, { L"f14", VK_F14, 0, false },
    { L"f15", VK_F15, 0, false }, { L"f16", VK_F16, 0, false },
    { L"f17", VK_F17, 0, false }, { L"f18", VK_F18, 0, false },
    { L"f19", VK_F19, 0, false }, { L"f20", VK_F20, 0, false },
    { L"f21", VK_F21, 0, false }, { L"f22", VK_F22, 0, false },
    { L"f23", VK_F23, 0, false }, { L"f24", VK_F24, 0, false },
};

static bool LookupKey(const wchar_t *name, KeyDef *out) {
    for (size_t i = 0; i < sizeof(g_keys) / sizeof(g_keys[0]); i++) {
        if (_wcsicmp(name, g_keys[i].name) == 0) { *out = g_keys[i]; return true; }
    }
    if (wcslen(name) == 1) {
        wchar_t c = name[0];
        UINT vk = 0;
        if (c >= L'a' && c <= L'z') vk = (UINT)(c - L'a' + L'A');
        else if (c >= L'A' && c <= L'Z') vk = (UINT)c;
        else if (c >= L'0' && c <= L'9') vk = (UINT)c;
        else {
            switch (c) {
                case L'-': vk = VK_OEM_MINUS;  break;
                case L'=': vk = VK_OEM_PLUS;   break;
                case L'[': vk = VK_OEM_4;      break;
                case L']': vk = VK_OEM_6;      break;
                case L';': vk = VK_OEM_1;      break;
                case L'\'': vk = VK_OEM_7;     break;
                case L',': vk = VK_OEM_COMMA;  break;
                case L'.': vk = VK_OEM_PERIOD; break;
                case L'/': vk = VK_OEM_2;      break;
                case L'\\': vk = VK_OEM_5;     break;
                case L'`': vk = VK_OEM_3;      break;
                default: break;
            }
        }
        if (vk) {
            out->name = NULL; out->vk = vk; out->mod = 0; out->ext = false;
            return true;
        }
    }
    return false;
}

/* Parse "alt+j" / "lctrl+lctrl (300ms)" into tokens + optional tap window. */
static bool ParseCombo(const wchar_t *text, int *tapMs, KeyToken *tokens, int *tokenCount) {
    wchar_t buf[MAX_VALUE];
    wcsncpy_s(buf, MAX_VALUE, text, _TRUNCATE);
    *tapMs = 0;

    wchar_t *open = wcschr(buf, L'(');
    if (open) {
        wchar_t *close = wcschr(open + 1, L')');
        if (close) {
            wchar_t *num = open + 1;
            int ms = 0;
            bool digits = false;
            for (; num < close && *num >= L'0' && *num <= L'9'; num++) {
                ms = ms * 10 + (*num - L'0');
                digits = true;
            }
            while (num < close && ((*num >= L'a' && *num <= L'z') ||
                                   (*num >= L'A' && *num <= L'Z') || *num == L' ')) num++;
            if (digits && num == close) {
                *tapMs = ms;
                *open = L'\0';
            }
        }
    }

    int n = 0;
    wchar_t *ctx = NULL;
    for (wchar_t *tok = wcstok_s(buf, L"+", &ctx); tok && n < MAX_TOKENS;
         tok = wcstok_s(NULL, L"+", &ctx)) {
        Trim(tok);
        if (!tok[0]) continue;
        KeyDef kd;
        if (!LookupKey(tok, &kd)) {
            Warn(L"bindy: unknown key '%ls' in combo '%ls'\n", tok, text);
            return false;
        }
        tokens[n].vk = kd.vk;
        tokens[n].mod = kd.mod;
        tokens[n].ext = kd.ext;
        n++;
    }
    *tokenCount = n;
    return n > 0;
}

static INPUT MakeInput(KeyToken k, bool up) {
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)k.vk;
    in.ki.dwFlags = (k.ext ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
    return in;
}

static void SendCombo(const wchar_t *text) {
    KeyToken t[MAX_TOKENS];
    int n = 0, tap = 0;
    if (!ParseCombo(text, &tap, t, &n)) return;
    INPUT inputs[MAX_TOKENS * 2];
    int c = 0;
    for (int i = 0; i < n; i++) inputs[c++] = MakeInput(t[i], false);
    for (int i = n - 1; i >= 0; i--) inputs[c++] = MakeInput(t[i], true);
    if (c > 0) SendInput((UINT)c, inputs, sizeof(INPUT));
}

/* Combos the OS owns outright: RegisterHotKey can never claim these. */
static bool IsKnownReservedCombo(const wchar_t *text) {
    return _wcsicmp(text, L"alt+tab") == 0 ||                 /* task switcher      */
           _wcsicmp(text, L"ctrl+alt+delete") == 0 ||
           _wcsicmp(text, L"ctrl+alt+del") == 0 ||
           _wcsicmp(text, L"alt+ctrl+delete") == 0 ||
           _wcsicmp(text, L"win+l") == 0 ||                   /* lock screen        */
           _wcsicmp(text, L"win+alt+o") == 0 ||               /* on-screen keyboard */
           _wcsicmp(text, L"win+ctrl+c") == 0 ||              /* color filters      */
           _wcsicmp(text, L"win+ctrl+o") == 0 ||              /* narrator           */
           _wcsicmp(text, L"win+ctrl+d") == 0;                /* virtual desktop    */
}

/* Translate a combo into VK/modifier arguments plus its tap semantics. */
static bool ComboToHotkey(const wchar_t *text, UINT *mods, UINT *vk, int *tapMs,
                          bool *shortPress, bool *doubleTap) {
    KeyToken t[MAX_TOKENS];
    int n = 0;
    *shortPress = false;
    *doubleTap = false;
    if (!ParseCombo(text, tapMs, t, &n)) return false;

    /* "lctrl (200ms)" = short press; "lctrl+lctrl (300ms)" = double-tap. */
    if (*tapMs > 0 && n == 1) {
        *mods = MOD_NOREPEAT;
        *vk = t[0].vk;
        *shortPress = true;
        return true;
    }
    if (*tapMs > 0 && n == 2 && t[0].vk == t[1].vk && t[0].mod == t[1].mod) {
        *mods = MOD_NOREPEAT;
        *vk = t[0].vk;
        *doubleTap = true;
        return true;
    }

    UINT m = MOD_NOREPEAT;
    for (int i = 0; i < n - 1; i++) {
        if (!t[i].mod) {
            Warn(L"bindy: only the last key in a combo may be a non-modifier ('%ls')\n", text);
            return false;
        }
        m |= t[i].mod;
    }
    *mods = m;
    *vk = t[n - 1].vk;
    return true;
}

/* ------------------------------------------------------------------ */
/* Variables, scopes and text expansion                                */
/* ------------------------------------------------------------------ */

static void ExpandTextEx(const wchar_t *in, wchar_t *out, size_t cap, int depth);

static int FindVar(const wchar_t *name) {
    for (int i = 0; i < g_varCount; i++) {
        if (_wcsicmp(g_vars[i].name, name) == 0) return i;
    }
    return -1;
}

static bool ResolveVar(const wchar_t *name, wchar_t *out, size_t cap) {
    for (int s = g_scopeDepth - 1; s >= 0; s--) {
        for (int i = 0; i < g_scopes[s].count; i++) {
            if (_wcsicmp(g_scopes[s].names[i], name) == 0) {
                wcsncpy_s(out, cap, g_scopes[s].values[i], _TRUNCATE);
                return true;
            }
        }
    }
    int vi = FindVar(name);
    if (vi >= 0) {
        wcsncpy_s(out, cap, g_vars[vi].value, _TRUNCATE);
        return true;
    }
    return false;
}

static void SetVar(const wchar_t *name, const wchar_t *value, bool isConst) {
    if (!name || !name[0]) return;
    wchar_t clean[MAX_VALUE];
    wcsncpy_s(clean, MAX_VALUE, value, _TRUNCATE);
    StripQuotes(clean);                 /* "hello" -> hello for literals */
    wchar_t expanded[MAX_VALUE];
    ExpandTextEx(clean, expanded, MAX_VALUE, 0);

    for (int s = g_scopeDepth - 1; s >= 0; s--) {
        for (int i = 0; i < g_scopes[s].count; i++) {
            if (_wcsicmp(g_scopes[s].names[i], name) == 0) {
                wcsncpy_s(g_scopes[s].values[i], MAX_VALUE, expanded, _TRUNCATE);
                return;
            }
        }
    }

    int vi = FindVar(name);
    if (vi >= 0) {
        if (g_vars[vi].isConst || (isConst && g_vars[vi].set)) {
            Warn(L"bindy: ignoring reassignment of const '%ls'\n", name);
            return;
        }
        wcsncpy_s(g_vars[vi].value, MAX_VALUE, expanded, _TRUNCATE);
        if (isConst) g_vars[vi].isConst = true;
        g_vars[vi].set = true;
        return;
    }
    if (g_varCount < MAX_VARS) {
        Var *v = &g_vars[g_varCount++];
        ZeroMemory(v, sizeof(*v));
        wcsncpy_s(v->name, MAX_NAME, name, _TRUNCATE);
        wcsncpy_s(v->value, MAX_VALUE, expanded, _TRUNCATE);
        v->isConst = isConst;
        v->set = true;
    } else {
        Warn(L"bindy: variable table full (max %d)\n", MAX_VARS);
    }
}

/* $name -> Bindy var/const (left intact if unknown), %NAME% -> environment var. */
static void ExpandTextEx(const wchar_t *in, wchar_t *out, size_t cap, int depth) {
    if (depth > 8) { wcsncpy_s(out, cap, in, _TRUNCATE); return; }
    size_t o = 0;
    for (const wchar_t *p = in; *p; ) {
        if (*p == L'%') {
            const wchar_t *q = wcschr(p + 1, L'%');
            if (q && q > p + 1) {
                wchar_t name[256];
                size_t n = (size_t)(q - p - 1);
                if (n < 256) {
                    wmemcpy(name, p + 1, n);
                    name[n] = L'\0';
                    wchar_t val[MAX_VALUE];
                    DWORD got = GetEnvironmentVariableW(name, val, MAX_VALUE);
                    if (got > 0 && got < MAX_VALUE) {
                        for (const wchar_t *v = val; *v && o + 1 < cap; v++) out[o++] = *v;
                        p = q + 1;
                        continue;
                    }
                }
            }
        } else if (*p == L'$') {
            const wchar_t *q = p + 1;
            while (IsIdentChar(*q)) q++;
            if (q > p + 1) {
                wchar_t name[MAX_NAME];
                size_t n = (size_t)(q - p - 1);
                if (n < MAX_NAME) {
                    wmemcpy(name, p + 1, n);
                    name[n] = L'\0';
                    wchar_t val[MAX_VALUE];
                    if (ResolveVar(name, val, MAX_VALUE)) {
                        for (const wchar_t *v = val; *v && o + 1 < cap; v++) out[o++] = *v;
                        p = q;
                        continue;
                    }
                }
            }
        }
        if (o + 1 < cap) out[o++] = *p;
        p++;
    }
    out[o] = L'\0';
}

static void ExpandText(const wchar_t *in, wchar_t *out, size_t cap) {
    ExpandTextEx(in, out, cap, 0);
}

/* ------------------------------------------------------------------ */
/* Command / PowerShell execution with captured stdout                 */
/* ------------------------------------------------------------------ */

static void Base64Utf16(const wchar_t *text, char *out, size_t cap) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *bytes = (const unsigned char *)text;
    size_t nbytes = wcslen(text) * 2;
    size_t o = 0;
    for (size_t i = 0; i < nbytes && o + 4 < cap; i += 3) {
        unsigned int v = ((unsigned int)bytes[i] << 16) |
                         ((i + 1 < nbytes) ? (unsigned int)bytes[i + 1] << 8 : 0u) |
                         ((i + 2 < nbytes) ? (unsigned int)bytes[i + 2] : 0u);
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i + 1 < nbytes) ? tbl[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < nbytes) ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
}

static void AppendBytesAsWide(const char *bytes, DWORD got, wchar_t *out, size_t *len, size_t cap) {
    wchar_t tmp[1100];
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes, (int)got, tmp, 1100);
    for (int i = 0; i < n && *len + 1 < cap; i++) out[(*len)++] = tmp[i];
    out[*len] = L'\0';
}

static void RunCommand(const wchar_t *raw, bool ps1, bool capture, wchar_t *out, size_t cap) {
    out[0] = L'\0';
    wchar_t exp[MAX_TEXT_CHARS];
    ExpandText(raw, exp, (size_t)g_maxChars);

    wchar_t cmdLine[MAX_TEXT_CHARS * 4];
    if (ps1) {
        char b64[MAX_TEXT_CHARS * 6];
        Base64Utf16(exp, b64, sizeof(b64));
        swprintf_s(cmdLine, MAX_TEXT_CHARS * 4,
                   L"powershell.exe -NoProfile -NonInteractive -EncodedCommand %hs", b64);
    } else {
        swprintf_s(cmdLine, MAX_TEXT_CHARS * 4, L"cmd.exe /c %ls", exp);
    }

    if (!capture) {
        /* Fire-and-forget: cmd.exe /c waits for GUI children, so blocking here
         * would freeze the keyboard hook and break repeated toggles. */
        STARTUPINFOW si;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(pi));
        wchar_t mutableCmd[MAX_TEXT_CHARS * 4];
        wcsncpy_s(mutableCmd, MAX_TEXT_CHARS * 4, cmdLine, _TRUNCATE);
        if (!CreateProcessW(NULL, mutableCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                            NULL, NULL, &si, &pi)) {
            Warn(L"bindy: launch failed for '%ls' (error %lu)\n", cmdLine, GetLastError());
            return;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return;
    }

    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL, erd = NULL, ewr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0) || !CreatePipe(&erd, &ewr, &sa, 0)) {
        Warn(L"bindy: CreatePipe failed (%lu)\n", GetLastError());
        return;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(erd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = ewr;      /* stderr drained but not captured (PS progress noise) */
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    wchar_t mutableCmd[MAX_VALUE * 4];
    wcsncpy_s(mutableCmd, MAX_VALUE * 4, cmdLine, _TRUNCATE);

    if (!CreateProcessW(NULL, mutableCmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        Warn(L"bindy: CreateProcess failed for '%ls' (%lu)\n", cmdLine, GetLastError());
        CloseHandle(rd); CloseHandle(wr);
        CloseHandle(erd); CloseHandle(ewr);
        return;
    }
    CloseHandle(wr);
    CloseHandle(ewr);

    char bytes[1024];
    wchar_t collected[OUTPUT_CAP];
    size_t len = 0;
    collected[0] = L'\0';
    DWORD started = GetTickCount();

    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            DWORD got = 0;
            if (ReadFile(rd, bytes, (avail < 1024 ? avail : 1024), &got, NULL) && got > 0) {
                AppendBytesAsWide(bytes, got, collected, &len, OUTPUT_CAP);
            }
        }
        DWORD eavail = 0;                       /* drain stderr so children never block */
        if (PeekNamedPipe(erd, NULL, 0, NULL, &eavail, NULL) && eavail > 0) {
            DWORD got = 0;
            char junk[1024];
            ReadFile(erd, junk, (eavail < 1024 ? eavail : 1024), &got, NULL);
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            for (;;) {
                DWORD left = 0;
                if (!PeekNamedPipe(rd, NULL, 0, NULL, &left, NULL) || left == 0) break;
                DWORD got = 0;
                if (!ReadFile(rd, bytes, (left < 1024 ? left : 1024), &got, NULL) || got == 0) break;
                AppendBytesAsWide(bytes, got, collected, &len, OUTPUT_CAP);
            }
            break;
        }
        if (GetTickCount() - started > CMD_TIMEOUT_MS) {
            TerminateProcess(pi.hProcess, 1);
            Warn(L"bindy: command timed out (%d ms): '%ls'\n", CMD_TIMEOUT_MS, exp);
            break;
        }
        Sleep(10);
    }

    CloseHandle(rd);
    CloseHandle(erd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    while (len > 0 && (collected[len - 1] == L'\r' || collected[len - 1] == L'\n' ||
                       collected[len - 1] == L' '  || collected[len - 1] == L'\t')) {
        collected[--len] = L'\0';
    }
    wcsncpy_s(out, cap, collected, _TRUNCATE);
}

/* ------------------------------------------------------------------ */
/* Condition evaluation                                                */
/* ------------------------------------------------------------------ */

static bool IsIdentStart(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_';
}
static bool IsIdentChar(wchar_t c) {
    return IsIdentStart(c) || (c >= L'0' && c <= L'9');
}

static bool IsTruthy(const wchar_t *s) {
    if (!s || !s[0]) return false;
    return !(_wcsicmp(s, L"0") == 0 || _wcsicmp(s, L"false") == 0 ||
             _wcsicmp(s, L"no") == 0 || _wcsicmp(s, L"off") == 0);
}

static void StripQuotes(wchar_t *s) {
    size_t n = wcslen(s);
    if (n >= 2 && ((s[0] == L'"' && s[n - 1] == L'"') ||
                   (s[0] == L'\'' && s[n - 1] == L'\''))) {
        s[n - 1] = L'\0';
        wmemmove(s, s + 1, n - 1);
    }
}

static void Operand(const wchar_t *token, wchar_t *out, size_t cap) {
    bool ident = IsIdentStart(token[0]);
    if (ident) {
        for (const wchar_t *p = token; *p; p++) {
            if (!IsIdentChar(*p)) { ident = false; break; }
        }
    }
    if (ident && ResolveVar(token, out, cap)) return;
    wcsncpy_s(out, cap, token, _TRUNCATE);
}

static bool ToNumber(const wchar_t *s, double *out) {
    if (!s[0]) return false;
    wchar_t *end = NULL;
    double v = wcstod(s, &end);
    if (end == s || *end != L'\0') return false;
    *out = v;
    return true;
}

static bool EvalCondition(const wchar_t *cond) {
    wchar_t c[MAX_VALUE];
    ExpandText(cond, c, MAX_VALUE);

    wchar_t *op = NULL;
    size_t oplen = 0;
    for (wchar_t *p = c; *p; p++) {
        if (*p == L'"') {
            p++;
            while (*p && *p != L'"') p++;
            if (!*p) break;
            continue;
        }
        wchar_t ch = *p;
        if (ch == L'=' || ch == L'!' || ch == L'<' || ch == L'>') {
            if (p[1] == ch && (ch == L'=' || ch == L'!' || ch == L'<' || ch == L'>')) {
                op = p; oplen = 2; break;
            }
            if (ch == L'=' || ch == L'<' || ch == L'>') { op = p; oplen = 1; break; }
        }
    }
    if (!op) return IsTruthy(c);

    wchar_t o0 = op[0];
    wchar_t o1 = (oplen == 2) ? op[1] : L'\0';
    *op = L'\0';
    wchar_t *l = op - 1;
    wchar_t *r = op + oplen;
    while (l >= c && (*l == L' ' || *l == L'\t')) l--;
    l[1] = L'\0';
    while (*r == L' ' || *r == L'\t') r++;
    Trim(c);
    Trim(r);
    StripQuotes(c);
    StripQuotes(r);

    wchar_t lv[MAX_VALUE], rv[MAX_VALUE];
    Operand(c, lv, MAX_VALUE);
    Operand(r, rv, MAX_VALUE);

    bool kind = (oplen == 2 && o0 == L'=' && o1 == L'=') ||
                (oplen == 1 && o0 == L'=');            /* equality family */
    bool neq  = (oplen == 2 && o0 == L'!' && o1 == L'=');
    bool lt   = (oplen == 1 && o0 == L'<');
    bool gt   = (oplen == 1 && o0 == L'>');
    bool le   = (oplen == 2 && o0 == L'<' && o1 == L'=') || lt;
    bool ge   = (oplen == 2 && o0 == L'>' && o1 == L'=') || gt;

    double ln = 0, rn = 0;
    if (ToNumber(lv, &ln) && ToNumber(rv, &rn)) {
        if (kind) return ln == rn;
        if (neq)  return ln != rn;
        if (lt)   return ln < rn;
        if (gt)   return ln > rn;
        if (le)   return ln <= rn;
        if (ge)   return ln >= rn;
        return false;
    }

    int cmp = _wcsicmp(lv, rv);
    if (kind) return cmp == 0;
    if (neq)  return cmp != 0;
    if (lt)   return cmp < 0;
    if (gt)   return cmp > 0;
    if (le)   return cmp <= 0;
    if (ge)   return cmp >= 0;
    return false;
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

static int FindFunc(const wchar_t *name) {
    for (int i = 0; i < g_funcCount; i++) {
        if (_wcsicmp(g_funcs[i].name, name) == 0) return i;
    }
    return -1;
}

static void AddArg(const wchar_t *s) {
    if (g_argCount < MAX_ARGS) {
        wcsncpy_s(g_args[g_argCount++], MAX_VALUE, s, _TRUNCATE);
    } else {
        Warn(L"bindy: argument pool full (max %d)\n", MAX_ARGS);
    }
}

/* "snap(fullscreen)" -> name + raw comma-separated args. */
static bool ParseCall(const wchar_t *line, wchar_t *name, int *argStart, int *argCount) {
    const wchar_t *p = line;
    if (!IsIdentStart(*p)) return false;
    while (IsIdentChar(*p)) p++;
    if (*p != L'(') return false;
    size_t len = wcslen(line);
    if (len < 2 || line[len - 1] != L')') return false;
    size_t nlen = (size_t)(p - line);
    if (nlen == 0 || nlen >= MAX_NAME) return false;
    wmemcpy(name, line, nlen);
    name[nlen] = L'\0';

    wchar_t inner[MAX_VALUE];
    size_t innerLen = len - nlen - 2;
    if (innerLen >= MAX_VALUE) return false;
    wmemcpy(inner, p + 1, innerLen);
    inner[innerLen] = L'\0';

    int before = g_argCount;
    bool q = false;
    wchar_t arg[MAX_VALUE];
    size_t a = 0;
    for (wchar_t *s = inner; ; s++) {
        if (*s == L'"') { q = !q; arg[a++] = *s; continue; }
        if ((*s == L',' && !q) || *s == L'\0') {
            arg[a] = L'\0';
            Trim(arg);
            if (arg[0]) AddArg(arg);
            a = 0;
            if (!*s) break;
            continue;
        }
        if (a + 1 < MAX_VALUE) arg[a++] = *s;
    }
    *argStart = before;
    *argCount = g_argCount - before;
    return true;
}

static void PushStmt(const Stmt *s) {
    if (g_stmtCount < MAX_STMTS) g_stmts[g_stmtCount++] = *s;
    else Warn(L"bindy: statement table full (max %d)\n", MAX_STMTS);
}

/* Parse a single action that has no nested block (inline action / arm action). */
static void ParseStatementText(const wchar_t *text, int *outStart, int *outCount) {
    int before = g_stmtCount;
    Stmt s;
    ZeroMemory(&s, sizeof(s));
    s.targetVar = -1;
    s.funcIndex = -1;

    if (StartsWithI(text, L"var ") || StartsWithI(text, L"const ")) {
        bool isConst = StartsWithI(text, L"const ");
        wchar_t work[MAX_VALUE];
        wcsncpy_s(work, MAX_VALUE, text + (isConst ? 6 : 4), _TRUNCATE);
        wchar_t *eq = FindAssignSep(work);
        if (eq) {
            *eq = L'\0';
            wchar_t *val = eq + 1;
            Trim(work);
            Trim(val);
            s.kind = ST_VARSET;
            s.isConst = isConst ? 1 : 0;
            wcsncpy_s(s.name, MAX_NAME, work, _TRUNCATE);
            SetStmtText(&s, val);
        } else {
            s.kind = ST_NOP;
        }
    } else if (StartsWithI(text, L"cmd ") || StartsWithI(text, L"ps1 ")) {
        bool ps1 = StartsWithI(text, L"ps1 ");
        s.kind = ps1 ? ST_PS1 : ST_CMD;
        SetStmtText(&s, text + 4);
    } else {
        wchar_t fname[MAX_NAME];
        int as = 0, ac = 0;
        if (ParseCall(text, fname, &as, &ac)) {
            s.kind = ST_CALL;
            wcsncpy_s(s.name, MAX_NAME, fname, _TRUNCATE);
            s.argStart = as;
            s.argCount = ac;
        } else {
            s.kind = ST_KEYS;
            SetStmtText(&s, text);
        }
    }
    PushStmt(&s);
    *outStart = before;
    *outCount = g_stmtCount - before;
}

static void ParseMatch(int start, int limit, Stmt *st);

/* Parse the statement at line idx; return the next unparsed line index. */
static int ParseStatementAt(int idx, int limit, int *outStart, int *outCount) {
    const wchar_t *line = g_lines[idx];
    int before = g_stmtCount;

    if (_wcsicmp(line, L"cmd:") == 0 || _wcsicmp(line, L"ps1:") == 0) {
        bool ps1 = (line[0] == L'p' || line[0] == L'P');
        int end = FindBlockEnd(idx + 1, limit);
        if (end < 0) {
            Warn(L"bindy: unterminated %ls block\n", ps1 ? L"ps1:" : L"cmd:");
            *outStart = before; *outCount = 0; return limit;
        }
        wchar_t body[MAX_TEXT_CHARS];
        JoinLines(idx + 1, end - (idx + 1), body, (size_t)g_maxChars);
        Stmt s; ZeroMemory(&s, sizeof(s));
        s.targetVar = -1; s.funcIndex = -1;
        s.kind = ps1 ? ST_PS1 : ST_CMD;
        SetStmtText(&s, body);
        PushStmt(&s);
        *outStart = before; *outCount = g_stmtCount - before;
        return end + 1;
    }

    if (_wcsicmp(line, L"match:") == 0) {
        int end = FindBlockEnd(idx + 1, limit);
        if (end < 0) {
            Warn(L"bindy: unterminated match: block\n");
            *outStart = before; *outCount = 0; return limit;
        }
        Stmt s; ZeroMemory(&s, sizeof(s));
        s.targetVar = -1; s.funcIndex = -1;
        s.kind = ST_MATCH;
        int selfIndex = g_stmtCount;
        PushStmt(&s);                       /* owner first, nested arms after */
        ParseMatch(idx + 1, end, &g_stmts[selfIndex]);
        g_stmts[selfIndex].skipCount = g_stmtCount - (selfIndex + 1);
        *outStart = before; *outCount = g_stmtCount - before;
        return end + 1;
    }

    if (StartsWithI(line, L"var ") || StartsWithI(line, L"const ")) {
        bool isConst = StartsWithI(line, L"const ");
        wchar_t work[MAX_VALUE];
        wcsncpy_s(work, MAX_VALUE, line + (isConst ? 6 : 4), _TRUNCATE);
        wchar_t *eq = FindAssignSep(work);
        if (!eq) {
            Warn(L"bindy: malformed assignment '%ls'\n", line);
            ParseStatementText(line, outStart, outCount);
            return idx + 1;
        }
        *eq = L'\0';
        wchar_t *val = eq + 1;
        Trim(work);
        Trim(val);
        Stmt s; ZeroMemory(&s, sizeof(s));
        s.targetVar = -1; s.funcIndex = -1;
        wcsncpy_s(s.name, MAX_NAME, work, _TRUNCATE);

        if (_wcsicmp(val, L"cmd:") == 0 || _wcsicmp(val, L"ps1:") == 0) {
            bool ps1 = (val[0] == L'p' || val[0] == L'P');
            int end = FindBlockEnd(idx + 1, limit);
            if (end < 0) {
                Warn(L"bindy: unterminated output block for '%ls'\n", work);
                *outStart = before; *outCount = 0; return limit;
            }
            wchar_t body[MAX_TEXT_CHARS];
            JoinLines(idx + 1, end - (idx + 1), body, (size_t)g_maxChars);
            s.kind = ps1 ? ST_PS1 : ST_CMD;
            s.targetVar = 1;
            SetStmtText(&s, body);
            PushStmt(&s);
            *outStart = before; *outCount = g_stmtCount - before;
            return end + 1;
        }

        if (StartsWithI(val, L"cmd ") || StartsWithI(val, L"ps1 ")) {
            bool ps1 = StartsWithI(val, L"ps1 ");
            s.kind = ps1 ? ST_PS1 : ST_CMD;
            s.targetVar = 1;
            SetStmtText(&s, val + 4);
        } else {
            s.kind = ST_VARSET;
            s.isConst = isConst ? 1 : 0;
            SetStmtText(&s, val);
        }
        PushStmt(&s);
        *outStart = before; *outCount = g_stmtCount - before;
        return idx + 1;
    }

    ParseStatementText(line, outStart, outCount);
    return idx + 1;
}

static void ParseMatch(int start, int limit, Stmt *st) {
    int armBefore = g_armCount;
    int i = start;
    while (i < limit) {
        const wchar_t *line = g_lines[i];
        if (IsEndLine(line)) break;
        Arm arm;
        ZeroMemory(&arm, sizeof(arm));
        int next = i + 1;

        if (StartsWithI(line, L"when ")) {
            wchar_t work[MAX_VALUE];
            wcsncpy_s(work, MAX_VALUE, line + 5, _TRUNCATE);
            wchar_t *sep = FindAssignSep(work);
            if (sep && sep[1] && !EndsWithColon(work)) {
                *sep = L'\0';
                wchar_t *action = sep + 1;
                Trim(work);
                Trim(action);
                wcsncpy_s(arm.cond, MAX_VALUE, work, _TRUNCATE);
                int bs, bc;
                ParseStatementText(action, &bs, &bc);
                arm.bodyStart = bs;
                arm.bodyCount = bc;
            } else if (EndsWithColon(work)) {
                work[wcslen(work) - 1] = L'\0';
                Trim(work);
                wcsncpy_s(arm.cond, MAX_VALUE, work, _TRUNCATE);
                int ae = FindArmEnd(i + 1, limit);
                arm.bodyStart = g_stmtCount;
                for (int j = i + 1; j < ae; ) {
                    int bs, bc;
                    j = ParseStatementAt(j, ae, &bs, &bc);
                }
                arm.bodyCount = g_stmtCount - arm.bodyStart;
                next = ae;
            } else {
                Warn(L"bindy: malformed when arm '%ls'\n", line);
            }
        } else if (IsArmHeader(line)) {  /* default: */
            int ae = FindArmEnd(i + 1, limit);
            arm.cond[0] = L'\0';
            arm.bodyStart = g_stmtCount;
            for (int j = i + 1; j < ae; ) {
                int bs, bc;
                j = ParseStatementAt(j, ae, &bs, &bc);
            }
            arm.bodyCount = g_stmtCount - arm.bodyStart;
            next = ae;
        } else {
            Warn(L"bindy: unexpected line in match: '%ls'\n", line);
        }

        if (g_armCount < MAX_ARMS) g_arms[g_armCount++] = arm;
        else Warn(L"bindy: arm table full (max %d)\n", MAX_ARMS);
        i = next;
    }
    st->armStart = armBefore;
    st->armCount = g_armCount - armBefore;
}

static bool LoadScript(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Warn(L"bindy: cannot open script '%ls' (error %lu)\n", path, GetLastError());
        return false;
    }
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0 || size > SCRIPT_CAP * 4) {
        Warn(L"bindy: script too large or empty\n");
        CloseHandle(h);
        return false;
    }
    char *bytes = (char *)malloc((size_t)size + 4);
    if (!bytes) { CloseHandle(h); return false; }
    DWORD read = 0;
    if (!ReadFile(h, bytes, size, &read, NULL)) {
        Warn(L"bindy: ReadFile failed (%lu)\n", GetLastError());
        free(bytes); CloseHandle(h); return false;
    }
    CloseHandle(h);

    int wideLen = 0;
    if (size >= 2 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE) {
        wideLen = (int)((size - 2) / 2);
        if (wideLen >= SCRIPT_CAP) wideLen = SCRIPT_CAP - 1;
        wmemcpy(g_script, (const wchar_t *)(bytes + 2), (size_t)wideLen);
    } else {
        const char *src = bytes;
        DWORD srcLen = size;
        if (size >= 3 && (unsigned char)bytes[0] == 0xEF &&
            (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) {
            src += 3;
            srcLen -= 3;
        }
        wideLen = MultiByteToWideChar(CP_UTF8, 0, src, (int)srcLen, g_script, SCRIPT_CAP - 1);
    }
    free(bytes);
    if (wideLen < 0) { Warn(L"bindy: script is not valid UTF-8/UTF-16\n"); return false; }
    g_script[wideLen] = L'\0';

    g_lineCount = 0;
    bool inBlock = false;
    for (wchar_t *p = g_script; *p && g_lineCount < MAX_LINES; ) {
        wchar_t *line = p;
        while (*p && *p != L'\n' && *p != L'\r') p++;
        if (*p == L'\r') p++;
        if (*p == L'\n') p++;
        if (*(p - 1) == L'\n' || *(p - 1) == L'\r') *(p - 1) = L'\0';
        StripCommentsLine(line, &inBlock);
        Trim(line);
        if (line[0]) g_lines[g_lineCount++] = line;
    }
    return true;
}

static bool ParseScript(void) {
    g_stmtCount = g_varCount = g_funcCount = g_armCount = g_argCount = g_hotkeyCount = 0;
    g_textPoolLen = 0;
    g_maxChars = MAX_VALUE;
    g_initListCount = 0;
    g_startStart = 0; g_startCount = -1;

    for (int i = 0; i < g_lineCount; i++) {
        const wchar_t *line = g_lines[i];

        /* Script directive: raise the per-statement text limit (default 1024).
         * Accepts: max_script_chars 8192 / max_script_chars = 8192. */
        if (StartsWithI(line, L"max_script_chars")) {
            const wchar_t *p = line;
            while (*p && *p != L' ' && *p != L'\t' && *p != L'=') p++;
            while (*p == L' ' || *p == L'\t' || *p == L'=') p++;
            wchar_t *end = NULL;
            long v = wcstol(p, &end, 10);
            if (end != p && *end == L'\0') {
                if (v < 256) v = 256;
                if (v > MAX_TEXT_CHARS) v = MAX_TEXT_CHARS;
                g_maxChars = (int)v;
            } else {
                Warn(L"bindy: malformed '%ls' directive (expected an integer)\n", line);
            }
            continue;
        }

        if (_wcsicmp(line, L"on_start:") == 0) {
            int end = FindBlockEnd(i + 1, g_lineCount);
            if (end < 0) { Warn(L"bindy: unterminated on_start: block\n"); return false; }
            g_startStart = g_stmtCount;
            for (int j = i + 1; j < end; ) {
                int bs, bc;
                j = ParseStatementAt(j, end, &bs, &bc);
            }
            g_startCount = g_stmtCount - g_startStart;
            i = end;
            continue;
        }

        if (StartsWithI(line, L"fn ")) {
            wchar_t hdr[MAX_VALUE];
            wcsncpy_s(hdr, MAX_VALUE, line, _TRUNCATE);
            const wchar_t *lp = wcschr(hdr, L'(');
            const wchar_t *rp = wcsrchr(hdr, L')');
            if (!lp || !rp || rp < lp) {
                Warn(L"bindy: malformed function header '%ls'\n", line);
                continue;
            }
            Func f; ZeroMemory(&f, sizeof(f));
            size_t nlen = (size_t)(lp - (hdr + 3));
            if (nlen == 0 || nlen >= MAX_NAME) { Warn(L"bindy: bad function name\n"); continue; }
            wmemcpy(f.name, hdr + 3, nlen);
            f.name[nlen] = L'\0';

            wchar_t params[MAX_VALUE];
            size_t plen = (size_t)(rp - lp - 1);
            if (plen >= MAX_VALUE) plen = MAX_VALUE - 1;
            wmemcpy(params, lp + 1, plen);
            params[plen] = L'\0';
            wchar_t *ctx = NULL;
            for (wchar_t *p = wcstok_s(params, L",", &ctx);
                 p && f.paramCount < MAX_PARAMS; p = wcstok_s(NULL, L",", &ctx)) {
                Trim(p);
                if (p[0]) {
                    wcsncpy_s(f.params[f.paramCount++], MAX_NAME, p, _TRUNCATE);
                }
            }

            int end = FindBlockEnd(i + 1, g_lineCount);
            if (end < 0) { Warn(L"bindy: unterminated function '%ls'\n", f.name); continue; }
            f.bodyStart = g_stmtCount;
            for (int j = i + 1; j < end; ) {
                int bs, bc;
                j = ParseStatementAt(j, end, &bs, &bc);
            }
            f.bodyCount = g_stmtCount - f.bodyStart;
            if (g_funcCount < MAX_FUNCS) g_funcs[g_funcCount++] = f;
            else Warn(L"bindy: function table full (max %d)\n", MAX_FUNCS);
            i = end;
            continue;
        }

        if (StartsWithI(line, L"var ") || StartsWithI(line, L"const ") ||
            _wcsicmp(line, L"match:") == 0) {
            int bs, bc;
            int before = g_stmtCount;
            int next = ParseStatementAt(i, g_lineCount, &bs, &bc);
            if (g_stmtCount > before && g_initListCount < MAX_STMTS) {
                g_initList[g_initListCount++] = before;
            }
            i = next - 1;
            continue;
        }

        /* Hotkey definition: left-hand combo, then '=' (single action) or ':' (block). */
        wchar_t work[MAX_VALUE];
        wcsncpy_s(work, MAX_VALUE, line, _TRUNCATE);
        wchar_t *eq = FindAssignSep(work);
        wchar_t *colon = FindTopLevelChar(work, L':');
        bool useEq = (eq != NULL) && (!colon || eq < colon);
        wchar_t *sep = useEq ? eq : colon;
        if (!sep) {
            int bs, bc;
            int before = g_stmtCount;
            ParseStatementText(line, &bs, &bc);
            if (g_stmtCount > before && g_initListCount < MAX_STMTS) {
                g_initList[g_initListCount++] = before;
            }
            continue;
        }

        *sep = L'\0';
        wchar_t *rhs = sep + 1;
        Trim(work);
        Trim(rhs);
        Hotkey h; ZeroMemory(&h, sizeof(h));
        if (work[0] == L'~') {          /* AHK pass-through prefix */
            h.passThrough = true;
            wmemmove(work, work + 1, wcslen(work));
        }
        if (!ComboToHotkey(work, &h.mods, &h.vk, &h.tapMs,
                           &h.shortPress, &h.onUp)) continue;
        h.tapMods = h.mods & ~MOD_NOREPEAT;
        wcsncpy_s(h.text, MAX_VALUE, work, _TRUNCATE);
        h.reserved = IsKnownReservedCombo(h.text);

        if (useEq) {
            int bs, bc;
            ParseStatementText(rhs, &bs, &bc);
            h.stmtStart = bs;
            h.stmtCount = bc;
        } else {
            int end = FindBlockEnd(i + 1, g_lineCount);
            if (end < 0) { Warn(L"bindy: unterminated hotkey block for '%ls'\n", work); continue; }
            h.stmtStart = g_stmtCount;
            for (int j = i + 1; j < end; ) {
                int bs, bc;
                j = ParseStatementAt(j, end, &bs, &bc);
            }
            h.stmtCount = g_stmtCount - h.stmtStart;
            i = end;
        }
        if (g_hotkeyCount < MAX_HOTKEYS) g_hotkeys[g_hotkeyCount++] = h;
        else Warn(L"bindy: hotkey table full (max %d)\n", MAX_HOTKEYS);
    }

    if (g_startCount < 0) g_startCount = 0;
    return true;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

static void ExecRange(int start, int count);

static void ExecMatch(const Stmt *s) {
    for (int i = 0; i < s->armCount; i++) {
        Arm *a = &g_arms[s->armStart + i];
        if (a->cond[0] == L'\0' || EvalCondition(a->cond)) {
            ExecRange(a->bodyStart, a->bodyCount);
            return;
        }
    }
}

static void ExecCall(const Stmt *s) {
    int fi = FindFunc(s->name);
    if (fi < 0) { Warn(L"bindy: unknown function '%ls'\n", s->name); return; }
    Func *f = &g_funcs[fi];
    if (g_scopeDepth >= MAX_SCOPES) { Warn(L"bindy: call depth limit reached\n"); return; }
    Scope *sc = &g_scopes[g_scopeDepth++];
    ZeroMemory(sc, sizeof(*sc));
    for (int i = 0; i < f->paramCount && i < s->argCount && i < MAX_PARAMS; i++) {
        wchar_t exp[MAX_VALUE];
        ExpandText(g_args[s->argStart + i], exp, MAX_VALUE);
        wcsncpy_s(sc->names[i], MAX_NAME, f->params[i], _TRUNCATE);
        wcsncpy_s(sc->values[i], MAX_VALUE, exp, _TRUNCATE);
        sc->count++;
    }
    ExecRange(f->bodyStart, f->bodyCount);
    g_scopeDepth--;
}

static void ExecStmt(const Stmt *s) {
    switch (s->kind) {
        case ST_VARSET:
            SetVar(s->name, StmtText(s), s->isConst != 0);
            break;
        case ST_KEYS:
            SendCombo(StmtText(s));
            break;
        case ST_CMD:
        case ST_PS1: {
            wchar_t out[OUTPUT_CAP];
            RunCommand(StmtText(s), s->kind == ST_PS1, s->targetVar >= 0, out, OUTPUT_CAP);
            if (s->targetVar >= 0) SetVar(s->name, out, false);
            break;
        }
        case ST_CALL:
            ExecCall(s);
            break;
        case ST_MATCH:
            ExecMatch(s);
            break;
        default:
            break;
    }
}

static void ExecRange(int start, int count) {
    for (int i = 0; i < count; i++) {
        const Stmt *s = &g_stmts[start + i];
        ExecStmt(s);
        i += s->skipCount;   /* nested bodies run via their owning statement */
    }
}

static void ExecInit(void) {
    for (int i = 0; i < g_initListCount; i++) ExecStmt(&g_stmts[g_initList[i]]);
}

static void SendVkEvent(UINT vk, bool up) {
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof(INPUT));
}

static void HandleHotkey(int id) {
    if (id < 0 || id >= MAX_IDS) return;
    int idx = g_idMap[id];
    if (idx < 0 || idx >= g_hotkeyCount) return;
    Hotkey *h = &g_hotkeys[idx];

    /* AHK-like: release held modifiers while sending, then restore them, so an
     * injected Home/End/F4 is not interpreted as Alt+Home / Ctrl+F4, etc. */
    UINT mods[4];
    int n = 0;
    if (g_keyDown[VK_LCONTROL] || g_keyDown[VK_RCONTROL] || g_keyDown[VK_CONTROL]) mods[n++] = VK_CONTROL;
    if (g_keyDown[VK_LMENU] || g_keyDown[VK_RMENU] || g_keyDown[VK_MENU]) mods[n++] = VK_MENU;
    if (g_keyDown[VK_LSHIFT] || g_keyDown[VK_RSHIFT] || g_keyDown[VK_SHIFT]) mods[n++] = VK_SHIFT;
    if (g_keyDown[VK_LWIN] || g_keyDown[VK_RWIN]) mods[n++] = VK_LWIN;
    /* A bare modifier hotkey owns its key: keep the OS modifier state cleared so
     * Ctrl+wheel (browser zoom) cannot fire while it is held. */
    bool bareMod = (h->mods & ~MOD_NOREPEAT) == 0 && IsModifierVk(h->vk);
    g_syntheticMods = true;
    for (int i = 0; i < n; i++) SendVkEvent(mods[i], true);
    g_syntheticMods = false;
    ExecRange(h->stmtStart, h->stmtCount);
    if (!bareMod) {
        g_syntheticMods = true;
        for (int i = n - 1; i >= 0; i--) SendVkEvent(mods[i], false);
        g_syntheticMods = false;
    }
}

/* Low-level keyboard hook: tracks left/right modifiers exactly, can suppress keys
 * the OS would otherwise handle (alt+tab, win+ctrl+c, F12), and supports AHK-style
 * short-press and release-based double-tap semantics. */
static bool HookModsHeld(UINT mods) {
    /* Exact match, like AHK: no extra modifiers are allowed to be held. */
    bool ctrl  = g_keyDown[VK_LCONTROL] || g_keyDown[VK_RCONTROL] || g_keyDown[VK_CONTROL];
    bool alt   = g_keyDown[VK_LMENU] || g_keyDown[VK_RMENU] || g_keyDown[VK_MENU];
    bool shift = g_keyDown[VK_LSHIFT] || g_keyDown[VK_RSHIFT] || g_keyDown[VK_SHIFT];
    bool win   = g_keyDown[VK_LWIN] || g_keyDown[VK_RWIN];
    if ((bool)(mods & MOD_CONTROL) != ctrl)  return false;
    if ((bool)(mods & MOD_ALT) != alt)       return false;
    if ((bool)(mods & MOD_SHIFT) != shift)   return false;
    if ((bool)(mods & MOD_WIN) != win)       return false;
    return true;
}

static bool IsModifierVk(UINT vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_MENU    || vk == VK_LMENU    || vk == VK_RMENU    ||
           vk == VK_SHIFT   || vk == VK_LSHIFT   || vk == VK_RSHIFT   ||
           vk == VK_LWIN    || vk == VK_RWIN;
}

static bool HookComboMatches(const Hotkey *h, UINT vk) {
    if (vk != h->vk) return false;
    UINT mods = h->mods & ~MOD_NOREPEAT;
    /* A bare modifier hotkey (rctrl = apps) matches the key itself; its own
     * down-state must not be treated as an extra modifier. */
    if (mods == 0 && IsModifierVk(h->vk)) return true;
    return HookModsHeld(mods);
}

/* A modifier used by a normal hotkey must not also count as a tap when released. */
static void MarkConsumedModifiers(UINT mods) {
    mods &= ~MOD_NOREPEAT;
    if (mods & MOD_CONTROL) {
        if (g_keyDown[VK_LCONTROL]) g_skipTapUp[VK_LCONTROL] = true;
        if (g_keyDown[VK_RCONTROL]) g_skipTapUp[VK_RCONTROL] = true;
    }
    if (mods & MOD_ALT) {
        if (g_keyDown[VK_LMENU]) g_skipTapUp[VK_LMENU] = true;
        if (g_keyDown[VK_RMENU]) g_skipTapUp[VK_RMENU] = true;
    }
    if (mods & MOD_SHIFT) {
        if (g_keyDown[VK_LSHIFT]) g_skipTapUp[VK_LSHIFT] = true;
        if (g_keyDown[VK_RSHIFT]) g_skipTapUp[VK_RSHIFT] = true;
    }
    if (mods & MOD_WIN) {
        if (g_keyDown[VK_LWIN]) g_skipTapUp[VK_LWIN] = true;
        if (g_keyDown[VK_RWIN]) g_skipTapUp[VK_RWIN] = true;
    }
}

/* Run actions on the message loop, never inside the hook callback: SendInput from
 * within a LL hook is dropped, and a blocking cmd would freeze all keyboard input. */
static void FireHotkeyAsync(const Hotkey *h) {
    if (g_hiddenWnd) PostMessageW(g_hiddenWnd, WM_APP + (UINT)h->id, 0, 0);
}

static LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg >= WM_APP && msg < WM_APP + MAX_IDS) {
        HandleHotkey((int)(msg - WM_APP));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool CreateHiddenWindow(void) {
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = HiddenWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"TinyBindyHookHost";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    g_hiddenWnd = CreateWindowExW(0, L"TinyBindyHookHost", L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    return g_hiddenWnd != NULL;
}

static LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (g_syntheticMods) return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
    if (code >= 0) {
        KBDLLHOOKSTRUCT *ks = (KBDLLHOOKSTRUCT *)lParam;
        UINT vk = ks->vkCode;
        bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
        if (vk < 256) {
            if (down) {
                g_keyDown[vk] = true;
                g_keyDownAt[vk] = GetTickCount64();
                for (int i = 0; i < g_hotkeyCount; i++) {
                    Hotkey *h = &g_hotkeys[i];
                    if (h->shortPress || h->onUp) continue;
                    if (HookComboMatches(h, vk)) {
                        MarkConsumedModifiers(h->mods);
                        FireHotkeyAsync(h);
                        if (!h->passThrough) {
                            /* Let modifier key-ups through so the OS clears its
                             * state; swallowing them leaves Ctrl stuck down. */
                            if (!IsModifierVk(vk)) g_suppressUp[vk] = true;
                            return 1;
                        }
                    }
                }
            } else if (up) {
                ULONGLONG now = GetTickCount64();
                ULONGLONG held = g_keyDown[vk] ? now - g_keyDownAt[vk] : 0;
                g_keyDown[vk] = false;
                bool consumed = g_skipTapUp[vk];
                if (consumed) g_skipTapUp[vk] = false;
                for (int i = 0; i < g_hotkeyCount && !consumed; i++) {
                    Hotkey *h = &g_hotkeys[i];
                    if (vk != h->vk) continue;
                    if (h->shortPress && held <= (ULONGLONG)h->tapMs) {
                        FireHotkeyAsync(h);
                    } else if (h->onUp) {
                        if (g_lastUp[h->id] && now - g_lastUp[h->id] <= (ULONGLONG)h->tapMs) {
                            g_lastUp[h->id] = 0;
                            FireHotkeyAsync(h);
                        } else {
                            g_lastUp[h->id] = now;
                        }
                    }
                }
                if (g_suppressUp[vk]) {
                    g_suppressUp[vk] = false;
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_running = false;
        if (g_wakeEvent) SetEvent(g_wakeEvent);
        return TRUE;
    }
    return FALSE;
}

int wmain(int argc, wchar_t **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    bool checkOnly = false;
    bool runOnce = false;
    bool badFlag = false;
    const wchar_t *script = NULL;
    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--check") == 0) checkOnly = true;
        else if (_wcsicmp(argv[i], L"--run-once") == 0) runOnce = true;
        else if (argv[i][0] != L'-' && argv[i][0] != L'/' && !script) script = argv[i];
        else if (argv[i][0] == L'-' || argv[i][0] == L'/') {
            Warn(L"bindy: unknown option '%ls'\n", argv[i]);
            badFlag = true;
        }
    }

    const CliOption opts[] = {
        { L"--check", CLI_OPT_BOOL, &checkOnly, 0, 0, NULL,
          L"Parse the script only; do not register hotkeys" },
        { L"--run-once", CLI_OPT_BOOL, &runOnce, 0, 0, NULL,
          L"Run on_start once and exit (diagnostic; no hotkeys)" },
    };
    if (TinyCLI_HasHelp(argc, argv)) {
        TinyCLI_FormatHelp(L"bindy",
                           L"Readable keymapper & hotkey scripting language for Windows",
                           L"bindy <script.bindy> [--check | --run-once]",
                           opts, (int)(sizeof(opts) / sizeof(opts[0])),
                           g_args[0], MAX_VALUE);
        TinyCLI_OutputHelp(L"bindy", g_args[0]);
        return 0;
    }
    if (badFlag) return 1;
    if (!script) {
        fprintf(stderr, "bindy: missing script path (use --help)\n");
        return 1;
    }
    TinyCLI_Parse(argc, argv, opts, (int)(sizeof(opts) / sizeof(opts[0])));

    if (!LoadScript(script)) return 1;
    if (!ParseScript()) return 1;

    wprintf(L"bindy: '%ls' - %d hotkey(s), %d function(s), %d statement(s)\n",
            script, g_hotkeyCount, g_funcCount, g_stmtCount);
    if (checkOnly) {
        wprintf(L"bindy: syntax check OK\n");
        for (int i = 0; i < g_hotkeyCount; i++) {
            Hotkey *h = &g_hotkeys[i];
            wprintf(L"  #%d  %-26ls%ls%ls%ls\n", i + 1, h->text,
                    h->passThrough ? L"  ~pass-through" : L"",
                    h->shortPress ? L"  short-press" : L"",
                    h->onUp ? L"  double-tap" : L"");
        }
        return 0;
    }
    if (runOnce) {
        ExecInit();
        ExecRange(g_startStart, g_startCount);
        wprintf(L"bindy: run-once complete\n");
        return 0;
    }

    if (!TinyIPC_AcquireOrToggle(APPMUTEX_NAME, APPEVENT_NAME, &g_toggleEvent)) {
        wprintf(L"bindy: toggle signal sent — running instance exits\n");
        return 0;
    }

    g_wakeEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    ExecInit();
    ExecRange(g_startStart, g_startCount);

    for (int i = 0; i < MAX_IDS; i++) g_idMap[i] = -1;
    for (int i = 0; i < g_hotkeyCount; i++) {
        g_hotkeys[i].id = i + 1;
        g_idMap[g_hotkeys[i].id] = i;
    }
    if (!CreateHiddenWindow()) {
        Warn(L"bindy: could not create hidden message window (error %lu)\n", GetLastError());
    }
    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc,
                                       GetModuleHandleW(NULL), 0);
    if (!g_keyboardHook) {
        Warn(L"bindy: could not install keyboard hook (error %lu) — hotkeys unavailable\n",
             GetLastError());
    }
    wprintf(L"bindy: running (low-level keyboard hook) — launch again or Ctrl+C to exit\n");

    HANDLE handles[2];
    handles[0] = g_toggleEvent;
    handles[1] = g_wakeEvent;
    while (g_running) {
        DWORD r = MsgWaitForMultipleObjects(2, handles, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0 || r == WAIT_OBJECT_0 + 1 || r == WAIT_FAILED) break;
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_keyboardHook) UnhookWindowsHookEx(g_keyboardHook);
    if (g_hiddenWnd) DestroyWindow(g_hiddenWnd);
    if (g_wakeEvent) CloseHandle(g_wakeEvent);
    if (g_toggleEvent) CloseHandle(g_toggleEvent);
    wprintf(L"bindy: exit\n");
    return 0;
}
