#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdbool.h>

#define MUTEX_NAME L"Global\\TinyContextMenuMutex"
#define EVENT_NAME L"Global\\TinyContextMenuEvent"
#define MANAGED_TAG L"context-menu"
#define HKCU_CLASSES_PATH L"Software\\Classes"

// Data Structures
typedef struct ContextMenuItem {
    wchar_t title[256];
    wchar_t icon[MAX_PATH];
    wchar_t command[MAX_PATH * 2];
    wchar_t arguments[MAX_PATH * 2];
    wchar_t key_name[128];

    struct ContextMenuItem** children;
    size_t child_count;
    size_t child_capacity;
} ContextMenuItem;

typedef struct ContextMenuTarget {
    wchar_t name[256];
    ContextMenuItem** items;
    size_t item_count;
    size_t item_capacity;
} ContextMenuTarget;

typedef struct ContextMenuConfig {
    ContextMenuTarget** targets;
    size_t target_count;
    size_t target_capacity;
} ContextMenuConfig;

// Globals
static HANDLE g_h_mutex = NULL;
static HANDLE g_h_event = NULL;
static bool g_cli_mode = false;
static wchar_t g_last_error[1024] = {0};

// Forward Declarations
static void set_error(const wchar_t* fmt, ...);
static void log_error(const wchar_t* msg);
static void log_info(const wchar_t* msg);

// Tree Allocators
static ContextMenuItem* item_create(const wchar_t* title) {
    ContextMenuItem* item = (ContextMenuItem*)calloc(1, sizeof(ContextMenuItem));
    if (!item) return NULL;
    if (title) wcscpy_s(item->title, 256, title);
    return item;
}

static void item_add_child(ContextMenuItem* parent, ContextMenuItem* child) {
    if (!parent || !child) return;
    if (parent->child_count >= parent->child_capacity) {
        size_t new_cap = parent->child_capacity == 0 ? 4 : parent->child_capacity * 2;
        ContextMenuItem** new_arr = (ContextMenuItem**)realloc(parent->children, new_cap * sizeof(ContextMenuItem*));
        if (!new_arr) return;
        parent->children = new_arr;
        parent->child_capacity = new_cap;
    }
    parent->children[parent->child_count++] = child;
}

static void item_free(ContextMenuItem* item) {
    if (!item) return;
    for (size_t i = 0; i < item->child_count; i++) {
        item_free(item->children[i]);
    }
    free(item->children);
    free(item);
}

static ContextMenuTarget* target_create(const wchar_t* name) {
    ContextMenuTarget* target = (ContextMenuTarget*)calloc(1, sizeof(ContextMenuTarget));
    if (!target) return NULL;
    if (name) wcscpy_s(target->name, 256, name);
    return target;
}

static void target_add_item(ContextMenuTarget* target, ContextMenuItem* item) {
    if (!target || !item) return;
    if (target->item_count >= target->item_capacity) {
        size_t new_cap = target->item_capacity == 0 ? 4 : target->item_capacity * 2;
        ContextMenuItem** new_arr = (ContextMenuItem**)realloc(target->items, new_cap * sizeof(ContextMenuItem*));
        if (!new_arr) return;
        target->items = new_arr;
        target->item_capacity = new_cap;
    }
    target->items[target->item_count++] = item;
}

static void target_free(ContextMenuTarget* target) {
    if (!target) return;
    for (size_t i = 0; i < target->item_count; i++) {
        item_free(target->items[i]);
    }
    free(target->items);
    free(target);
}

static ContextMenuConfig* config_create(void) {
    return (ContextMenuConfig*)calloc(1, sizeof(ContextMenuConfig));
}

static void config_add_target(ContextMenuConfig* config, ContextMenuTarget* target) {
    if (!config || !target) return;
    if (config->target_count >= config->target_capacity) {
        size_t new_cap = config->target_capacity == 0 ? 4 : config->target_capacity * 2;
        ContextMenuTarget** new_arr = (ContextMenuTarget**)realloc(config->targets, new_cap * sizeof(ContextMenuTarget*));
        if (!new_arr) return;
        config->targets = new_arr;
        config->target_capacity = new_cap;
    }
    config->targets[config->target_count++] = target;
}

static ContextMenuTarget* config_find_or_create_target(ContextMenuConfig* config, const wchar_t* name) {
    if (!config || !name) return NULL;
    for (size_t i = 0; i < config->target_count; i++) {
        if (_wcsicmp(config->targets[i]->name, name) == 0) {
            return config->targets[i];
        }
    }
    ContextMenuTarget* t = target_create(name);
    if (t) config_add_target(config, t);
    return t;
}

static void config_free(ContextMenuConfig* config) {
    if (!config) return;
    for (size_t i = 0; i < config->target_count; i++) {
        target_free(config->targets[i]);
    }
    free(config->targets);
    free(config);
}

// Error Handling Helpers
static void set_error(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vswprintf(g_last_error, sizeof(g_last_error) / sizeof(wchar_t), fmt, args);
    va_end(args);
}

static void log_error(const wchar_t* msg) {
    if (g_cli_mode) {
        fwprintf(stderr, L"Error: %s\n", msg);
        fflush(stderr);
    } else {
        MessageBoxW(NULL, msg, L"Context Menu Error", MB_ICONERROR | MB_OK);
    }
}

static void log_info(const wchar_t* msg) {
    if (g_cli_mode) {
        wprintf(L"%s\n", msg);
        fflush(stdout);
    } else {
        MessageBoxW(NULL, msg, L"Context Menu", MB_ICONINFORMATION | MB_OK);
    }
}

// Key Slug Generator
static void make_key_slug(const wchar_t* title, wchar_t* out_slug, size_t max_len) {
    size_t s = 0, d = 0;
    wcscpy_s(out_slug, max_len, L"TinyCM_");
    d = wcslen(out_slug);

    while (title[s] != L'\0' && d < max_len - 1) {
        wchar_t c = title[s++];
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) {
            out_slug[d++] = c;
        } else if (c == L' ' || c == L'_' || c == L'-') {
            if (d > 0 && out_slug[d - 1] != L'_') {
                out_slug[d++] = L'_';
            }
        }
    }
    while (d > 0 && out_slug[d - 1] == L'_') {
        d--;
    }
    out_slug[d] = L'\0';
    if (wcslen(out_slug) <= 7) {
        wcscat_s(out_slug, max_len, L"Item");
    }
}

// XML Escaping & Unescaping
static void xml_unescape(const wchar_t* src, wchar_t* dst, size_t dst_max) {
    size_t s = 0, d = 0;
    while (src[s] != L'\0' && d < dst_max - 1) {
        if (src[s] == L'&') {
            if (wcsncmp(&src[s], L"&amp;", 5) == 0) {
                dst[d++] = L'&'; s += 5;
            } else if (wcsncmp(&src[s], L"&lt;", 4) == 0) {
                dst[d++] = L'<'; s += 4;
            } else if (wcsncmp(&src[s], L"&gt;", 4) == 0) {
                dst[d++] = L'>'; s += 4;
            } else if (wcsncmp(&src[s], L"&quot;", 6) == 0) {
                dst[d++] = L'"'; s += 6;
            } else if (wcsncmp(&src[s], L"&apos;", 6) == 0) {
                dst[d++] = L'\''; s += 6;
            } else {
                dst[d++] = src[s++];
            }
        } else {
            dst[d++] = src[s++];
        }
    }
    dst[d] = L'\0';
}

static void xml_escape(const wchar_t* src, wchar_t* dst, size_t dst_max) {
    size_t s = 0, d = 0;
    while (src[s] != L'\0' && d < dst_max - 1) {
        if (src[s] == L'&' && d + 5 < dst_max) {
            wcscpy_s(&dst[d], dst_max - d, L"&amp;"); d += 5; s++;
        } else if (src[s] == L'<' && d + 4 < dst_max) {
            wcscpy_s(&dst[d], dst_max - d, L"&lt;"); d += 4; s++;
        } else if (src[s] == L'>' && d + 4 < dst_max) {
            wcscpy_s(&dst[d], dst_max - d, L"&gt;"); d += 4; s++;
        } else if (src[s] == L'"' && d + 6 < dst_max) {
            wcscpy_s(&dst[d], dst_max - d, L"&quot;"); d += 6; s++;
        } else if (src[s] == L'\'' && d + 6 < dst_max) {
            wcscpy_s(&dst[d], dst_max - d, L"&apos;"); d += 6; s++;
        } else {
            dst[d++] = src[s++];
        }
    }
    dst[d] = L'\0';
}

// XML File Reader
static wchar_t* xml_load_file(const wchar_t* filepath) {
    HANDLE h_file = CreateFileW(filepath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h_file == INVALID_HANDLE_VALUE) {
        set_error(L"Failed to open XML file: %s", filepath);
        return NULL;
    }

    DWORD file_size = GetFileSize(h_file, NULL);
    if (file_size == INVALID_FILE_SIZE || file_size == 0) {
        CloseHandle(h_file);
        set_error(L"XML file is empty or unreadable.");
        return NULL;
    }

    BYTE* raw_buf = (BYTE*)malloc(file_size + 2);
    if (!raw_buf) {
        CloseHandle(h_file);
        set_error(L"Out of memory allocating XML read buffer.");
        return NULL;
    }

    DWORD read_bytes = 0;
    if (!ReadFile(h_file, raw_buf, file_size, &read_bytes, NULL)) {
        free(raw_buf);
        CloseHandle(h_file);
        set_error(L"Failed to read XML file content.");
        return NULL;
    }
    CloseHandle(h_file);
    raw_buf[read_bytes] = 0;
    raw_buf[read_bytes + 1] = 0;

    wchar_t* wbuf = NULL;
    // Check BOM
    if (read_bytes >= 2 && raw_buf[0] == 0xFF && raw_buf[1] == 0xFE) {
        // UTF-16 LE
        size_t wchars = (read_bytes - 2) / sizeof(wchar_t);
        wbuf = (wchar_t*)malloc((wchars + 1) * sizeof(wchar_t));
        if (wbuf) {
            memcpy(wbuf, raw_buf + 2, wchars * sizeof(wchar_t));
            wbuf[wchars] = L'\0';
        }
    } else {
        // Assume UTF-8
        size_t offset = 0;
        if (read_bytes >= 3 && raw_buf[0] == 0xEF && raw_buf[1] == 0xBB && raw_buf[2] == 0xBF) {
            offset = 3;
        }
        int wlen = MultiByteToWideChar(CP_UTF8, 0, (char*)(raw_buf + offset), (int)(read_bytes - offset), NULL, 0);
        if (wlen > 0) {
            wbuf = (wchar_t*)malloc((wlen + 1) * sizeof(wchar_t));
            if (wbuf) {
                MultiByteToWideChar(CP_UTF8, 0, (char*)(raw_buf + offset), (int)(read_bytes - offset), wbuf, wlen);
                wbuf[wlen] = L'\0';
            }
        }
    }
    free(raw_buf);

    if (!wbuf) {
        set_error(L"Failed to decode XML file text.");
    }
    return wbuf;
}

// Lightweight Recursive XML Parser
typedef struct {
    const wchar_t* ptr;
    size_t line;
} XmlParser;

static void xml_skip_whitespace(XmlParser* p) {
    while (*p->ptr != L'\0') {
        if (*p->ptr == L'\n') {
            p->line++;
            p->ptr++;
        } else if (*p->ptr == L' ' || *p->ptr == L'\t' || *p->ptr == L'\r') {
            p->ptr++;
        } else if (wcsncmp(p->ptr, L"<!--", 4) == 0) {
            p->ptr += 4;
            while (*p->ptr != L'\0' && wcsncmp(p->ptr, L"-->", 3) != 0) {
                if (*p->ptr == L'\n') p->line++;
                p->ptr++;
            }
            if (*p->ptr != L'\0') p->ptr += 3;
        } else if (wcsncmp(p->ptr, L"<?", 2) == 0) {
            p->ptr += 2;
            while (*p->ptr != L'\0' && wcsncmp(p->ptr, L"?>", 2) != 0) {
                if (*p->ptr == L'\n') p->line++;
                p->ptr++;
            }
            if (*p->ptr != L'\0') p->ptr += 2;
        } else {
            break;
        }
    }
}

static bool xml_parse_attribute(XmlParser* p, wchar_t* attr_name, size_t name_max, wchar_t* attr_val, size_t val_max) {
    xml_skip_whitespace(p);
    if (*p->ptr == L'>' || *p->ptr == L'/' || *p->ptr == L'\0') return false;

    size_t n = 0;
    while (*p->ptr != L'\0' && *p->ptr != L'=' && *p->ptr != L' ' && *p->ptr != L'\t' && *p->ptr != L'>' && *p->ptr != L'/') {
        if (n < name_max - 1) attr_name[n++] = *p->ptr;
        p->ptr++;
    }
    attr_name[n] = L'\0';
    xml_skip_whitespace(p);

    if (*p->ptr != L'=') return false;
    p->ptr++; // skip '='
    xml_skip_whitespace(p);

    wchar_t quote = L'\0';
    if (*p->ptr == L'"' || *p->ptr == L'\'') {
        quote = *p->ptr++;
    }

    wchar_t raw_val[1024] = {0};
    size_t v = 0;
    while (*p->ptr != L'\0') {
        if (quote != L'\0') {
            if (*p->ptr == quote) { p->ptr++; break; }
        } else {
            if (*p->ptr == L' ' || *p->ptr == L'\t' || *p->ptr == L'>' || *p->ptr == L'/') break;
        }
        if (v < sizeof(raw_val)/sizeof(wchar_t) - 1) raw_val[v++] = *p->ptr;
        p->ptr++;
    }
    raw_val[v] = L'\0';
    xml_unescape(raw_val, attr_val, val_max);
    return true;
}

static void xml_parse_element_children(XmlParser* p, const wchar_t* parent_tag, ContextMenuItem* current_item, ContextMenuTarget* current_target, ContextMenuConfig* config) {
    while (*p->ptr != L'\0') {
        xml_skip_whitespace(p);
        if (*p->ptr == L'\0') break;

        if (wcsncmp(p->ptr, L"</", 2) == 0) {
            // Closing tag
            p->ptr += 2;
            wchar_t close_name[128] = {0};
            size_t n = 0;
            while (*p->ptr != L'\0' && *p->ptr != L'>' && *p->ptr != L' ') {
                if (n < 127) close_name[n++] = *p->ptr;
                p->ptr++;
            }
            close_name[n] = L'\0';
            while (*p->ptr != L'\0' && *p->ptr != L'>') p->ptr++;
            if (*p->ptr == L'>') p->ptr++;
            break;
        } else if (*p->ptr == L'<') {
            p->ptr++; // skip '<'
            wchar_t tag_name[128] = {0};
            size_t n = 0;
            while (*p->ptr != L'\0' && *p->ptr != L' ' && *p->ptr != L'\t' && *p->ptr != L'\r' && *p->ptr != L'\n' && *p->ptr != L'>' && *p->ptr != L'/') {
                if (n < 127) tag_name[n++] = *p->ptr;
                p->ptr++;
            }
            tag_name[n] = L'\0';

            wchar_t attr_name[128], attr_val[512];
            wchar_t item_title[256] = {0}, item_icon[MAX_PATH] = {0}, item_cmd[MAX_PATH*2] = {0}, item_args[MAX_PATH*2] = {0}, target_name[256] = {0};

            while (xml_parse_attribute(p, attr_name, 128, attr_val, 512)) {
                if (_wcsicmp(attr_name, L"title") == 0 || _wcsicmp(attr_name, L"name") == 0) wcscpy_s(item_title, 256, attr_val);
                if (_wcsicmp(attr_name, L"icon") == 0) wcscpy_s(item_icon, MAX_PATH, attr_val);
                if (_wcsicmp(attr_name, L"command") == 0 || _wcsicmp(attr_name, L"cmd") == 0) wcscpy_s(item_cmd, MAX_PATH*2, attr_val);
                if (_wcsicmp(attr_name, L"arguments") == 0 || _wcsicmp(attr_name, L"args") == 0) wcscpy_s(item_args, MAX_PATH*2, attr_val);
                if (_wcsicmp(attr_name, L"target") == 0) wcscpy_s(target_name, 256, attr_val);
            }

            xml_skip_whitespace(p);
            bool is_self_closing = false;
            if (*p->ptr == L'/') {
                is_self_closing = true;
                p->ptr++;
            }
            if (*p->ptr == L'>') p->ptr++;

            if (_wcsicmp(tag_name, L"target") == 0) {
                wchar_t tname[256] = {0};
                wcscpy_s(tname, 256, target_name[0] != L'\0' ? target_name : item_title);
                if (tname[0] == L'\0') wcscpy_s(tname, 256, L"*");
                ContextMenuTarget* tgt = config_find_or_create_target(config, tname);
                if (!is_self_closing) {
                    xml_parse_element_children(p, L"target", NULL, tgt, config);
                }
            } else if (_wcsicmp(tag_name, L"item") == 0) {
                ContextMenuItem* child_item = item_create(item_title);
                if (item_icon[0] != L'\0') wcscpy_s(child_item->icon, MAX_PATH, item_icon);
                if (item_cmd[0] != L'\0') wcscpy_s(child_item->command, MAX_PATH*2, item_cmd);
                if (item_args[0] != L'\0') wcscpy_s(child_item->arguments, MAX_PATH*2, item_args);

                if (!is_self_closing) {
                    xml_parse_element_children(p, L"item", child_item, current_target, config);
                }

                if (current_item) {
                    item_add_child(current_item, child_item);
                } else if (current_target) {
                    target_add_item(current_target, child_item);
                } else {
                    ContextMenuTarget* tgt = config_find_or_create_target(config, L"*");
                    target_add_item(tgt, child_item);
                }
            } else if (_wcsicmp(tag_name, L"title") == 0 || _wcsicmp(tag_name, L"icon") == 0 ||
                       _wcsicmp(tag_name, L"command") == 0 || _wcsicmp(tag_name, L"arguments") == 0) {
                // Read text content
                wchar_t text_buf[1024] = {0};
                size_t tlen = 0;
                while (*p->ptr != L'\0' && *p->ptr != L'<') {
                    if (tlen < 1023) text_buf[tlen++] = *p->ptr;
                    p->ptr++;
                }
                text_buf[tlen] = L'\0';
                wchar_t unescaped[1024] = {0};
                xml_unescape(text_buf, unescaped, 1024);

                if (current_item) {
                    if (_wcsicmp(tag_name, L"title") == 0) wcscpy_s(current_item->title, 256, unescaped);
                    else if (_wcsicmp(tag_name, L"icon") == 0) wcscpy_s(current_item->icon, MAX_PATH, unescaped);
                    else if (_wcsicmp(tag_name, L"command") == 0) wcscpy_s(current_item->command, MAX_PATH*2, unescaped);
                    else if (_wcsicmp(tag_name, L"arguments") == 0) wcscpy_s(current_item->arguments, MAX_PATH*2, unescaped);
                }

                // Skip closing tag
                if (*p->ptr == L'<') {
                    p->ptr++;
                    while (*p->ptr != L'\0' && *p->ptr != L'>') p->ptr++;
                    if (*p->ptr == L'>') p->ptr++;
                }
            } else {
                // Unknown tag, skip
                if (!is_self_closing) {
                    xml_parse_element_children(p, tag_name, NULL, NULL, config);
                }
            }
        } else {
            // Text outside tags, skip
            p->ptr++;
        }
    }
}

static ContextMenuConfig* xml_parse_config(const wchar_t* xml_str) {
    if (!xml_str) return NULL;
    ContextMenuConfig* config = config_create();
    XmlParser p = { xml_str, 1 };

    xml_skip_whitespace(&p);
    if (*p.ptr == L'<') {
        // Find root element
        xml_parse_element_children(&p, L"", NULL, NULL, config);
    }
    return config;
}

// Complete Validation
static bool item_validate_recursive(const ContextMenuItem* item, const wchar_t* target_name) {
    if (!item) return false;
    if (item->title[0] == L'\0') {
        set_error(L"Validation Error: An item under target '%s' is missing a title.", target_name);
        return false;
    }
    if (item->child_count == 0) {
        if (item->command[0] == L'\0') {
            set_error(L"Validation Error: Item '%s' under target '%s' has no command specified.", item->title, target_name);
            return false;
        }
    } else {
        for (size_t i = 0; i < item->child_count; i++) {
            if (!item_validate_recursive(item->children[i], target_name)) {
                return false;
            }
        }
    }
    return true;
}

static bool config_validate(const ContextMenuConfig* config) {
    if (!config) {
        set_error(L"Validation Error: Configuration object is null.");
        return false;
    }
    for (size_t i = 0; i < config->target_count; i++) {
        ContextMenuTarget* target = config->targets[i];
        if (target->name[0] == L'\0') {
            set_error(L"Validation Error: Context menu target name cannot be empty.");
            return false;
        }
        for (size_t j = 0; j < target->item_count; j++) {
            if (!item_validate_recursive(target->items[j], target->name)) {
                return false;
            }
        }
    }
    return true;
}

// Registry Helper: Write Item Recursively
static bool registry_write_item(HKEY h_shell_key, ContextMenuItem* item, int index, bool is_top_level) {
    wchar_t subkey_name[128];
    make_key_slug(item->title, subkey_name, 128);
    if (wcslen(item->key_name) > 0) {
        wcscpy_s(subkey_name, 128, item->key_name);
    } else {
        wsprintfW(item->key_name, L"%s_%d", subkey_name, index);
        wcscpy_s(subkey_name, 128, item->key_name);
    }

    HKEY h_item_key = NULL;
    LONG res = RegCreateKeyExW(h_shell_key, subkey_name, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &h_item_key, NULL);
    if (res != ERROR_SUCCESS) {
        set_error(L"Failed to create registry key for item '%s'.", item->title);
        return false;
    }

    // Set Title
    RegSetValueExW(h_item_key, NULL, 0, REG_SZ, (BYTE*)item->title, (DWORD)((wcslen(item->title) + 1) * sizeof(wchar_t)));
    RegSetValueExW(h_item_key, L"MUIVerb", 0, REG_SZ, (BYTE*)item->title, (DWORD)((wcslen(item->title) + 1) * sizeof(wchar_t)));

    // Tag managed item if top level
    if (is_top_level) {
        RegSetValueExW(h_item_key, L"ManagedBy", 0, REG_SZ, (BYTE*)MANAGED_TAG, (DWORD)((wcslen(MANAGED_TAG) + 1) * sizeof(wchar_t)));
    }

    // Icon
    if (item->icon[0] != L'\0') {
        RegSetValueExW(h_item_key, L"Icon", 0, REG_SZ, (BYTE*)item->icon, (DWORD)((wcslen(item->icon) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(h_item_key, L"Icon");
    }

    if (item->child_count == 0) {
        // Action item
        RegDeleteValueW(h_item_key, L"SubCommands");
        RegDeleteTreeW(h_item_key, L"shell");

        HKEY h_cmd_key = NULL;
        res = RegCreateKeyExW(h_item_key, L"command", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &h_cmd_key, NULL);
        if (res == ERROR_SUCCESS) {
            wchar_t full_cmd[MAX_PATH * 4];
            if (item->arguments[0] != L'\0') {
                wsprintfW(full_cmd, L"%s %s", item->command, item->arguments);
            } else {
                wsprintfW(full_cmd, L"%s", item->command);
            }
            RegSetValueExW(h_cmd_key, NULL, 0, REG_SZ, (BYTE*)full_cmd, (DWORD)((wcslen(full_cmd) + 1) * sizeof(wchar_t)));
            RegCloseKey(h_cmd_key);
        }
    } else {
        // Submenu item
        RegDeleteTreeW(h_item_key, L"command");
        const wchar_t* empty_str = L"";
        RegSetValueExW(h_item_key, L"SubCommands", 0, REG_SZ, (BYTE*)empty_str, (DWORD)(sizeof(wchar_t)));

        HKEY h_sub_shell = NULL;
        res = RegCreateKeyExW(h_item_key, L"shell", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &h_sub_shell, NULL);
        if (res == ERROR_SUCCESS) {
            for (size_t i = 0; i < item->child_count; i++) {
                registry_write_item(h_sub_shell, item->children[i], (int)(i + 1), false);
            }
            RegCloseKey(h_sub_shell);
        }
    }

    RegCloseKey(h_item_key);
    return true;
}

// Registry Clean & Update Engine
static bool registry_update(const ContextMenuConfig* config) {
    HKEY h_classes = NULL;
    LONG res = RegOpenKeyExW(HKEY_CURRENT_USER, HKCU_CLASSES_PATH, 0, KEY_ALL_ACCESS, &h_classes);
    if (res != ERROR_SUCCESS) {
        set_error(L"Failed to open HKCU\\Software\\Classes.");
        return false;
    }

    // Phase 1: Purge existing managed entries not in config or all managed entries
    DWORD class_idx = 0;
    wchar_t target_name[256];
    DWORD target_len = 256;

    while (target_len = 256, RegEnumKeyExW(h_classes, class_idx++, target_name, &target_len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        wchar_t shell_path[512];
        wsprintfW(shell_path, L"%s\\shell", target_name);

        HKEY h_shell = NULL;
        if (RegOpenKeyExW(h_classes, shell_path, 0, KEY_ALL_ACCESS, &h_shell) == ERROR_SUCCESS) {
            DWORD item_idx = 0;
            wchar_t item_key_name[256];
            DWORD item_key_len = 256;

            wchar_t keys_to_delete[64][256];
            size_t delete_count = 0;

            while (item_key_len = 256, RegEnumKeyExW(h_shell, item_idx++, item_key_name, &item_key_len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                HKEY h_item = NULL;
                if (RegOpenKeyExW(h_shell, item_key_name, 0, KEY_READ, &h_item) == ERROR_SUCCESS) {
                    wchar_t managed_buf[64] = {0};
                    DWORD managed_len = sizeof(managed_buf);
                    if (RegQueryValueExW(h_item, L"ManagedBy", NULL, NULL, (BYTE*)managed_buf, &managed_len) == ERROR_SUCCESS) {
                        if (_wcsicmp(managed_buf, MANAGED_TAG) == 0) {
                            if (delete_count < 64) {
                                wcscpy_s(keys_to_delete[delete_count++], 256, item_key_name);
                            }
                        }
                    }
                    RegCloseKey(h_item);
                }
            }

            for (size_t i = 0; i < delete_count; i++) {
                RegDeleteTreeW(h_shell, keys_to_delete[i]);
            }
            RegCloseKey(h_shell);
        }
    }

    // Phase 2: Apply new config
    for (size_t i = 0; i < config->target_count; i++) {
        ContextMenuTarget* target = config->targets[i];
        wchar_t shell_path[512];
        wsprintfW(shell_path, L"%s\\shell", target->name);

        HKEY h_target_shell = NULL;
        res = RegCreateKeyExW(h_classes, shell_path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &h_target_shell, NULL);
        if (res == ERROR_SUCCESS) {
            for (size_t j = 0; j < target->item_count; j++) {
                registry_write_item(h_target_shell, target->items[j], (int)(j + 1), true);
            }
            RegCloseKey(h_target_shell);
        }
    }

    RegCloseKey(h_classes);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return true;
}

// Registry Backup Engine
static ContextMenuItem* registry_read_item(HKEY h_item_key, const wchar_t* item_name) {
    wchar_t title[256] = {0};
    DWORD title_bytes = sizeof(title);
    if (RegQueryValueExW(h_item_key, L"MUIVerb", NULL, NULL, (BYTE*)title, &title_bytes) != ERROR_SUCCESS) {
        title_bytes = sizeof(title);
        RegQueryValueExW(h_item_key, NULL, NULL, NULL, (BYTE*)title, &title_bytes);
    }
    if (title[0] == L'\0') {
        wcscpy_s(title, 256, item_name);
    }

    ContextMenuItem* item = item_create(title);
    wcscpy_s(item->key_name, 128, item_name);

    // Icon
    DWORD icon_bytes = sizeof(item->icon);
    RegQueryValueExW(h_item_key, L"Icon", NULL, NULL, (BYTE*)item->icon, &icon_bytes);

    // Command subkey
    HKEY h_cmd = NULL;
    if (RegOpenKeyExW(h_item_key, L"command", 0, KEY_READ, &h_cmd) == ERROR_SUCCESS) {
        wchar_t raw_cmd[MAX_PATH * 4] = {0};
        DWORD cmd_bytes = sizeof(raw_cmd);
        if (RegQueryValueExW(h_cmd, NULL, NULL, NULL, (BYTE*)raw_cmd, &cmd_bytes) == ERROR_SUCCESS) {
            wcscpy_s(item->command, MAX_PATH * 2, raw_cmd);
        }
        RegCloseKey(h_cmd);
    }

    // Submenu shell subkey
    HKEY h_sub_shell = NULL;
    if (RegOpenKeyExW(h_item_key, L"shell", 0, KEY_READ, &h_sub_shell) == ERROR_SUCCESS) {
        DWORD idx = 0;
        wchar_t child_key_name[256];
        DWORD child_len = 256;

        while (child_len = 256, RegEnumKeyExW(h_sub_shell, idx++, child_key_name, &child_len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            HKEY h_child = NULL;
            if (RegOpenKeyExW(h_sub_shell, child_key_name, 0, KEY_READ, &h_child) == ERROR_SUCCESS) {
                ContextMenuItem* child_item = registry_read_item(h_child, child_key_name);
                if (child_item) {
                    item_add_child(item, child_item);
                }
                RegCloseKey(h_child);
            }
        }
        RegCloseKey(h_sub_shell);
    }

    return item;
}

static ContextMenuConfig* registry_backup_read(void) {
    ContextMenuConfig* config = config_create();

    HKEY h_classes = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, HKCU_CLASSES_PATH, 0, KEY_READ, &h_classes) != ERROR_SUCCESS) {
        return config;
    }

    DWORD class_idx = 0;
    wchar_t target_name[256];
    DWORD target_len = 256;
    size_t total_found = 0;

    // Pass 1: Managed entries with ManagedBy tag
    while (target_len = 256, RegEnumKeyExW(h_classes, class_idx++, target_name, &target_len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        wchar_t shell_path[512];
        wsprintfW(shell_path, L"%s\\shell", target_name);

        HKEY h_shell = NULL;
        if (RegOpenKeyExW(h_classes, shell_path, 0, KEY_READ, &h_shell) == ERROR_SUCCESS) {
            DWORD item_idx = 0;
            wchar_t item_key_name[256];
            DWORD item_key_len = 256;

            while (item_key_len = 256, RegEnumKeyExW(h_shell, item_idx++, item_key_name, &item_key_len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                HKEY h_item = NULL;
                if (RegOpenKeyExW(h_shell, item_key_name, 0, KEY_READ, &h_item) == ERROR_SUCCESS) {
                    wchar_t managed_buf[64] = {0};
                    DWORD managed_len = sizeof(managed_buf);
                    if (RegQueryValueExW(h_item, L"ManagedBy", NULL, NULL, (BYTE*)managed_buf, &managed_len) == ERROR_SUCCESS) {
                        if (_wcsicmp(managed_buf, MANAGED_TAG) == 0) {
                            ContextMenuItem* item = registry_read_item(h_item, item_key_name);
                            if (item) {
                                ContextMenuTarget* current_target = config_find_or_create_target(config, target_name);
                                target_add_item(current_target, item);
                                total_found++;
                            }
                        }
                    }
                    RegCloseKey(h_item);
                }
            }
            RegCloseKey(h_shell);
        }
    }

    // Pass 2: Fallback scan for any custom HKCU shell entries if no ManagedBy entries were tagged yet
    if (total_found == 0) {
        class_idx = 0;
        while (target_len = 256, RegEnumKeyExW(h_classes, class_idx++, target_name, &target_len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            wchar_t shell_path[512];
            wsprintfW(shell_path, L"%s\\shell", target_name);

            HKEY h_shell = NULL;
            if (RegOpenKeyExW(h_classes, shell_path, 0, KEY_READ, &h_shell) == ERROR_SUCCESS) {
                DWORD item_idx = 0;
                wchar_t item_key_name[256];
                DWORD item_key_len = 256;

                while (item_key_len = 256, RegEnumKeyExW(h_shell, item_idx++, item_key_name, &item_key_len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                    HKEY h_item = NULL;
                    if (RegOpenKeyExW(h_shell, item_key_name, 0, KEY_READ, &h_item) == ERROR_SUCCESS) {
                        HKEY h_cmd = NULL, h_subshell = NULL;
                        bool has_command = (RegOpenKeyExW(h_item, L"command", 0, KEY_READ, &h_cmd) == ERROR_SUCCESS);
                        if (h_cmd) RegCloseKey(h_cmd);

                        bool has_subshell = (RegOpenKeyExW(h_item, L"shell", 0, KEY_READ, &h_subshell) == ERROR_SUCCESS);
                        if (h_subshell) RegCloseKey(h_subshell);

                        if (has_command || has_subshell) {
                            ContextMenuItem* item = registry_read_item(h_item, item_key_name);
                            if (item) {
                                ContextMenuTarget* current_target = config_find_or_create_target(config, target_name);
                                target_add_item(current_target, item);
                                total_found++;
                            }
                        }
                        RegCloseKey(h_item);
                    }
                }
                RegCloseKey(h_shell);
            }
        }
    }

    RegCloseKey(h_classes);
    return config;
}

static void write_utf8(FILE* fp, const wchar_t* wstr) {
    if (!wstr || !fp) return;
    char utf8_buf[2048] = {0};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8_buf, sizeof(utf8_buf), NULL, NULL);
    if (len > 0) {
        fputs(utf8_buf, fp);
    }
}

// XML Generator for Backup
static void xml_generate_item(FILE* fp, ContextMenuItem* item, int indent) {
    for (int i = 0; i < indent && i < 30; i++) fputs("\t", fp);

    wchar_t esc_title[512] = {0}, esc_icon[MAX_PATH * 2] = {0}, esc_cmd[MAX_PATH * 4] = {0}, esc_args[MAX_PATH * 4] = {0};
    xml_escape(item->title, esc_title, 512);
    if (item->icon[0] != L'\0') xml_escape(item->icon, esc_icon, MAX_PATH * 2);
    if (item->command[0] != L'\0') xml_escape(item->command, esc_cmd, MAX_PATH * 4);
    if (item->arguments[0] != L'\0') xml_escape(item->arguments, esc_args, MAX_PATH * 4);

    fputs("<item title=\"", fp);
    write_utf8(fp, esc_title);
    fputs("\"", fp);

    if (esc_icon[0] != L'\0') {
        fputs(" icon=\"", fp);
        write_utf8(fp, esc_icon);
        fputs("\"", fp);
    }

    if (item->child_count == 0) {
        if (esc_args[0] == L'\0') {
            fputs(" command=\"", fp);
            write_utf8(fp, esc_cmd);
            fputs("\" />\n", fp);
        } else {
            fputs(">\n", fp);
            for (int i = 0; i <= indent; i++) fputs("\t", fp);
            fputs("<command>", fp);
            write_utf8(fp, esc_cmd);
            fputs("</command>\n", fp);

            for (int i = 0; i <= indent; i++) fputs("\t", fp);
            fputs("<arguments>", fp);
            write_utf8(fp, esc_args);
            fputs("</arguments>\n", fp);

            for (int i = 0; i < indent; i++) fputs("\t", fp);
            fputs("</item>\n", fp);
        }
    } else {
        fputs(">\n", fp);
        for (size_t i = 0; i < item->child_count; i++) {
            xml_generate_item(fp, item->children[i], indent + 1);
        }
        for (int i = 0; i < indent; i++) fputs("\t", fp);
        fputs("</item>\n", fp);
    }
}

static bool xml_save_backup(const ContextMenuConfig* config, const wchar_t* filepath) {
    FILE* fp = NULL;
    if (_wfopen_s(&fp, filepath, L"wb") != 0 || !fp) {
        set_error(L"Failed to open destination file for backup: %s", filepath);
        return false;
    }

    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", fp);
    fputs("<context-menu>\n", fp);

    for (size_t i = 0; i < config->target_count; i++) {
        ContextMenuTarget* target = config->targets[i];
        wchar_t esc_tname[512] = {0};
        xml_escape(target->name, esc_tname, 512);

        fputs("\t<target name=\"", fp);
        write_utf8(fp, esc_tname);
        fputs("\">\n", fp);

        for (size_t j = 0; j < target->item_count; j++) {
            xml_generate_item(fp, target->items[j], 2);
        }
        fputs("\t</target>\n", fp);
    }

    fputs("</context-menu>\n", fp);
    fclose(fp);
    return true;
}

// Update Flow Action
static bool do_update(const wchar_t* xml_file) {
    wchar_t full_path[MAX_PATH];
    if (GetFullPathNameW(xml_file, MAX_PATH, full_path, NULL) == 0) {
        wcscpy_s(full_path, MAX_PATH, xml_file);
    }
    wchar_t* xml_text = xml_load_file(full_path);
    if (!xml_text) return false;

    ContextMenuConfig* config = xml_parse_config(xml_text);
    free(xml_text);

    if (!config) {
        set_error(L"Failed to parse XML configuration file.");
        return false;
    }

    if (!config_validate(config)) {
        config_free(config);
        return false;
    }

    bool res = registry_update(config);
    config_free(config);

    return res;
}

// Backup Flow Action
static bool do_backup(const wchar_t* backup_file) {
    wchar_t full_path[MAX_PATH];
    if (GetFullPathNameW(backup_file, MAX_PATH, full_path, NULL) == 0) {
        wcscpy_s(full_path, MAX_PATH, backup_file);
    }
    ContextMenuConfig* config = registry_backup_read();
    bool res = xml_save_backup(config, full_path);
    config_free(config);
    return res;
}

// Native TaskDialog / Choice Dialog for Double-Click
static int show_main_choice_dialog(void) {
    HMODULE h_comctl = LoadLibraryW(L"comctl32.dll");
    typedef HRESULT (WINAPI *PFN_TaskDialogIndirect)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    PFN_TaskDialogIndirect pfn_task_dialog = NULL;
    if (h_comctl) {
        pfn_task_dialog = (PFN_TaskDialogIndirect)GetProcAddress(h_comctl, "TaskDialogIndirect");
    }

    if (pfn_task_dialog) {
        TASKDIALOG_BUTTON buttons[] = {
            { 101, L"Update Context Menu\nApply configuration from XML file to Windows Registry" },
            { 102, L"Backup Context Menu\nExport managed context menu entries to XML backup file" }
        };
        TASKDIALOGCONFIG tdc = { sizeof(tdc) };
        tdc.hwndParent = NULL;
        tdc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        tdc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        tdc.pszWindowTitle = L"Context Menu Utility";
        tdc.pszMainInstruction = L"Context Menu Manager";
        tdc.pszContent = L"Select an operation:";
        tdc.pButtons = buttons;
        tdc.cButtons = 2;

        int button_id = 0;
        HRESULT hr = pfn_task_dialog(&tdc, &button_id, NULL, NULL);
        FreeLibrary(h_comctl);

        if (SUCCEEDED(hr)) {
            if (button_id == 101) return 1;
            if (button_id == 102) return 2;
            return 0;
        }
    }

    // Fallback native MessageBox choice dialog
    int res = MessageBoxW(NULL,
        L"Click 'Yes' to Update Context Menu.\nClick 'No' to Backup Context Menu.\nClick 'Cancel' to exit.",
        L"Context Menu Utility",
        MB_YESNOCANCEL | MB_ICONQUESTION);

    if (res == IDYES) return 1;
    if (res == IDNO) return 2;
    return 0;
}

// Native File Open / Save Dialogs
static bool gui_select_open_file(wchar_t* out_path, DWORD max_path) {
    wchar_t filename[MAX_PATH] = {0};
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.lpstrFilter = L"XML Files (*.xml)\0*.xml\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Select Context Menu XML Configuration";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (GetOpenFileNameW(&ofn)) {
        wcscpy_s(out_path, max_path, filename);
        return true;
    }
    return false;
}

static bool gui_select_save_file(wchar_t* out_path, DWORD max_path) {
    wchar_t filename[MAX_PATH] = L"context-menu-backup.xml";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.lpstrFilter = L"XML Files (*.xml)\0*.xml\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Save Context Menu XML Backup";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"xml";

    if (GetSaveFileNameW(&ofn)) {
        wcscpy_s(out_path, max_path, filename);
        return true;
    }
    return false;
}

// Entry Point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;

    // CLI Arguments Check
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argc > 1) {
        g_cli_mode = true;
        AttachConsole(ATTACH_PARENT_PROCESS);

        wchar_t* action_mode = NULL;
        wchar_t* file_arg = NULL;

        for (int i = 1; i < argc; i++) {
            if (_wcsicmp(argv[i], L"--update") == 0 || _wcsicmp(argv[i], L"-u") == 0) {
                action_mode = L"update";
                if (i + 1 < argc) file_arg = argv[++i];
            } else if (_wcsicmp(argv[i], L"--backup") == 0 || _wcsicmp(argv[i], L"-b") == 0) {
                action_mode = L"backup";
                if (i + 1 < argc) file_arg = argv[++i];
            }
        }

        if (!action_mode || !file_arg) {
            fwprintf(stderr, L"Usage:\n");
            fwprintf(stderr, L"  context-menu.exe --update <config.xml>\n");
            fwprintf(stderr, L"  context-menu.exe --backup <backup.xml>\n");
            LocalFree(argv);
            return 1;
        }

        bool success = false;
        if (_wcsicmp(action_mode, L"update") == 0) {
            success = do_update(file_arg);
            if (!success) {
                log_error(g_last_error[0] ? g_last_error : L"Failed to update context menu.");
            }
        } else if (_wcsicmp(action_mode, L"backup") == 0) {
            success = do_backup(file_arg);
            if (!success) {
                log_error(g_last_error[0] ? g_last_error : L"Failed to backup context menu.");
            }
        }

        LocalFree(argv);
        return success ? 0 : 1;
    }

    LocalFree(argv);

    // Double-Click Flow (GUI mode) — Single Instance Mutex Check
    g_h_mutex = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (g_h_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        g_h_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, EVENT_NAME);
        if (g_h_event) {
            SetEvent(g_h_event);
            CloseHandle(g_h_event);
        }
        CloseHandle(g_h_mutex);
        return 0;
    }
    g_h_event = CreateEventW(NULL, FALSE, FALSE, EVENT_NAME);

    g_cli_mode = false;
    int choice = show_main_choice_dialog();

    if (choice == 1) {
        // Update GUI flow
        wchar_t open_path[MAX_PATH] = {0};
        if (gui_select_open_file(open_path, MAX_PATH)) {
            if (!do_update(open_path)) {
                log_error(g_last_error[0] ? g_last_error : L"Failed to update context menu.");
            }
        }
    } else if (choice == 2) {
        // Backup GUI flow
        wchar_t save_path[MAX_PATH] = {0};
        if (gui_select_save_file(save_path, MAX_PATH)) {
            if (!do_backup(save_path)) {
                log_error(g_last_error[0] ? g_last_error : L"Failed to backup context menu.");
            }
        }
    }

    if (g_h_event) CloseHandle(g_h_event);
    if (g_h_mutex) CloseHandle(g_h_mutex);
    return 0;
}
