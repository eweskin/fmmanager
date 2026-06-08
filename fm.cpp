#include <windows.h>
#include <wininet.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <cwctype>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

// ============================================================
//  DATA STRUCTURES & GLOBAL STATE
// ============================================================
struct Entry {
    std::wstring name;
    bool isDir;
    unsigned long long size;
    FILETIME ctime;
    FILETIME mtime;
};

struct Panel {
    std::wstring path;
    std::vector<Entry> entries;
    int cursor;
    int top;
    bool detailed;
};

struct MarkItem {
    bool isDir;
    std::wstring fullPath;
};

static HANDLE g_out;
static HANDLE g_in;
static int g_w = 0;
static int g_h = 0;
static int g_lastW = -1;
static int g_lastH = -1;
static bool g_needClear = true;
static std::vector<CHAR_INFO> g_buf;

static void markDirty() { g_needClear = true; }

static int accelStep(WORD dir) {
    static DWORD lastTime = 0;
    static WORD lastDir = 0;
    static int streak = 0;
    DWORD now = GetTickCount();
    if (dir == lastDir && now - lastTime < 110) streak++;
    else streak = 0;
    lastTime = now;
    lastDir = dir;
    const int THRESH = 18;
    if (streak < THRESH) return 1;
    int s = streak - THRESH;
    int step = 1 + (s * s) / 6;
    if (step > 40) step = 40;
    return step;
}

static Panel g_panel[2];
static int g_active = 0;
static std::vector<MarkItem> g_marks;

static int markIndex(const std::wstring &full) {
    for (int i = 0; i < (int)g_marks.size(); i++)
        if (_wcsicmp(g_marks[i].fullPath.c_str(), full.c_str()) == 0) return i;
    return -1;
}

static bool isMarked(const std::wstring &full) { return markIndex(full) >= 0; }

static void toggleMark(const std::wstring &full, bool isDir) {
    int i = markIndex(full);
    if (i >= 0) g_marks.erase(g_marks.begin() + i);
    else g_marks.push_back({ isDir, full });
}

static void unmark(const std::wstring &full) {
    int i = markIndex(full);
    if (i >= 0) g_marks.erase(g_marks.begin() + i);
}

// ============================================================
//  COLOR INDICES & ATTRIBUTES
// ============================================================
enum {
    CI_BG = 0, CI_FRAME, CI_DIM, CI_FG, CI_ACCENT, CI_DIR, CI_MARK,
    CI_CURBG, CI_CURFG, CI_HDRBG, CI_HDRFG, CI_STBG, CI_STFG,
    CI_ACCENT2, CI_SPARE, CI_WHITE,
    CI_SYKW, CI_SYSTR, CI_SYCOM, CI_SYNUM, CI_SYTYPE, CI_SYPRE, CI_SYFUNC, CI_SYOP,
    CI_COUNT
};

#define ATTR(fg, bg) ((WORD)(((fg) & 0xFF) | (((bg) & 0xFF) << 8)))

static const WORD A_NORMAL   = ATTR(CI_FG, CI_BG);
static const WORD A_DIM      = ATTR(CI_DIM, CI_BG);
static const WORD A_DIR      = ATTR(CI_DIR, CI_BG);
static const WORD A_MARK     = ATTR(CI_MARK, CI_BG);
static const WORD A_FRAME    = ATTR(CI_FRAME, CI_BG);
static const WORD A_ACCENT   = ATTR(CI_ACCENT, CI_BG);
static const WORD A_HEADER   = ATTR(CI_HDRFG, CI_HDRBG);
static const WORD A_CURSOR   = ATTR(CI_CURFG, CI_CURBG);
static const WORD A_CURSORIN = ATTR(CI_ACCENT2, CI_CURBG);
static const WORD A_CURMARK  = ATTR(CI_MARK, CI_CURBG);
static const WORD A_STATUS   = ATTR(CI_STFG, CI_STBG);

static const WORD A_SYKW   = ATTR(CI_SYKW, CI_BG);
static const WORD A_SYSTR  = ATTR(CI_SYSTR, CI_BG);
static const WORD A_SYCOM  = ATTR(CI_SYCOM, CI_BG);
static const WORD A_SYNUM  = ATTR(CI_SYNUM, CI_BG);
static const WORD A_SYTYPE = ATTR(CI_SYTYPE, CI_BG);
static const WORD A_SYPRE  = ATTR(CI_SYPRE, CI_BG);
static const WORD A_SYFUNC = ATTR(CI_SYFUNC, CI_BG);
static const WORD A_SYOP   = ATTR(CI_SYOP, CI_BG);

// ============================================================
//  THEMES & PALETTES
// ============================================================
struct Theme {
    const wchar_t *name;
    COLORREF pal[CI_COUNT];
};

static const Theme g_themes[] = {
    { L"Default", {
        RGB(0x1F,0x1E,0x1D), RGB(0x4A,0x47,0x44), RGB(0x90,0x8C,0x86), RGB(0xE6,0xE2,0xDB),
        RGB(0xD9,0x77,0x57), RGB(0x8F,0xB7,0xE0), RGB(0xE5,0xC0,0x7B), RGB(0x34,0x32,0x2F),
        RGB(0xFF,0xFF,0xFF), RGB(0x2A,0x28,0x26), RGB(0xC0,0x84,0x57), RGB(0xD9,0x77,0x57),
        RGB(0x1F,0x1E,0x1D), RGB(0xB8,0xD4,0xF0), RGB(0x80,0x80,0x80), RGB(0xFF,0xFF,0xFF),
        RGB(0xD9,0x88,0x5F), RGB(0x98,0xC3,0x79), RGB(0x6E,0x6A,0x64), RGB(0xD6,0xA9,0x5C),
        RGB(0x56,0xB6,0xC2), RGB(0xC6,0x78,0xDD), RGB(0x61,0xAF,0xEF), RGB(0xB0,0xAB,0xA3) } },
    { L"Midnight", {
        RGB(0x11,0x14,0x1C), RGB(0x2E,0x38,0x50), RGB(0x6B,0x7A,0x99), RGB(0xD6,0xDE,0xEC),
        RGB(0x58,0xA6,0xFF), RGB(0x7E,0xE0,0xC8), RGB(0xF2,0xC5,0x5C), RGB(0x1E,0x27,0x40),
        RGB(0xFF,0xFF,0xFF), RGB(0x16,0x1B,0x27), RGB(0x58,0xA6,0xFF), RGB(0x58,0xA6,0xFF),
        RGB(0x0B,0x0E,0x14), RGB(0x9C,0xE6,0xD6), RGB(0x80,0x80,0x80), RGB(0xFF,0xFF,0xFF),
        RGB(0xFF,0x7B,0x72), RGB(0xA5,0xD6,0xA7), RGB(0x5C,0x67,0x73), RGB(0xF2,0xC5,0x5C),
        RGB(0x79,0xC0,0xFF), RGB(0xD2,0xA8,0xFF), RGB(0x58,0xA6,0xFF), RGB(0x9D,0xA5,0xB3) } },
    { L"Matrix", {
        RGB(0x05,0x0A,0x05), RGB(0x1B,0x3A,0x1B), RGB(0x3E,0x8E,0x3E), RGB(0x6C,0xFF,0x6C),
        RGB(0x00,0xFF,0x66), RGB(0xB6,0xFF,0x7A), RGB(0xDF,0xFF,0x50), RGB(0x0E,0x2A,0x0E),
        RGB(0xCF,0xFF,0xCF), RGB(0x0A,0x1A,0x0A), RGB(0x00,0xFF,0x66), RGB(0x00,0xCC,0x44),
        RGB(0x02,0x10,0x02), RGB(0xB6,0xFF,0x7A), RGB(0x80,0x80,0x80), RGB(0xDF,0xFF,0xDF),
        RGB(0x7C,0xFF,0x7C), RGB(0xB6,0xFF,0x7A), RGB(0x2E,0x7D,0x32), RGB(0xDF,0xFF,0x50),
        RGB(0x50,0xFF,0xB0), RGB(0xA0,0xFF,0xA0), RGB(0x00,0xFF,0x66), RGB(0x7F,0xBF,0x7F) } },
    { L"Dracula", {
        RGB(0x28,0x2A,0x36), RGB(0x44,0x47,0x5A), RGB(0x62,0x72,0xA4), RGB(0xF8,0xF8,0xF2),
        RGB(0xBD,0x93,0xF9), RGB(0x8B,0xE9,0xFD), RGB(0xF1,0xFA,0x8C), RGB(0x3A,0x3D,0x52),
        RGB(0xFF,0xFF,0xFF), RGB(0x21,0x22,0x2C), RGB(0xFF,0x79,0xC6), RGB(0xBD,0x93,0xF9),
        RGB(0x21,0x22,0x2C), RGB(0x8B,0xE9,0xFD), RGB(0x80,0x80,0x80), RGB(0xFF,0xFF,0xFF),
        RGB(0xFF,0x79,0xC6), RGB(0xF1,0xFA,0x8C), RGB(0x62,0x72,0xA4), RGB(0xBD,0x93,0xF9),
        RGB(0x8B,0xE9,0xFD), RGB(0xFF,0xB8,0x6C), RGB(0x50,0xFA,0x7B), RGB(0xF8,0xF8,0xF2) } },
    { L"Paper", {
        RGB(0xF4,0xF1,0xEA), RGB(0xC9,0xC2,0xB4), RGB(0x7A,0x74,0x68), RGB(0x2E,0x2A,0x24),
        RGB(0xC2,0x41,0x0C), RGB(0x1D,0x6F,0xB8), RGB(0xA6,0x63,0x00), RGB(0xE2,0xD9,0xC6),
        RGB(0x1A,0x17,0x12), RGB(0xE8,0xE2,0xD5), RGB(0xC2,0x41,0x0C), RGB(0xC2,0x41,0x0C),
        RGB(0xF4,0xF1,0xEA), RGB(0x1D,0x6F,0xB8), RGB(0x80,0x80,0x80), RGB(0x2E,0x2A,0x24),
        RGB(0xA6,0x26,0xA4), RGB(0x50,0xA1,0x4F), RGB(0xA0,0xA1,0xA7), RGB(0x98,0x68,0x01),
        RGB(0xC1,0x84,0x01), RGB(0x40,0x78,0xF2), RGB(0x40,0x78,0xF2), RGB(0x38,0x3A,0x42) } },
    { L"Black", {
        RGB(0x00,0x00,0x00), RGB(0x2A,0x2A,0x2A), RGB(0x6E,0x6E,0x6E), RGB(0xE8,0xE8,0xE8),
        RGB(0xFF,0xB0,0x00), RGB(0x66,0xCC,0xFF), RGB(0xFF,0xD2,0x4C), RGB(0x1C,0x1C,0x1C),
        RGB(0xFF,0xFF,0xFF), RGB(0x0C,0x0C,0x0C), RGB(0xFF,0xB0,0x00), RGB(0xFF,0xB0,0x00),
        RGB(0x00,0x00,0x00), RGB(0x66,0xCC,0xFF), RGB(0x80,0x80,0x80), RGB(0xFF,0xFF,0xFF),
        RGB(0xFF,0xB0,0x00), RGB(0x9E,0xD0,0x6C), RGB(0x5E,0x5E,0x5E), RGB(0xE0,0xC0,0x80),
        RGB(0x66,0xCC,0xFF), RGB(0xC0,0x90,0xFF), RGB(0x7F,0xD0,0xFF), RGB(0xC8,0xC8,0xC8) } },
    { L"Snow", {
        RGB(0xFF,0xFF,0xFF), RGB(0xD4,0xD4,0xD4), RGB(0x8A,0x8A,0x8A), RGB(0x20,0x20,0x20),
        RGB(0x25,0x63,0xEB), RGB(0x7C,0x3A,0xED), RGB(0xB4,0x53,0x09), RGB(0xE3,0xEA,0xF6),
        RGB(0x0A,0x0A,0x0A), RGB(0xEF,0xEF,0xEF), RGB(0x25,0x63,0xEB), RGB(0x25,0x63,0xEB),
        RGB(0xFF,0xFF,0xFF), RGB(0x7C,0x3A,0xED), RGB(0x80,0x80,0x80), RGB(0x00,0x00,0x00),
        RGB(0xAF,0x00,0xDB), RGB(0x0A,0x7D,0x33), RGB(0x9A,0x9A,0x9A), RGB(0x98,0x68,0x01),
        RGB(0x09,0x69,0xDA), RGB(0x25,0x63,0xEB), RGB(0x25,0x63,0xEB), RGB(0x38,0x3A,0x42) } },
    { L"Mono", {
        RGB(0x12,0x12,0x12), RGB(0x3A,0x3A,0x3A), RGB(0x77,0x77,0x77), RGB(0xDD,0xDD,0xDD),
        RGB(0xE8,0x91,0x3A), RGB(0xED,0xED,0xED), RGB(0xE8,0x91,0x3A), RGB(0x2E,0x2E,0x2E),
        RGB(0xFF,0xFF,0xFF), RGB(0x1E,0x1E,0x1E), RGB(0xE8,0x91,0x3A), RGB(0xE8,0x91,0x3A),
        RGB(0x12,0x12,0x12), RGB(0xFF,0xFF,0xFF), RGB(0x80,0x80,0x80), RGB(0xFF,0xFF,0xFF),
        RGB(0xE8,0x91,0x3A), RGB(0xAA,0xAA,0xAA), RGB(0x66,0x66,0x66), RGB(0xCC,0xCC,0xCC),
        RGB(0xBB,0xBB,0xBB), RGB(0xE8,0x91,0x3A), RGB(0xEE,0xEE,0xEE), RGB(0x99,0x99,0x99) } },
    { L"Solar Dark", {
        RGB(0x00,0x2B,0x36), RGB(0x07,0x36,0x42), RGB(0x58,0x6E,0x75), RGB(0x93,0xA1,0xA1),
        RGB(0xB5,0x89,0x00), RGB(0x26,0x8B,0xD2), RGB(0x2A,0xA1,0x98), RGB(0x07,0x36,0x42),
        RGB(0xFD,0xF6,0xE3), RGB(0x00,0x2B,0x36), RGB(0x85,0x99,0x00), RGB(0xB5,0x89,0x00),
        RGB(0x00,0x2B,0x36), RGB(0x2A,0xA1,0x98), RGB(0x80,0x80,0x80), RGB(0xFD,0xF6,0xE3),
        RGB(0x85,0x99,0x00), RGB(0x2A,0xA1,0x98), RGB(0x58,0x6E,0x75), RGB(0xD3,0x36,0x82),
        RGB(0xB5,0x89,0x00), RGB(0xCB,0x4B,0x16), RGB(0x26,0x8B,0xD2), RGB(0x93,0xA1,0xA1) } },
    { L"Solar Light", {
        RGB(0xFD,0xF6,0xE3), RGB(0xEE,0xE8,0xD5), RGB(0x93,0xA1,0xA1), RGB(0x58,0x6E,0x75),
        RGB(0xB5,0x89,0x00), RGB(0x26,0x8B,0xD2), RGB(0xD3,0x36,0x82), RGB(0xEE,0xE8,0xD5),
        RGB(0x00,0x2B,0x36), RGB(0xFD,0xF6,0xE3), RGB(0x85,0x99,0x00), RGB(0xB5,0x89,0x00),
        RGB(0xFD,0xF6,0xE3), RGB(0x2A,0xA1,0x98), RGB(0x80,0x80,0x80), RGB(0x00,0x2B,0x36),
        RGB(0x85,0x99,0x00), RGB(0x2A,0xA1,0x98), RGB(0x93,0xA1,0xA1), RGB(0xD3,0x36,0x82),
        RGB(0xB5,0x89,0x00), RGB(0xCB,0x4B,0x16), RGB(0x26,0x8B,0xD2), RGB(0x58,0x6E,0x75) } },
    { L"Nord", {
        RGB(0x2E,0x34,0x40), RGB(0x3B,0x42,0x52), RGB(0x61,0x6E,0x88), RGB(0xD8,0xDE,0xE9),
        RGB(0x88,0xC0,0xD0), RGB(0x81,0xA1,0xC1), RGB(0xEB,0xCB,0x8B), RGB(0x3B,0x42,0x52),
        RGB(0xEC,0xEF,0xF4), RGB(0x2E,0x34,0x40), RGB(0x88,0xC0,0xD0), RGB(0x88,0xC0,0xD0),
        RGB(0x2E,0x34,0x40), RGB(0x8F,0xBC,0xBB), RGB(0x80,0x80,0x80), RGB(0xEC,0xEF,0xF4),
        RGB(0x81,0xA1,0xC1), RGB(0xA3,0xBE,0x8C), RGB(0x61,0x6E,0x88), RGB(0xB4,0x8E,0xAD),
        RGB(0x8F,0xBC,0xBB), RGB(0x5E,0x81,0xAC), RGB(0x88,0xC0,0xD0), RGB(0xD8,0xDE,0xE9) } },
    { L"Gruvbox", {
        RGB(0x28,0x28,0x28), RGB(0x3C,0x38,0x36), RGB(0x92,0x83,0x74), RGB(0xEB,0xDB,0xB2),
        RGB(0xFE,0x80,0x19), RGB(0x83,0xA5,0x98), RGB(0xFA,0xBD,0x2F), RGB(0x3C,0x38,0x36),
        RGB(0xFB,0xF1,0xC7), RGB(0x1D,0x20,0x21), RGB(0xFE,0x80,0x19), RGB(0xFE,0x80,0x19),
        RGB(0x28,0x28,0x28), RGB(0x8E,0xC0,0x7C), RGB(0x80,0x80,0x80), RGB(0xFB,0xF1,0xC7),
        RGB(0xFB,0x49,0x34), RGB(0xB8,0xBB,0x26), RGB(0x92,0x83,0x74), RGB(0xD3,0x86,0x9B),
        RGB(0xFA,0xBD,0x2F), RGB(0x8E,0xC0,0x7C), RGB(0xB8,0xBB,0x26), RGB(0xEB,0xDB,0xB2) } },
    { L"Tokyo Night", {
        RGB(0x1A,0x1B,0x26), RGB(0x2F,0x35,0x49), RGB(0x56,0x5F,0x89), RGB(0xC0,0xCA,0xF5),
        RGB(0x7A,0xA2,0xF7), RGB(0x7D,0xCF,0xFF), RGB(0xE0,0xAF,0x68), RGB(0x29,0x2E,0x42),
        RGB(0xC0,0xCA,0xF5), RGB(0x16,0x16,0x1E), RGB(0x7A,0xA2,0xF7), RGB(0x7A,0xA2,0xF7),
        RGB(0x1A,0x1B,0x26), RGB(0xBB,0x9A,0xF7), RGB(0x80,0x80,0x80), RGB(0xC0,0xCA,0xF5),
        RGB(0xBB,0x9A,0xF7), RGB(0x9E,0xCE,0x6A), RGB(0x56,0x5F,0x89), RGB(0xFF,0x9E,0x64),
        RGB(0x2A,0xC3,0xDE), RGB(0x7D,0xCF,0xFF), RGB(0x7A,0xA2,0xF7), RGB(0xC0,0xCA,0xF5) } },
    { L"Monokai", {
        RGB(0x27,0x28,0x22), RGB(0x3E,0x3D,0x32), RGB(0x75,0x71,0x5E), RGB(0xF8,0xF8,0xF2),
        RGB(0xFD,0x97,0x1F), RGB(0x66,0xD9,0xEF), RGB(0xE6,0xDB,0x74), RGB(0x3E,0x3D,0x32),
        RGB(0xF8,0xF8,0xF2), RGB(0x1E,0x1F,0x1C), RGB(0xA6,0xE2,0x2E), RGB(0xFD,0x97,0x1F),
        RGB(0x27,0x28,0x22), RGB(0xAE,0x81,0xFF), RGB(0x80,0x80,0x80), RGB(0xF8,0xF8,0xF2),
        RGB(0xF9,0x26,0x72), RGB(0xE6,0xDB,0x74), RGB(0x75,0x71,0x5E), RGB(0xAE,0x81,0xFF),
        RGB(0x66,0xD9,0xEF), RGB(0xA6,0xE2,0x2E), RGB(0xA6,0xE2,0x2E), RGB(0xF8,0xF8,0xF2) } },
};
static const int g_themeCount = (int)(sizeof(g_themes) / sizeof(g_themes[0]));
static int g_theme = 0;

static COLORREF g_activePal[CI_COUNT];

struct MonoHue { const wchar_t *name; COLORREF color; };
static const MonoHue g_monoHues[] = {
    { L"Orange", RGB(0xE8,0x91,0x3A) },
    { L"Coral",  RGB(0xE0,0x67,0x4C) },
    { L"Red",    RGB(0xE0,0x52,0x4C) },
    { L"Amber",  RGB(0xE5,0xB8,0x4C) },
    { L"Green",  RGB(0x5F,0xC8,0x5F) },
    { L"Teal",   RGB(0x3F,0xC9,0xB0) },
    { L"Cyan",   RGB(0x45,0xB8,0xD8) },
    { L"Blue",   RGB(0x5A,0x8F,0xE0) },
    { L"Purple", RGB(0xB0,0x73,0xE0) },
    { L"Pink",   RGB(0xE0,0x70,0xC0) },
    { L"White",  RGB(0xE8,0xE8,0xE8) },
};
static const int g_monoHueCount = (int)(sizeof(g_monoHues) / sizeof(g_monoHues[0]));
static int g_monoHue = 0;

static const wchar_t *g_foxMini[] = {
    L" /\\_/\\ ",
    L"( o.o )",
    L" > ^ < "
};
static const int g_foxMiniN = (int)(sizeof(g_foxMini) / sizeof(g_foxMini[0]));

static std::vector<std::wstring> g_art;

// ============================================================
//  CONSOLE BUFFER & RENDER PRIMITIVES
// ============================================================
static void queryConsole() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(g_out, &csbi);
    g_w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    g_h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    if (g_w < 40) g_w = 40;
    if (g_h < 10) g_h = 10;
    if (g_w != g_lastW || g_h != g_lastH) { g_needClear = true; g_lastW = g_w; g_lastH = g_h; }
    g_buf.assign((size_t)g_w * g_h, CHAR_INFO{});
    for (auto &c : g_buf) { c.Char.UnicodeChar = L' '; c.Attributes = A_NORMAL; }
}

static void putCh(int x, int y, wchar_t ch, WORD attr) {
    if (x < 0 || y < 0 || x >= g_w || y >= g_h) return;
    CHAR_INFO &c = g_buf[(size_t)y * g_w + x];
    c.Char.UnicodeChar = ch;
    c.Attributes = attr;
}

static void putStr(int x, int y, const std::wstring &s, WORD attr, int maxw) {
    int n = 0;
    for (wchar_t ch : s) {
        if (n >= maxw) break;
        putCh(x + n, y, ch, attr);
        n++;
    }
}

static void fillRow(int x, int y, int width, wchar_t ch, WORD attr) {
    for (int i = 0; i < width; i++) putCh(x + i, y, ch, attr);
}

static void flush() {
    static std::wstring out;
    out.clear();
    out.reserve((size_t)g_w * g_h * 6 + 64);
    const COLORREF *pal = g_activePal;
    int curFg = -1, curBg = -1;
    wchar_t esc[48];
    if (g_needClear) {
        COLORREF b = pal[CI_BG];
        swprintf(esc, 48, L"\x1b[48;2;%d;%d;%dm", GetRValue(b), GetGValue(b), GetBValue(b));
        out += esc;
        out += L"\x1b[2J";
        curBg = CI_BG;
        g_needClear = false;
    }
    out += L"\x1b[H";
    for (int y = 0; y < g_h; y++) {
        swprintf(esc, 48, L"\x1b[%d;1H", y + 1);
        out += esc;
        for (int x = 0; x < g_w; x++) {
            CHAR_INFO &c = g_buf[(size_t)y * g_w + x];
            int fi = c.Attributes & 0xFF;
            int bi = (c.Attributes >> 8) & 0xFF;
            if (fi != curFg) {
                COLORREF f = pal[fi];
                swprintf(esc, 48, L"\x1b[38;2;%d;%d;%dm", GetRValue(f), GetGValue(f), GetBValue(f));
                out += esc;
                curFg = fi;
            }
            if (bi != curBg) {
                COLORREF b = pal[bi];
                swprintf(esc, 48, L"\x1b[48;2;%d;%d;%dm", GetRValue(b), GetGValue(b), GetBValue(b));
                out += esc;
                curBg = bi;
            }
            wchar_t ch = c.Char.UnicodeChar ? c.Char.UnicodeChar : L' ';
            out += ch;
        }
    }
    out += L"\x1b[0m";
    DWORD written;
    WriteConsoleW(g_out, out.data(), (DWORD)out.size(), &written, NULL);
}

static void dimBuf() {
    for (auto &c : g_buf) c.Attributes = ATTR(CI_DIM, CI_BG);
}

static void clearBuf() {
    for (auto &c : g_buf) { c.Char.UnicodeChar = L' '; c.Attributes = A_NORMAL; }
}

static void drawBox(int x0, int x1, int y0, int y1, WORD attr, const std::wstring &title) {
    putCh(x0, y0, L'\x256D', attr); putCh(x1, y0, L'\x256E', attr);
    putCh(x0, y1, L'\x2570', attr); putCh(x1, y1, L'\x256F', attr);
    for (int x = x0 + 1; x < x1; x++) { putCh(x, y0, L'\x2500', attr); putCh(x, y1, L'\x2500', attr); }
    for (int y = y0 + 1; y < y1; y++) { putCh(x0, y, L'\x2502', attr); putCh(x1, y, L'\x2502', attr); }
    if (!title.empty()) putStr(x0 + 2, y0, title, attr, x1 - x0 - 3);
}

// ============================================================
//  PALETTE / THEME APPLY & CONFIG PERSISTENCE
// ============================================================
static void buildPalette() {
    if (wcscmp(g_themes[g_theme].name, L"Mono") == 0) {
        COLORREF a = g_monoHues[g_monoHue].color;
        COLORREF *p = g_activePal;
        p[CI_BG]    = RGB(0x12,0x12,0x12); p[CI_FRAME]  = RGB(0x3A,0x3A,0x3A);
        p[CI_DIM]   = RGB(0x77,0x77,0x77); p[CI_FG]     = RGB(0xDD,0xDD,0xDD);
        p[CI_ACCENT]= a;                   p[CI_DIR]    = RGB(0xED,0xED,0xED);
        p[CI_MARK]  = a;                   p[CI_CURBG]  = RGB(0x2E,0x2E,0x2E);
        p[CI_CURFG] = RGB(0xFF,0xFF,0xFF); p[CI_HDRBG]  = RGB(0x1E,0x1E,0x1E);
        p[CI_HDRFG] = a;                   p[CI_STBG]   = a;
        p[CI_STFG]  = RGB(0x12,0x12,0x12); p[CI_ACCENT2]= RGB(0xFF,0xFF,0xFF);
        p[CI_SPARE] = RGB(0x80,0x80,0x80); p[CI_WHITE]  = RGB(0xFF,0xFF,0xFF);
        p[CI_SYKW]  = a;                   p[CI_SYSTR]  = RGB(0xAA,0xAA,0xAA);
        p[CI_SYCOM] = RGB(0x66,0x66,0x66); p[CI_SYNUM]  = RGB(0xCC,0xCC,0xCC);
        p[CI_SYTYPE]= RGB(0xBB,0xBB,0xBB); p[CI_SYPRE]  = a;
        p[CI_SYFUNC]= RGB(0xEE,0xEE,0xEE); p[CI_SYOP]   = RGB(0x99,0x99,0x99);
    } else {
        for (int i = 0; i < CI_COUNT; i++) g_activePal[i] = g_themes[g_theme].pal[i];
    }
}

static void applyTheme(int t) {
    if (t >= 0 && t < g_themeCount) g_theme = t;
    buildPalette();
}

static std::wstring cfgPath() {
    wchar_t *la = _wgetenv(L"LOCALAPPDATA");
    std::wstring d = la ? la : L".";
    d += L"\\FoxFM";
    CreateDirectoryW(d.c_str(), NULL);
    return d + L"\\theme.txt";
}

static void saveTheme() {
    HANDLE h = CreateFileW(cfgPath().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[32];
    int n = sprintf(buf, "%d %d", g_theme, g_monoHue);
    DWORD w;
    WriteFile(h, buf, n, &w, NULL);
    CloseHandle(h);
}

static void loadTheme() {
    HANDLE h = CreateFileW(cfgPath().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[32] = { 0 };
    DWORD r;
    ReadFile(h, buf, 31, &r, NULL);
    CloseHandle(h);
    int t = 0, hue = 0;
    if (sscanf(buf, "%d %d", &t, &hue) >= 1) {
        if (t >= 0 && t < g_themeCount) g_theme = t;
        if (hue >= 0 && hue < g_monoHueCount) g_monoHue = hue;
    }
}

// ============================================================
//  PATH & STRING HELPERS
// ============================================================
static bool isDriveRoot(const std::wstring &p) {
    return p.size() == 3 && p[1] == L':' && p[2] == L'\\';
}

static std::wstring joinPath(const std::wstring &dir, const std::wstring &name) {
    if (!dir.empty() && dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

static std::wstring baseName(const std::wstring &p) {
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return p;
    return p.substr(pos + 1);
}

static std::wstring parentDir(const std::wstring &p) {
    std::wstring s = p;
    if (s.size() > 3 && s.back() == L'\\') s.pop_back();
    size_t pos = s.find_last_of(L'\\');
    if (pos == std::wstring::npos) return s;
    if (pos <= 2) return s.substr(0, 3);
    return s.substr(0, pos);
}

static std::wstring winErr(DWORD code) {
    LPWSTR msg = NULL;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&msg, 0, NULL);
    std::wstring s;
    if (n && msg) {
        s.assign(msg, n);
        while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ' || s.back() == L'.'))
            s.pop_back();
    }
    if (msg) LocalFree(msg);
    if (s.empty()) s = L"error " + std::to_wstring(code);
    return s;
}

static std::wstring sanitizeFileName(const std::wstring &in) {
    std::wstring base = baseName(in);
    std::wstring out;
    for (wchar_t c : base) {
        if (c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' ||
            c == L'\\' || c == L'|' || c == L'?' || c == L'*' || c < 32)
            continue;
        out += c;
    }
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
    size_t firstGood = out.find_first_not_of(L". ");
    if (firstGood != std::wstring::npos) out = out.substr(firstGood);
    if (out.empty() || out == L"." || out == L"..") out = L"download.bin";
    if (out.size() > 200) out = out.substr(0, 200);
    return out;
}

// ============================================================
//  DIRECTORY LISTING & FORMATTING
// ============================================================
static void loadDir(Panel &p) {
    p.entries.clear();
    if (!isDriveRoot(p.path)) {
        Entry up;
        up.name = L"..";
        up.isDir = true;
        up.size = 0;
        up.ctime = FILETIME{};
        up.mtime = FILETIME{};
        p.entries.push_back(up);
    }
    std::wstring mask = joinPath(p.path, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(mask.c_str(), &fd);
    std::vector<Entry> dirs, files;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            Entry e;
            e.name = name;
            e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            e.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            e.ctime = fd.ftCreationTime;
            e.mtime = fd.ftLastWriteTime;
            if (e.isDir) dirs.push_back(e); else files.push_back(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    auto cmp = [](const Entry &a, const Entry &b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    };
    std::sort(dirs.begin(), dirs.end(), cmp);
    std::sort(files.begin(), files.end(), cmp);
    for (auto &e : dirs) p.entries.push_back(e);
    for (auto &e : files) p.entries.push_back(e);
    if (p.cursor >= (int)p.entries.size()) p.cursor = (int)p.entries.size() - 1;
    if (p.cursor < 0) p.cursor = 0;
    p.top = 0;
}

static std::wstring fmtSize(unsigned long long s) {
    wchar_t buf[32];
    if (s < 1024ULL) swprintf(buf, 32, L"%llu B", s);
    else if (s < 1024ULL * 1024) swprintf(buf, 32, L"%.1f K", s / 1024.0);
    else if (s < 1024ULL * 1024 * 1024) swprintf(buf, 32, L"%.1f M", s / (1024.0 * 1024));
    else swprintf(buf, 32, L"%.1f G", s / (1024.0 * 1024 * 1024));
    return buf;
}

static std::wstring fmtDate(const FILETIME &ft) {
    SYSTEMTIME st;
    FILETIME local;
    if (!FileTimeToLocalFileTime(&ft, &local)) return L"";
    if (!FileTimeToSystemTime(&local, &st)) return L"";
    wchar_t buf[16];
    swprintf(buf, 16, L"%02d.%02d.%04d", st.wDay, st.wMonth, st.wYear);
    return buf;
}

static std::wstring shortPath(const std::wstring &p, int maxw) {
    if (maxw < 1) return L"";
    if ((int)p.size() <= maxw) return p;
    int keep = maxw - 3;
    if (keep < 1) keep = 1;
    return L"\x2026" + p.substr(p.size() - keep);
}

// ============================================================
//  UI RENDERING (panels, bars, main frame)
// ============================================================
static void renderPanel(int idx, int x0, int x1, int y0, int y1) {
    Panel &p = g_panel[idx];
    bool act = (idx == g_active);
    WORD b = act ? A_ACCENT : A_FRAME;
    int innerX = x0 + 1;
    int innerW = x1 - x0 - 1;

    drawBox(x0, x1, y0, y1, b, L"");
    std::wstring dot = act ? L" \x25CF " : L" \x25CB ";
    std::wstring title = dot + shortPath(p.path, innerW - 6) + L" ";
    putStr(x0 + 2, y0, title, act ? A_ACCENT : A_DIM, innerW - 2);

    int hy = y0 + 1;
    fillRow(innerX, hy, innerW, L' ', A_HEADER);
    int sizeX = x1 - 33, createdX = x1 - 23, modX = x1 - 11;
    bool detail = p.detailed && innerW > 42;
    if (detail) {
        putStr(innerX + 1, hy, L"Name", A_HEADER, innerW);
        putStr(sizeX, hy, L"Size", A_HEADER, 8);
        putStr(createdX, hy, L"Created", A_HEADER, 10);
        putStr(modX, hy, L"Modified", A_HEADER, 10);
    } else {
        putStr(innerX + 1, hy, L"Name", A_HEADER, innerW - 1);
    }

    int listTop = y0 + 2;
    int listH = y1 - listTop;
    if (p.cursor < p.top) p.top = p.cursor;
    if (p.cursor >= p.top + listH) p.top = p.cursor - listH + 1;
    if (p.top < 0) p.top = 0;

    for (int row = 0; row < listH; row++) {
        int ei = p.top + row;
        int y = listTop + row;
        if (ei >= (int)p.entries.size()) continue;

        Entry &e = p.entries[ei];
        bool isCur = act && (ei == p.cursor);
        std::wstring full = joinPath(p.path, e.name);
        bool isMark = isMarked(full);

        WORD attr;
        if (isCur) attr = isMark ? A_CURMARK : (e.isDir ? A_CURSORIN : A_CURSOR);
        else if (isMark) attr = A_MARK;
        else attr = e.isDir ? A_DIR : A_NORMAL;

        fillRow(innerX, y, innerW, L' ', attr);
        wchar_t mk = isMark ? L'\x25C6' : (isCur ? L'\x25B8' : L' ');
        putCh(innerX, y, mk, attr);

        std::wstring nm = e.name;
        if (e.isDir && e.name != L"..") nm = e.name + L"\\";

        if (detail && e.name != L"..") {
            int nameW = sizeX - (innerX + 1) - 1;
            if (nameW < 4) nameW = 4;
            putStr(innerX + 1, y, nm, attr, nameW);
            WORD dcol = isCur ? attr : A_DIM;
            if (!e.isDir) putStr(sizeX, y, fmtSize(e.size), attr, 8);
            else putStr(sizeX, y, L"<DIR>", attr, 8);
            putStr(createdX, y, fmtDate(e.ctime), dcol, 10);
            putStr(modX, y, fmtDate(e.mtime), dcol, 10);
        } else {
            putStr(innerX + 1, y, nm, attr, innerW - 1);
        }
    }

    wchar_t cnt[40];
    int files = 0, dirs = 0;
    for (auto &e : p.entries) { if (e.name == L"..") continue; if (e.isDir) dirs++; else files++; }

    if (dirs == 0 && files == 0 && listH > g_foxMiniN + 2) {
        int fy = listTop + (listH - g_foxMiniN - 1) / 2;
        for (int k = 0; k < g_foxMiniN; k++) {
            std::wstring fl = g_foxMini[k];
            int fx = innerX + (innerW - (int)fl.size()) / 2;
            putStr(fx, fy + k, fl, A_DIM, innerW);
        }
        std::wstring em = L"(empty)";
        putStr(innerX + (innerW - (int)em.size()) / 2, fy + g_foxMiniN, em, A_DIM, innerW);
    }

    swprintf(cnt, 40, L" %d dirs, %d files ", dirs, files);
    std::wstring c = cnt;
    int cx = x1 - 2 - (int)c.size();
    if (cx > x0 + 2) putStr(cx, y1, c, b, (int)c.size());
}

static void drawTopBar() {
    fillRow(0, 0, g_w, L' ', A_NORMAL);
    putStr(1, 0, L"Fox File Manager", A_ACCENT, 18);
    putStr(18, 0, L"\x00B7 console file manager", A_DIM, 30);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t r[96];
    swprintf(r, 96, L"%02d:%02d   Theme: %s [F9]   Help [F12]", st.wHour, st.wMinute, g_themes[g_theme].name);
    std::wstring rs = r;
    int rx = g_w - 1 - (int)rs.size();
    if (rx < 36) rx = 36;
    putStr(rx, 0, rs, A_DIM, g_w - rx);
}

static void drawInfoBar() {
    int y = g_h - 2;
    fillRow(0, y, g_w, L' ', A_NORMAL);
    if (!g_marks.empty()) {
        int dirs = 0, files = 0;
        for (auto &m : g_marks) { if (m.isDir) dirs++; else files++; }
        wchar_t tag[48];
        swprintf(tag, 48, L"\x2605 Marked: %d (%d dirs, %d files)  ", (int)g_marks.size(), dirs, files);
        putStr(1, y, tag, A_MARK, 40);
        if (g_marks.size() == 1) putStr(41, y, g_marks[0].fullPath, A_NORMAL, g_w - 64);
        putStr(g_w - 30, y, L"[F1] copy  [F2] move  [Del]", A_DIM, 29);
    } else {
        putStr(1, y, L"\x2606 No marked items  \x2014  press [Space] to mark; [F1] copy / [F2] move / [Del] delete all marked", A_DIM, g_w - 2);
    }
}

static void renderStatus(const std::wstring &msg) {
    int y = g_h - 1;
    fillRow(0, y, g_w, L' ', A_STATUS);
    if (!msg.empty()) {
        putStr(1, y, msg, A_STATUS, g_w - 1);
    } else {
        std::wstring help = L"[Tab] switch  [\x2191\x2193] move  [Enter] open  [Bksp] up  [F3] view  [F4] mode  [Del] delete  [F5/F6] http/ftp  [Esc] quit";
        putStr(1, y, help, A_STATUS, g_w - 1);
    }
}

static void render(const std::wstring &status) {
    clearBuf();
    drawTopBar();
    int boxTop = 1, boxBot = g_h - 3;
    int half = g_w / 2;
    renderPanel(0, 0, half - 2, boxTop, boxBot);
    renderPanel(1, half, g_w - 1, boxTop, boxBot);
    drawInfoBar();
    renderStatus(status);
    flush();
}

// ============================================================
//  SETTINGS SCREEN
// ============================================================
static void settingsScreen() {
    int orig = g_theme, origHue = g_monoHue;
    int sel = g_theme;
    int top = 0;
    applyTheme(sel);
    markDirty();
    while (true) {
        queryConsole();
        clearBuf();
        drawTopBar();
        int bw = 52;
        int visN = g_h - 16;
        if (visN > g_themeCount) visN = g_themeCount;
        if (visN < 4) visN = 4;
        int bh = visN + 12;
        int bx = (g_w - bw) / 2;
        int by = (g_h - bh) / 2;
        if (bx < 1) bx = 1;
        if (by < 2) by = 2;
        bool isMono = wcscmp(g_themes[sel].name, L"Mono") == 0;

        drawBox(bx, bx + bw - 1, by, by + bh - 1, A_ACCENT, L" \x25C6 Settings \x2014 Theme ");
        putStr(bx + 3, by + 2, L"Choose a color theme:", A_DIM, bw - 6);

        if (sel < top) top = sel;
        if (sel >= top + visN) top = sel - visN + 1;
        for (int r = 0; r < visN; r++) {
            int i = top + r;
            if (i >= g_themeCount) break;
            int y = by + 4 + r;
            bool s = (i == sel);
            WORD a = s ? A_CURSOR : A_NORMAL;
            fillRow(bx + 2, y, bw - 4, L' ', a);
            putCh(bx + 3, y, s ? L'\x25B8' : L' ', a);
            putStr(bx + 5, y, g_themes[i].name, a, bw - 10);
            if (s) putStr(bx + bw - 12, y, L"\x2714 active", a, 10);
        }
        if (top > 0) putStr(bx + bw - 6, by + 4, L"\x25B2", A_DIM, 1);
        if (top + visN < g_themeCount) putStr(bx + bw - 6, by + 3 + visN, L"\x25BC", A_DIM, 1);

        int sy = by + 4 + visN + 1;
        if (isMono) {
            putStr(bx + 3, sy, L"Accent color  (use \x2190 \x2192):", A_DIM, bw - 6);
            std::wstring pick = L"\x25C0   " + std::wstring(g_monoHues[g_monoHue].name) + L"   \x25B6";
            putStr(bx + 3, sy + 1, pick, A_ACCENT, bw - 6);
            wchar_t pos[24];
            swprintf(pos, 24, L"%d / %d", g_monoHue + 1, g_monoHueCount);
            putStr(bx + 3, sy + 2, pos, A_DIM, bw - 6);
        } else {
            putStr(bx + 3, sy, L"Preview", A_DIM, bw - 6);
            putStr(bx + 3, sy + 1, L"folder\\", A_DIR, 12);
            putStr(bx + 14, sy + 1, L"file.txt", A_NORMAL, 12);
            putStr(bx + 26, sy + 1, L"\x25C6 marked", A_MARK, 12);
            fillRow(bx + 3, sy + 2, bw - 6, L' ', A_CURSOR);
            putStr(bx + 4, sy + 2, L"\x25B8 selected row", A_CURSOR, bw - 8);
        }
        putStr(bx + 3, by + bh - 2, L"\x2191\x2193 theme   \x2190\x2192 accent   Enter apply   Esc cancel", A_DIM, bw - 6);
        flush();

        INPUT_RECORD ir;
        DWORD n;
        ReadConsoleInputW(g_in, &ir, 1, &n);
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;
        WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;
        if (vk == VK_UP) { if (sel > 0) sel--; applyTheme(sel); }
        else if (vk == VK_DOWN) { if (sel < g_themeCount - 1) sel++; applyTheme(sel); }
        else if (vk == VK_LEFT && isMono) { if (g_monoHue > 0) g_monoHue--; buildPalette(); }
        else if (vk == VK_RIGHT && isMono) { if (g_monoHue < g_monoHueCount - 1) g_monoHue++; buildPalette(); }
        else if (vk == VK_RETURN) { g_theme = sel; saveTheme(); return; }
        else if (vk == VK_ESCAPE || vk == VK_F9) { g_theme = orig; g_monoHue = origHue; applyTheme(orig); return; }
    }
}

// ============================================================
//  INPUT HELPERS
// ============================================================
static void waitKey() {
    INPUT_RECORD ir;
    DWORD n;
    while (ReadConsoleInputW(g_in, &ir, 1, &n)) {
        if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) break;
    }
}

// ============================================================
//  SPLASH SCREEN & ASCII ART
// ============================================================
static const wchar_t *g_artEmbed[] = { // was made by Denis ZZhiryakov BIG BALLS and site ASCCIART.COM
    L"",
    L"",
    L"",
    L"",
    L"",
    L"",
    L"",
    L"",
    L"",
    L"",
    L"                                                                                              -@",
    L"                                                                                           *@  =",
    L"                                                                           +             @@+  ==",
    L"                                                                           %           @@@    *@                        +@@--",
    L"                                                                          #@         +@@@     +@                   #@@@@- +-",
    L"                                                                          %@        @@@@     ++%:              #%%%%@@   ++",
    L"                                                                         %%=       @@@@      +*#@++=        #*+**##+    =-",
    L"                                                                       -%%@       @@@%@      ==#%@==++*%#:---==+++     ---",
    L"                                                                      @%%@      *@@@@@:  =    +##@@=++*****%@--=-     --+",
    L"                                                                    @*@%-   -+--%@@@@@:   +=  +**%%@++*####%##@@     ---",
    L"                                                                 =@#@@:  ++-----@@@+@*%   =+** ***#%@****#%%%@@%#@= =---",
    L"                                                               -@@@@= ++=------+@@@@@%%    =++++*+**+@######@@@@@%%@--=",
    L"                                                         *    @@@@  %++-----:--@@@@%=@#%   = =++++****#*##%%%%@@@@@@@-",
    L"                                                        %    @@@ =%++*-----:---%@@@##==*#   -==-+++***####%%@@@@@@@@@#%        @##",
    L"                                                      ++    :@#%@#*#+----::::--:@%@###*===:  ===+++++**##%%%@@@@@@@@@##=         *@@@",
    L"                                                     *@     @ @@###-----::::----@@*#***%=-------=++++++*#**%%@%@@@#@@@#%          @#@@@@",
    L"                                                    @@      %@@%%%-----::--::--@#*%-------====----====+++***%*#@%@@#*@@%%          %#@@@@@+",
    L"                                                   @@*     @@@%%%------==:::-###+--+#+++++*++++===+++===+******#*@#@#*@#@           +@@@@@@@#",
    L"                                                  =@@     @@@@%%*=-=-++:::::=---**#--+++-=====+++++*#=  -*+*******%#@##%%            *@#@@@@@@",
    L"                                                  @@@    @%@@##%==-*+--:::---:**+-+---@@@@@@@@@@@@@=#*##@+   +*****##@##%             %@@@@@@@@@",
    L"                                                 @@@@   @@@@@##-=-#*-:::-::::*=---@@@@@@@@@@@@@@@@@@@@@%*#%=   +#*#####%@              @@@@@@@@@@",
    L"                                                 @@@@  @@@@@@*#==#+--:--:::-+---@%@@@@@@@@@@@@@@@@@@@@@@@@#%%    ######%*              %@@@@@@@@@@+  #",
    L"                                                =@@@@**@@@%@@#*=*=-----::::=-=%@@@@@@@@@@@@@@@@@@@@@@@@@@@@#%%%@+ ####%%*           =*  @@@@@@@@@@@# @",
    L"                                                %%@*@*@%%@@@@@-=+-----::::--@@@@*@@*##########%@@@@%@@@@@@@@@@#%%%#%%%%%@            @@ @@@@@@@@@@@@ @@",
    L"                                                %%%#+%@%%%@@@@-=------:::--@@##@*#*:-*@@@@@@@@@@@@%%@@@@@@@@@@@@@@%%*%%@@+           +@@@@@@@@@@@@@@@@@@",
    L"                                                %+%++@@%%%@@@@@=-----::::-@@*##:---@@@%%%@@@@@%%%%%%#@%@@@@@@@@@@@@#@%@@@@*          %@@@@@@@@@@@@@@@@@@",
    L"                                                ++#-=@@#%%@@%@@@-----::::@%*-:---@@@@@@@@@@#############%%@@@@@@@@@@@%@@@@@*         +@@@@@@@@@@@@@@@@@@@",
    L"                                            -   :=*-=@@###@@@@@@@----::::%+=--=-@@@@@@@#%%%%%%%+++++++-:-=:+@%%@@@@@@@@%@  =%        #@@@@@@@@@@@@@@@@@@@",
    L"                                            :    ==+-@@*###@@@@@@@---:::=-=:==-%@@@@@*@@@@@@*%*%*+:========   *-@@@@@@@@#           +@@@@@@@@@@@@@@@@@@@@=",
    L"                                            :-   --+-@%@*##@@%#@+@@#--:::-=++==%@@@=#@@@@@@@@@##=:==----::::--    :@@@@@@@=##      =*@@@@@@@@@@@@@@@@@@@@@",
    L"                                             --   =--%%%+*#@@%%*@+*+@=-:-==+-==%@@+#@@@@@@@@@*++:=--::::::::                      +*@@@@@@@@@@@@@@@@@@@@@@",
    L"                                             ---   --@=%@+**@@%##@@*++%-:= *-==@@+*+@@@@@@@@#*%#=--:::::---:                    =++#@@@@@@@@@@@@@@@@@@@@@@",
    L"                                             -----  -%+@%@=*@@%%##@@+++++- +*===+**#@@@@@@@#%@%@=-::-:::----                  ==+++@@@@@@@@@@@@@@@@@@@@@@@  *",
    L"                                             :-----  :#-##+=#@@%@#%%@@=+++* *===###*@@@@@@@%%@@@+-:-----------             ---===@@@@@@@@@@@@@@@@@@@@@@@@@  *",
    L"                                              ------- #*-*#-=#@@@@#%%%#@@++=-*===%%%%@@@@@@@@@@@:-:-------------=-  =---------=-@@@@@@@@@@@@@@@@@@@@@@@@@* *",
    L"                                               --:----:*--##--=@%%%*%%%%%@@===*==-%@%%@@@@@@@@@@@=----------------------------@#@@@@@@@@@@@@@@@@@@@@@@@@@  @",
    L"                                               :-------+*--##---@%%%%*%%%%%@@==*==@@@@@@@@@@@@@@@@----------=------:--------@@@@@@@%@@@@@@@@@@%%@@@@@@@@# @+",
    L"                                                :-- ----++=--#---*@%%%=*%%%%@@@=++=@@@@@@@@@@@@@@@@*:------------:::--*--@@@@@#%%%@@@@@@@@@@@@#*@@@@@@%@ @#",
    L"                                                 :-: ----+----#----@*%#%+%%-%%%%%-=-*@@@@@@@@@@@@@@@@::-----::--:-:@%%-@@@####*@@@@@@@@@@@@@@**@@@@#@#@+@**",
    L"                                                   -  :-:-=-----#---%*+##=-%*-%%%%@=-%@@@@@@@@@@@@@@@@::-----==-%%%#-@=***#*@##@@@%@@@@@@@@@*+=@%@#%+%@@**",
    L"                                                 :  :   --------------%=##-=#==-%%#%=-#%@@@@@@@@@@@%%%%::-:::%%%#%=*+***#%#@@*#%@@@##@@@#@+++=%@@**=#@+*++",
    L"                                                  -   :   --------------==#-=*-==-###--%%%#@@@@@%@@*##*:-=@#%++@++**+@@@+*#%%%@+##@@@@*%#++++@@@++#@@**=#",
    L"                                                  :-       ::-------------=%--+-==-###--%#@%%%@@#*@+*+=:%%+==#+*+@@++=@%%%+*#**#@@@@+%=++==@@#+=#@@**==*",
    L"                                                   :-:      :::-:----:------%-------:##--##%#%#@**==-=#=-==++=%%==%%%%*###**@@@@@++=+++=@@@==-@@%+++=++",
    L"                                                    --::     ::::-:-:::::----*-=-----:*=--**+**=+::*#----==#+==#%%%%%###@@@@@@-====#@@@===*@@%+++-==++",
    L"                                                     -:::      ::::-:-::::::--=-------*+:::+=+=-:++-----+#---####%%--@%%%@-===========@%@%=++*=---==+",
    L"                                                      -:::::    ::::--:--:::::-=----:::*:::=:::==-----*---=#+-====@%%*-======-#@%%%%++++=#*------==+",
    L"                                                       --:::::    ::::----::::::=----::-:::-::------+=---====---%@-=====*%%%++++++++#*#--------==+=",
    L"                                                        --::::::   :::: -:--:::::=---:::::::-----:+=---=+-----#-===-#*+++++++=#**#-----------==--:",
    L"                                                         :-:::::::  :::: ----::::::--::::::-::::==--=+---------********-***#--------------===---",
    L"                                                           -:::   :   ::: .---::::-:-:::::-:::+=--=--------=**+*-==+++=-----------------=---:-   :",
    L"                                                            --::   :   ::   --:::::::::::-:::=--=-------+++----==+--------------------:::::-   +",
    L"                                                              -::       ::  :-:::::::: :-::=--=-----=++----===--------:-----::::::::::::-:  :=:",
    L"                                                                -::     :: : -:::::::: -::+-------=----====----:-----:--:::::::::::::-:   -=:",
    L"                                                                  - :    : : :::::::: -:=*-----=---+===+.-------:::::::::::::::::::    =--",
    L"                                                                    -         :::::  ::#*=---=-*+++*.::::::::::::::::::::::::     :-----",
    L"                                                                      ..     ::::::  -@%---=#***=:--::::::::::::::::        ::--:----",
    L"                                                                             :::::- :#%=-+%%%@-----:::::::        :::::::::::::::",
    L"                                                                             :::--==-@=#+@@=-----==-                    ::",
    L"                                                                              :--=+-@*#@@+===-**-    -  -==-:              :",
    L"                                                                               = ++@@%@@+++=@**=#++   -            :::",
    L"                                                                               = **@%@%***@@@%%%       -::",
    L"                                                                                 *-@@@%%%@@@#    +=",
    L"                                                                                 #:@@@%@@@   *",
    L"                                                                                 #:@@@@@  #",
    L"                                                                                  %@@@+ %",
    L"                                                                                   @@%@",
    L"                                                                                   :@",
    L"                                                                                    @",
    L"                                                                                     %",
    L"",
    L"",
    L"",
};

static void loadArt() {
    int count = (int)(sizeof(g_artEmbed) / sizeof(g_artEmbed[0]));
    for (int i = 0; i < count; i++) g_art.push_back(g_artEmbed[i]);
}

static int artWeight(wchar_t c) {
    switch (c) {
        case L'@': return 9;
        case L'#': return 8;
        case L'%': return 7;
        case L'*': return 6;
        case L'+': return 5;
        case L'=': return 4;
        case L'-': return 3;
        case L':': return 2;
        case L'.': return 1;
        default:   return 0;
    }
}

static bool peekKey() {
    DWORD num = 0;
    if (!GetNumberOfConsoleInputEvents(g_in, &num) || num == 0) return false;
    INPUT_RECORD ir;
    DWORD r = 0;
    ReadConsoleInputW(g_in, &ir, 1, &r);
    return (r && ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown);
}

static void splashScreen() {
    markDirty();
    clearBuf();
    flush();

    if (g_art.empty()) return;

    int minR = 1 << 30, maxR = -1, minC = 1 << 30, maxC = -1;
    for (int r = 0; r < (int)g_art.size(); r++) {
        const std::wstring &ln = g_art[r];
        for (int c = 0; c < (int)ln.size(); c++) {
            if (artWeight(ln[c]) > 0) {
                if (r < minR) minR = r;
                if (r > maxR) maxR = r;
                if (c < minC) minC = c;
                if (c > maxC) maxC = c;
            }
        }
    }
    if (maxR < 0) { Sleep(200); return; }

    int cw = maxC - minC + 1, ch = maxR - minR + 1;
    int availW = g_w - 2, availH = g_h - 4;
    double sx = (double)availW / cw, sy = (double)availH / ch;
    double sc = sx < sy ? sx : sy;
    if (sc > 1.0) sc = 1.0;
    int outW = (int)(cw * sc), outH = (int)(ch * sc);
    if (outW < 1) outW = 1;
    if (outH < 1) outH = 1;

    auto sample = [&](int oy, int ox, WORD &col) -> wchar_t {
        int br0 = minR + oy * ch / outH, br1 = minR + (oy + 1) * ch / outH;
        int bc0 = minC + ox * cw / outW, bc1 = minC + (ox + 1) * cw / outW;
        if (br1 <= br0) br1 = br0 + 1;
        if (bc1 <= bc0) bc1 = bc0 + 1;
        int bestW = 0;
        wchar_t best = L' ';
        for (int r = br0; r < br1 && r < (int)g_art.size(); r++) {
            const std::wstring &ln = g_art[r];
            for (int c = bc0; c < bc1 && c < (int)ln.size(); c++) {
                int w = artWeight(ln[c]);
                if (w > bestW) { bestW = w; best = ln[c]; }
            }
        }
        col = A_ACCENT;
        return best;
    };

    int top = (g_h - outH - 3) / 2;
    if (top < 0) top = 0;
    int left = (g_w - outW) / 2;
    if (left < 0) left = 0;

    int stepMs = 850 / outH;
    if (stepMs < 6) stepMs = 6;
    if (stepMs > 35) stepMs = 35;

    bool skip = false;
    for (int oy = 0; oy < outH; oy++) {
        for (int ox = 0; ox < outW; ox++) {
            WORD col;
            wchar_t ch2 = sample(oy, ox, col);
            putCh(left + ox, top + oy, ch2, col);
        }
        flush();
        if (!skip && peekKey()) skip = true;
        if (!skip) Sleep(stepMs);
    }

    std::wstring t = L"F O X   F I L E   M A N A G E R";
    putStr((g_w - (int)t.size()) / 2, top + outH + 1, t, A_SYKW, t.size());
    std::wstring sub = L"console file manager";
    putStr((g_w - (int)sub.size()) / 2, top + outH + 2, sub, A_DIM, sub.size());
    flush();
    Sleep(450);
}

// ============================================================
//  HELP SCREEN
// ============================================================
static void helpScreen() {
    struct Row { const wchar_t *key; const wchar_t *desc; };
    static const Row rows[] = {
        { L"\x2191 \x2193",      L"move cursor up / down" },
        { L"Tab / \x2190 \x2192", L"switch between the two panels" },
        { L"Enter",     L"open folder  (or view file with syntax)" },
        { L"Backspace", L"go to parent directory" },
        { L"Home / End", L"jump to first / last entry" },
        { L"PgUp/PgDn", L"scroll a page" },
        { L"",          L"" },
        { L"Space",     L"mark / unmark (multi-select, across dirs)" },
        { L"F1",        L"copy all marked into active panel" },
        { L"F2",        L"move (cut) all marked into active panel" },
        { L"Del",       L"delete marked items (or cursor if none)" },
        { L"F7",        L"create new folder" },
        { L"",          L"" },
        { L"F3",        L"view file content (code is highlighted)" },
        { L"F4",        L"toggle detailed / short list (per panel)" },
        { L"F5 / F6",   L"download file from HTTP / FTP url" },
        { L"",          L"" },
        { L"F9",        L"open theme settings" },
        { L"F12",       L"this help" },
        { L"Esc",       L"quit  (or close overlay)" },
    };
    int nrows = (int)(sizeof(rows) / sizeof(rows[0]));
    markDirty();
    queryConsole();
    clearBuf();
    drawTopBar();
    int bw = 60;
    int bh = nrows + 6;
    int bx = (g_w - bw) / 2;
    int by = (g_h - bh) / 2;
    if (bx < 1) bx = 1;
    if (by < 1) by = 1;
    drawBox(bx, bx + bw - 1, by, by + bh - 1, A_ACCENT, L" Fox File Manager \x2014 Keys ");
    for (int i = 0; i < g_foxMiniN; i++)
        putStr(bx + bw - 10, by + 1 + i, g_foxMini[i], A_DIM, 9);
    for (int i = 0; i < nrows; i++) {
        int y = by + 2 + i;
        putStr(bx + 3, y, rows[i].key, A_ACCENT, 12);
        putStr(bx + 16, y, rows[i].desc, A_NORMAL, bw - 18);
    }
    putStr(bx + 3, by + bh - 2, L"press any key to close", A_DIM, bw - 6);
    flush();
    waitKey();
}

// ============================================================
//  FILE OPERATIONS (recursive copy / delete)
// ============================================================
static bool copyRecursive(const std::wstring &src, const std::wstring &dst, bool isDir, std::wstring &err) {
    if (!isDir) {
        if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) return true;
        if (err.empty()) err = baseName(src) + L": " + winErr(GetLastError());
        return false;
    }
    if (!CreateDirectoryW(dst.c_str(), NULL)) {
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) {
            if (err.empty()) err = baseName(dst) + L": " + winErr(e);
            return false;
        }
    }
    std::wstring mask = joinPath(src, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(mask.c_str(), &fd);
    bool ok = true;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring s = joinPath(src, name);
            std::wstring d = joinPath(dst, name);
            bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!copyRecursive(s, d, dir, err)) ok = false;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return ok;
}

static bool deleteRecursive(const std::wstring &path, bool isDir, std::wstring &err) {
    if (!isDir) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(path.c_str())) return true;
        if (err.empty()) err = baseName(path) + L": " + winErr(GetLastError());
        return false;
    }
    std::wstring mask = joinPath(path, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(mask.c_str(), &fd);
    bool ok = true;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring s = joinPath(path, name);
            bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!deleteRecursive(s, dir, err)) ok = false;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (RemoveDirectoryW(path.c_str())) return ok;
    if (err.empty()) err = baseName(path) + L": " + winErr(GetLastError());
    return false;
}

// ============================================================
//  PROMPTS & DIALOGS
// ============================================================
static std::wstring promptLine(const std::wstring &prompt) {
    dimBuf();
    int y = g_h - 1;
    fillRow(0, y, g_w, L' ', A_STATUS);
    putStr(0, y, L" " + prompt, A_STATUS, g_w);
    flush();

    SetConsoleCursorPosition(g_out, COORD{ (SHORT)(prompt.size() + 2), (SHORT)y });
    DWORD oldMode;
    GetConsoleMode(g_in, &oldMode);
    SetConsoleMode(g_in, ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);

    wchar_t buf[2048];
    DWORD read = 0;
    std::wstring result;
    if (ReadConsoleW(g_in, buf, 2047, &read, NULL)) {
        result.assign(buf, read);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n'))
            result.pop_back();
    }
    SetConsoleMode(g_in, oldMode);
    return result;
}

static bool confirmYesNo(const std::wstring &prompt) {
    dimBuf();
    int y = g_h - 1;
    fillRow(0, y, g_w, L' ', A_STATUS);
    putStr(1, y, prompt + L"    [Y] yes   [N]/[Esc] no", A_STATUS, g_w - 1);
    flush();
    while (true) {
        INPUT_RECORD ir;
        DWORD n;
        if (!ReadConsoleInputW(g_in, &ir, 1, &n)) return false;
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;
        wchar_t c = ir.Event.KeyEvent.uChar.UnicodeChar;
        WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;
        if (c == L'y' || c == L'Y') return true;
        if (c == L'n' || c == L'N' || vk == VK_ESCAPE) return false;
    }
}

// ============================================================
//  DOWNLOAD (HTTP / FTP)
// ============================================================
static bool downloadUrl(const std::wstring &url, const std::wstring &outPath, std::wstring &err) {
    HINTERNET hNet = InternetOpenW(L"FoxFM/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hNet) { err = L"InternetOpen failed"; return false; }
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_PASSIVE;
    HINTERNET hUrl = InternetOpenUrlW(hNet, url.c_str(), NULL, 0, flags, 0);
    if (!hUrl) {
        err = L"Connect/open URL failed (err " + std::to_wstring(GetLastError()) + L")";
        InternetCloseHandle(hNet);
        return false;
    }
    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        err = L"Cannot create local file";
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hNet);
        return false;
    }
    char data[16384];
    DWORD got = 0;
    bool ok = true;
    unsigned long long total = 0;
    while (InternetReadFile(hUrl, data, sizeof(data), &got) && got > 0) {
        DWORD written = 0;
        if (!WriteFile(hFile, data, got, &written, NULL) || written != got) { ok = false; break; }
        total += got;
    }
    CloseHandle(hFile);
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hNet);
    if (ok && total == 0) { err = L"0 bytes received"; }
    return ok;
}

static void doDownload(int kind) {
    Panel &p = g_panel[g_active];
    std::wstring proto = (kind == 0) ? L"HTTP url: " : L"FTP url: ";
    std::wstring url = promptLine(proto);
    if (url.empty()) return;
    std::wstring fname = url;
    size_t q = fname.find_first_of(L"?#");
    if (q != std::wstring::npos) fname = fname.substr(0, q);
    fname = sanitizeFileName(fname);
    std::wstring out = joinPath(p.path, fname);
    render(L"Downloading " + url + L" ...");
    std::wstring err;
    bool ok = downloadUrl(url, out, err);
    loadDir(p);
    if (ok) render(L"Saved: " + fname);
    else render(L"Download failed: " + err);
    flush();
    Sleep(900);
}

// ============================================================
//  SYNTAX HIGHLIGHTING
// ============================================================
static const std::unordered_set<std::wstring> g_cKW = {
    L"if", L"else", L"for", L"while", L"do", L"switch", L"case", L"default", L"break",
    L"continue", L"return", L"goto", L"new", L"delete", L"try", L"catch", L"finally",
    L"throw", L"throws", L"class", L"struct", L"enum", L"union", L"namespace", L"using",
    L"template", L"typename", L"typedef", L"public", L"private", L"protected", L"static",
    L"const", L"constexpr", L"consteval", L"virtual", L"override", L"final", L"friend",
    L"inline", L"explicit", L"operator", L"this", L"super", L"true", L"false", L"null",
    L"nullptr", L"nil", L"sizeof", L"alignof", L"decltype", L"volatile", L"mutable",
    L"register", L"extern", L"import", L"export", L"package", L"function", L"func", L"var",
    L"let", L"async", L"await", L"yield", L"in", L"of", L"instanceof", L"typeof", L"with",
    L"defer", L"go", L"select", L"range", L"fn", L"impl", L"trait", L"pub", L"use", L"mod",
    L"match", L"where", L"ref", L"out", L"is", L"as", L"base", L"internal", L"sealed",
    L"readonly", L"interface", L"abstract", L"extends", L"implements", L"synchronized",
    L"native", L"transient", L"static_cast", L"reinterpret_cast", L"dynamic_cast",
    L"const_cast", L"and", L"or", L"not", L"xor", L"bitand", L"bitor"
};
static const std::unordered_set<std::wstring> g_cTypes = {
    L"int", L"long", L"short", L"char", L"unsigned", L"signed", L"float", L"double",
    L"bool", L"void", L"size_t", L"ssize_t", L"int8_t", L"uint8_t", L"int16_t", L"uint16_t",
    L"int32_t", L"uint32_t", L"int64_t", L"uint64_t", L"intptr_t", L"uintptr_t", L"wchar_t",
    L"char16_t", L"char32_t", L"string", L"wstring", L"vector", L"map", L"set", L"auto",
    L"byte", L"boolean", L"String", L"Integer", L"Object", L"number", L"any", L"decimal",
    L"uint", L"sbyte", L"ulong", L"ushort", L"DWORD", L"WORD", L"BYTE", L"HANDLE", L"BOOL",
    L"LPVOID", L"COLORREF", L"FILETIME", L"HINTERNET", L"LPCWSTR", L"LPWSTR", L"UINT", L"u8",
    L"u16", L"u32", L"u64", L"i8", L"i16", L"i32", L"i64", L"usize", L"isize", L"str", L"f32", L"f64"
};
static const std::unordered_set<std::wstring> g_pyKW = {
    L"def", L"class", L"import", L"from", L"as", L"if", L"elif", L"else", L"for", L"while",
    L"return", L"yield", L"with", L"try", L"except", L"finally", L"raise", L"lambda",
    L"None", L"True", L"False", L"and", L"or", L"not", L"in", L"is", L"pass", L"break",
    L"continue", L"global", L"nonlocal", L"del", L"assert", L"async", L"await", L"match", L"case"
};
static const std::unordered_set<std::wstring> g_pyTypes = {
    L"int", L"float", L"str", L"bool", L"list", L"dict", L"tuple", L"set", L"bytes",
    L"object", L"complex", L"frozenset", L"bytearray", L"type"
};

struct Lang {
    bool valid;
    std::wstring lineComment;
    std::wstring blockStart;
    std::wstring blockEnd;
    bool hashPreproc;
    bool tripleStr;
    const std::unordered_set<std::wstring> *kw;
    const std::unordered_set<std::wstring> *tp;
};

static bool extIn(const std::wstring &e, std::initializer_list<const wchar_t*> set) {
    for (auto s : set) if (e == s) return true;
    return false;
}

static Lang langFor(const std::wstring &ext) {
    Lang L = { false, L"", L"", L"", false, false, NULL, NULL };
    if (extIn(ext, { L"c", L"h", L"cpp", L"hpp", L"cc", L"cxx", L"hxx", L"cs", L"ino" })) {
        L = { true, L"//", L"/*", L"*/", true, false, &g_cKW, &g_cTypes };
    } else if (extIn(ext, { L"js", L"jsx", L"ts", L"tsx", L"java", L"go", L"rs", L"php",
                            L"swift", L"kt", L"kts", L"scala", L"json", L"jsonc", L"dart", L"glsl", L"hlsl" })) {
        L = { true, L"//", L"/*", L"*/", false, false, &g_cKW, &g_cTypes };
    } else if (extIn(ext, { L"py", L"pyw", L"pyi" })) {
        L = { true, L"#", L"", L"", false, true, &g_pyKW, &g_pyTypes };
    } else if (extIn(ext, { L"sh", L"bash", L"zsh", L"ps1", L"psm1", L"yml", L"yaml",
                            L"ini", L"cfg", L"conf", L"toml", L"env", L"properties", L"makefile", L"mk" })) {
        L = { true, L"#", L"", L"", false, false, NULL, NULL };
    } else if (extIn(ext, { L"lua", L"sql" })) {
        L = { true, L"--", L"", L"", false, false, &g_cKW, &g_cTypes };
    }
    return L;
}

static bool matchAt(const std::wstring &t, size_t i, const std::wstring &p) {
    if (p.empty() || i + p.size() > t.size()) return false;
    return t.compare(i, p.size(), p) == 0;
}

static void highlight(const std::wstring &text, const Lang &L,
                      std::vector<std::wstring> &lines,
                      std::vector<std::vector<WORD>> &attrs) {
    std::wstring cur;
    std::vector<WORD> curA;
    auto flushLine = [&]() { lines.push_back(cur); attrs.push_back(curA); cur.clear(); curA.clear(); };
    auto emit = [&](wchar_t ch, WORD a) {
        if (ch == L'\t') { for (int k = 0; k < 4; k++) { cur += L' '; curA.push_back(a); } }
        else { cur += ch; curA.push_back(a); }
    };

    size_t i = 0, n = text.size();
    bool lineStart = true;
    while (i < n) {
        wchar_t c = text[i];
        if (c == L'\r') { i++; continue; }
        if (c == L'\n') { flushLine(); i++; lineStart = true; continue; }

        if (matchAt(text, i, L.blockStart)) {
            for (wchar_t bc : L.blockStart) emit(bc, A_SYCOM);
            i += L.blockStart.size();
            while (i < n && !matchAt(text, i, L.blockEnd)) {
                if (text[i] == L'\n') { flushLine(); i++; }
                else if (text[i] == L'\r') i++;
                else { emit(text[i], A_SYCOM); i++; }
            }
            if (i < n) { for (wchar_t bc : L.blockEnd) emit(bc, A_SYCOM); i += L.blockEnd.size(); }
            lineStart = false;
            continue;
        }

        if (matchAt(text, i, L.lineComment)) {
            while (i < n && text[i] != L'\n') { if (text[i] != L'\r') emit(text[i], A_SYCOM); i++; }
            continue;
        }

        if (L.hashPreproc && lineStart && c == L'#') {
            emit(c, A_SYPRE);
            i++;
            while (i < n && (iswalpha(text[i]) || text[i] == L'_')) { emit(text[i], A_SYPRE); i++; }
            lineStart = false;
            continue;
        }

        if (c == L'"' || c == L'\'' || c == L'`') {
            if (L.tripleStr && (matchAt(text, i, L"\"\"\"") || matchAt(text, i, L"'''"))) {
                std::wstring tri(3, c);
                for (int k = 0; k < 3; k++) emit(c, A_SYSTR);
                i += 3;
                while (i < n && !matchAt(text, i, tri)) {
                    if (text[i] == L'\n') { flushLine(); i++; }
                    else if (text[i] == L'\r') i++;
                    else { emit(text[i], A_SYSTR); i++; }
                }
                if (i < n) { for (int k = 0; k < 3; k++) emit(c, A_SYSTR); i += 3; }
                lineStart = false;
                continue;
            }
            wchar_t q = c;
            emit(c, A_SYSTR);
            i++;
            while (i < n && text[i] != q && text[i] != L'\n') {
                if (text[i] == L'\\' && i + 1 < n) { emit(text[i], A_SYSTR); emit(text[i + 1], A_SYSTR); i += 2; }
                else { emit(text[i], A_SYSTR); i++; }
            }
            if (i < n && text[i] == q) { emit(q, A_SYSTR); i++; }
            lineStart = false;
            continue;
        }

        if (iswdigit(c)) {
            while (i < n && (iswalnum(text[i]) || text[i] == L'.' || text[i] == L'_')) { emit(text[i], A_SYNUM); i++; }
            lineStart = false;
            continue;
        }

        if (iswalpha(c) || c == L'_') {
            std::wstring id;
            size_t s = i;
            while (i < n && (iswalnum(text[i]) || text[i] == L'_')) { id += text[i]; i++; }
            WORD a = A_NORMAL;
            if (L.kw && L.kw->count(id)) a = A_SYKW;
            else if (L.tp && L.tp->count(id)) a = A_SYTYPE;
            else {
                size_t j = i;
                while (j < n && (text[j] == L' ' || text[j] == L'\t')) j++;
                if (j < n && text[j] == L'(') a = A_SYFUNC;
            }
            for (wchar_t ch : id) emit(ch, a);
            (void)s;
            lineStart = false;
            continue;
        }

        static const std::wstring ops = L"+-*/%=<>!&|^~?:.,;(){}[]";
        if (c == L' ' || c == L'\t') { emit(c, A_NORMAL); i++; continue; }
        if (ops.find(c) != std::wstring::npos) emit(c, A_SYOP);
        else emit(c, A_NORMAL);
        i++;
        lineStart = false;
    }
    flushLine();
}

// ============================================================
//  FILE VIEWER
// ============================================================
static bool isBinaryData(const char *d, DWORD n) {
    if (n == 0) return false;
    DWORD lim = n < 8192 ? n : 8192;
    int bad = 0;
    for (DWORD i = 0; i < lim; i++) {
        unsigned char b = (unsigned char)d[i];
        if (b == 0) return true;
        if (b < 32 && b != 9 && b != 10 && b != 13) bad++;
    }
    return (long)bad * 100 / lim > 10;
}

static void blockedScreen(const std::wstring &path, const std::wstring &reason) {
    int bw = 60, bh = 11;
    markDirty();
    while (true) {
        queryConsole();
        clearBuf();
        int bx = (g_w - bw) / 2, by = (g_h - bh) / 2;
        if (bx < 1) bx = 1;
        if (by < 1) by = 1;
        drawBox(bx, bx + bw - 1, by, by + bh - 1, A_MARK, L" Cannot open ");
        putStr(bx + 3, by + 2, L"[!]  This file cannot be displayed in the viewer", A_MARK, bw - 6);
        putStr(bx + 3, by + 4, L"File:   " + baseName(path), A_NORMAL, bw - 6);
        putStr(bx + 3, by + 5, L"Reason: " + reason, A_DIM, bw - 6);
        putStr(bx + 3, by + 7, L"[Enter]  open with the default system app", A_ACCENT, bw - 6);
        putStr(bx + 3, by + 8, L"[Esc]    go back", A_DIM, bw - 6);
        flush();

        INPUT_RECORD ir;
        DWORD n;
        if (!ReadConsoleInputW(g_in, &ir, 1, &n)) return;
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;
        WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;
        if (vk == VK_RETURN) {
            ShellExecuteW(NULL, L"open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
            return;
        }
        if (vk == VK_ESCAPE || vk == VK_F3) return;
    }
}

static void viewFile(const std::wstring &path) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    GetFileSizeEx(hFile, &sz);
    const long long VIEW_LIMIT = 64LL * 1024 * 1024;
    bool truncated = sz.QuadPart > VIEW_LIMIT;
    DWORD cap = (DWORD)(truncated ? VIEW_LIMIT : sz.QuadPart);
    std::vector<char> raw(cap + 1);
    DWORD got = 0;
    ReadFile(hFile, raw.data(), cap, &got, NULL);
    CloseHandle(hFile);
    raw[got] = 0;

    std::wstring name = baseName(path);
    std::wstring ext;
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) ext = name.substr(dot + 1);
    for (auto &ch : ext) ch = (wchar_t)towlower(ch);

    std::wstring text;
    Lang lang = { false, L"", L"", L"", false, false, NULL, NULL };

    bool bom16 = got >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE;
    bool bom8  = got >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF;
    if (!bom16 && !bom8 && isBinaryData(raw.data(), got)) {
        blockedScreen(path, L"Unsupported or binary file format");
        return;
    }
    if (bom16) {
        text.assign((wchar_t*)(raw.data() + 2), (got - 2) / 2);
    } else {
        int skip = bom8 ? 3 : 0;
        int need = MultiByteToWideChar(CP_UTF8, 0, raw.data() + skip, got - skip, NULL, 0);
        std::wstring u(need, 0);
        MultiByteToWideChar(CP_UTF8, 0, raw.data() + skip, got - skip, &u[0], need);
        if (u.find(0xFFFD) != std::wstring::npos) {
            int n2 = MultiByteToWideChar(CP_ACP, 0, raw.data(), got, NULL, 0);
            std::wstring a(n2, 0);
            MultiByteToWideChar(CP_ACP, 0, raw.data(), got, &a[0], n2);
            text = a;
        } else {
            text = u;
        }
    }
    lang = langFor(ext);

    std::vector<std::wstring> lines;
    std::vector<std::vector<WORD>> attrs;
    if (lang.valid) {
        highlight(text, lang, lines, attrs);
    } else {
        std::wstring cur;
        std::vector<WORD> ca;
        for (wchar_t ch : text) {
            if (ch == L'\r') continue;
            if (ch == L'\n') { lines.push_back(cur); attrs.push_back(ca); cur.clear(); ca.clear(); }
            else if (ch == L'\t') { for (int k = 0; k < 4; k++) { cur += L' '; ca.push_back(A_NORMAL); } }
            else { cur += ch; ca.push_back(A_NORMAL); }
        }
        lines.push_back(cur);
        attrs.push_back(ca);
    }

    int scroll = 0, colOff = 0;
    markDirty();
    while (true) {
        queryConsole();
        int viewH = g_h - 2;
        clearBuf();
        fillRow(0, 0, g_w, L' ', A_HEADER);
        std::wstring head = L" \x25B6 VIEW  " + name;
        if (lang.valid) head += L"   [syntax]";
        if (truncated) head += L"   [TRUNCATED 64M of " + fmtSize((unsigned long long)sz.QuadPart) + L"]";
        putStr(0, 0, head, A_HEADER, g_w);

        int numW = (int)std::to_wstring(lines.size()).size();
        if (numW < 3) numW = 3;
        int sepX = numW + 1;
        int contentX = numW + 3;
        for (int r = 0; r < viewH; r++) {
            int li = scroll + r;
            if (li >= (int)lines.size()) break;
            wchar_t num[16];
            swprintf(num, 16, L"%*d", numW, li + 1);
            putStr(0, 1 + r, num, A_DIM, numW);
            putCh(sepX, 1 + r, L'\x2502', A_FRAME);
            const std::wstring &ln = lines[li];
            const std::vector<WORD> &la = attrs[li];
            for (int cx = contentX; cx < g_w; cx++) {
                int si = colOff + (cx - contentX);
                if (si >= (int)ln.size()) break;
                putCh(cx, 1 + r, ln[si], si < (int)la.size() ? la[si] : A_NORMAL);
            }
        }

        fillRow(0, g_h - 1, g_w, L' ', A_STATUS);
        wchar_t info[96];
        swprintf(info, 96, L" line %d/%d%s  col %d   \x2502  \x2191\x2193 \x2190\x2192 scroll  \x2502  Esc back",
                 scroll + 1, (int)lines.size(), truncated ? L"+" : L"", colOff + 1);
        putStr(0, g_h - 1, info, A_STATUS, g_w);
        flush();

        INPUT_RECORD ir;
        DWORD n;
        ReadConsoleInputW(g_in, &ir, 1, &n);
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;
        WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;
        int maxScroll = (int)lines.size() - viewH;
        if (maxScroll < 0) maxScroll = 0;
        if (vk == VK_ESCAPE || vk == VK_F3) break;
        else if (vk == VK_DOWN) { scroll += accelStep(VK_DOWN); if (scroll > maxScroll) scroll = maxScroll; }
        else if (vk == VK_UP) { scroll -= accelStep(VK_UP); if (scroll < 0) scroll = 0; }
        else if (vk == VK_RIGHT) colOff += 8;
        else if (vk == VK_LEFT) { colOff -= 8; if (colOff < 0) colOff = 0; }
        else if (vk == VK_NEXT) { scroll += viewH; if (scroll > maxScroll) scroll = maxScroll; }
        else if (vk == VK_PRIOR) { scroll -= viewH; if (scroll < 0) scroll = 0; }
        else if (vk == VK_HOME) { scroll = 0; colOff = 0; }
        else if (vk == VK_END) scroll = maxScroll;
    }
}

// ============================================================
//  FILE-OP ACTIONS & NAVIGATION
// ============================================================
static void doPaste(bool move) {
    if (g_marks.empty()) { render(L"Nothing marked"); flush(); Sleep(700); return; }
    Panel &p = g_panel[g_active];
    int done = 0, failed = 0, skipped = 0;
    std::wstring lastErr;
    std::vector<MarkItem> remain;

    for (size_t k = 0; k < g_marks.size(); k++) {
        MarkItem m = g_marks[k];
        std::wstring dst = joinPath(p.path, baseName(m.fullPath));
        if (_wcsicmp(dst.c_str(), m.fullPath.c_str()) == 0) {
            skipped++;
            remain.push_back(m);
            if (lastErr.empty()) lastErr = baseName(m.fullPath) + L": source equals destination";
            continue;
        }
        wchar_t prog[96];
        swprintf(prog, 96, L"%s %d/%d: %s", move ? L"Moving" : L"Copying",
                 (int)k + 1, (int)g_marks.size(), baseName(m.fullPath).c_str());
        render(prog);
        flush();

        std::wstring err;
        bool ok;
        if (move) {
            ok = MoveFileExW(m.fullPath.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING) != 0;
            if (!ok) {
                if (copyRecursive(m.fullPath, dst, m.isDir, err)) {
                    err.clear();
                    ok = deleteRecursive(m.fullPath, m.isDir, err);
                }
            }
        } else {
            ok = copyRecursive(m.fullPath, dst, m.isDir, err);
        }

        if (ok) {
            done++;
        } else {
            failed++;
            remain.push_back(m);
            if (err.empty()) err = baseName(m.fullPath) + L": " + winErr(GetLastError());
            lastErr = err;
        }
    }

    g_marks = remain;
    loadDir(g_panel[0]);
    loadDir(g_panel[1]);

    wchar_t sum[160];
    if (failed == 0 && skipped == 0)
        swprintf(sum, 160, L"%s %d item(s)", move ? L"Moved" : L"Copied", done);
    else
        swprintf(sum, 160, L"%s %d, failed %d, skipped %d  \x2014  %s",
                 move ? L"Moved" : L"Copied", done, failed, skipped, lastErr.c_str());
    render(sum);
    flush();
    Sleep(failed || skipped ? 1600 : 700);
}

static void doDelete() {
    Panel &p = g_panel[g_active];
    std::vector<MarkItem> targets;
    if (!g_marks.empty()) {
        targets = g_marks;
    } else {
        if (p.entries.empty()) return;
        Entry &e = p.entries[p.cursor];
        if (e.name == L"..") return;
        targets.push_back({ e.isDir, joinPath(p.path, e.name) });
    }

    std::wstring prompt;
    if (targets.size() == 1)
        prompt = L"Delete  '" + baseName(targets[0].fullPath) + L"' ?";
    else
        prompt = L"Delete " + std::to_wstring(targets.size()) + L" marked items ?";
    if (!confirmYesNo(prompt)) return;

    int done = 0, failed = 0;
    std::wstring lastErr;
    for (auto &t : targets) {
        std::wstring err;
        if (deleteRecursive(t.fullPath, t.isDir, err)) { done++; unmark(t.fullPath); }
        else { failed++; if (!err.empty()) lastErr = err; }
    }

    loadDir(g_panel[0]);
    loadDir(g_panel[1]);
    if (failed) {
        render(L"Deleted " + std::to_wstring(done) + L", failed " + std::to_wstring(failed) + L"  \x2014  " + lastErr);
        flush();
        Sleep(1600);
    }
}

static void doMkdir() {
    Panel &p = g_panel[g_active];
    std::wstring name = promptLine(L"New folder name: ");
    if (name.empty()) return;
    if (!CreateDirectoryW(joinPath(p.path, name).c_str(), NULL)) {
        render(L"Cannot create folder: " + winErr(GetLastError()));
        flush();
        Sleep(1400);
    }
    loadDir(p);
}

static void enterEntry() {
    Panel &p = g_panel[g_active];
    if (p.entries.empty()) return;
    Entry &e = p.entries[p.cursor];
    if (e.name == L"..") {
        std::wstring child = baseName(p.path);
        p.path = parentDir(p.path);
        loadDir(p);
        for (int i = 0; i < (int)p.entries.size(); i++) {
            if (_wcsicmp(p.entries[i].name.c_str(), child.c_str()) == 0) { p.cursor = i; break; }
        }
        return;
    }
    if (e.isDir) {
        p.path = joinPath(p.path, e.name);
        p.cursor = 0;
        loadDir(p);
    } else {
        viewFile(joinPath(p.path, e.name));
    }
}

// ============================================================
//  ENTRY POINT & MAIN LOOP
// ============================================================
int wmain() {
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    SetConsoleOutputCP(CP_UTF8);

    DWORD outMode = 0;
    GetConsoleMode(g_out, &outMode);
    SetConsoleMode(g_out, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
    DWORD wr;
    const wchar_t *enterSeq = L"\x1b[?1049h\x1b[?7l\x1b[2J\x1b[H";
    WriteConsoleW(g_out, enterSeq, (DWORD)wcslen(enterSeq), &wr, NULL);

    CONSOLE_CURSOR_INFO cci;
    GetConsoleCursorInfo(g_out, &cci);
    cci.bVisible = FALSE;
    SetConsoleCursorInfo(g_out, &cci);

    loadTheme();
    applyTheme(g_theme);

    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    g_panel[0].path = cwd;
    g_panel[0].cursor = 0; g_panel[0].top = 0; g_panel[0].detailed = true;
    g_panel[1].path = cwd;
    g_panel[1].cursor = 0; g_panel[1].top = 0; g_panel[1].detailed = true;

    queryConsole();
    loadDir(g_panel[0]);
    loadDir(g_panel[1]);

    loadArt();
    splashScreen();

    bool run = true;
    std::wstring status;
    while (run) {
        queryConsole();
        render(status);
        status.clear();

        INPUT_RECORD ir;
        DWORD n;
        if (!ReadConsoleInputW(g_in, &ir, 1, &n)) break;
        if (ir.EventType == WINDOW_BUFFER_SIZE_EVENT) continue;
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;

        WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;
        Panel &p = g_panel[g_active];
        int last = (int)p.entries.size() - 1;
        int pageH = g_h - 4;

        switch (vk) {
            case VK_ESCAPE:
            case VK_F10:
                run = false;
                break;
            case VK_TAB:
            case VK_LEFT:
            case VK_RIGHT:
                g_active ^= 1;
                break;
            case VK_UP:
                p.cursor -= accelStep(VK_UP);
                if (p.cursor < 0) p.cursor = 0;
                break;
            case VK_DOWN:
                p.cursor += accelStep(VK_DOWN);
                if (p.cursor > last) p.cursor = last;
                break;
            case VK_PRIOR:
                p.cursor -= pageH; if (p.cursor < 0) p.cursor = 0;
                break;
            case VK_NEXT:
                p.cursor += pageH; if (p.cursor > last) p.cursor = last;
                break;
            case VK_HOME:
                p.cursor = 0;
                break;
            case VK_END:
                p.cursor = last;
                break;
            case VK_RETURN:
                enterEntry();
                break;
            case VK_BACK:
                if (!isDriveRoot(p.path)) {
                    std::wstring child = baseName(p.path);
                    p.path = parentDir(p.path);
                    loadDir(p);
                    for (int i = 0; i < (int)p.entries.size(); i++)
                        if (_wcsicmp(p.entries[i].name.c_str(), child.c_str()) == 0) { p.cursor = i; break; }
                }
                break;
            case VK_SPACE:
            case VK_INSERT:
                if (!p.entries.empty() && p.entries[p.cursor].name != L"..") {
                    std::wstring full = joinPath(p.path, p.entries[p.cursor].name);
                    toggleMark(full, p.entries[p.cursor].isDir);
                    status = isMarked(full) ? (L"Marked (" + std::to_wstring(g_marks.size()) + L"): " + full)
                                            : (L"Unmarked (" + std::to_wstring(g_marks.size()) + L" left)");
                    if (p.cursor < last) p.cursor++;
                }
                break;
            case VK_DELETE:
            case VK_F8:
                doDelete();
                break;
            case VK_F1:
                doPaste(false);
                break;
            case VK_F2:
                doPaste(true);
                break;
            case VK_F3:
                if (!p.entries.empty() && !p.entries[p.cursor].isDir)
                    viewFile(joinPath(p.path, p.entries[p.cursor].name));
                break;
            case VK_F4:
                p.detailed = !p.detailed;
                break;
            case VK_F5:
                doDownload(0);
                break;
            case VK_F6:
                doDownload(1);
                break;
            case VK_F7:
                doMkdir();
                break;
            case VK_F9:
                settingsScreen();
                break;
            case VK_F12:
                helpScreen();
                break;
            default:
                break;
        }
    }

    cci.bVisible = TRUE;
    SetConsoleCursorInfo(g_out, &cci);
    const wchar_t *exitSeq = L"\x1b[0m\x1b[?7h\x1b[?1049l";
    WriteConsoleW(g_out, exitSeq, (DWORD)wcslen(exitSeq), &wr, NULL);
    return 0;
}
