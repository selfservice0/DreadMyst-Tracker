#include "resource.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <cwchar>

// Global paths for extracted files
wchar_t g_extractedDir[MAX_PATH] = {0};
wchar_t g_dllPath[MAX_PATH] = {0};
wchar_t g_injectorPath[MAX_PATH] = {0};
wchar_t g_unloaderPath[MAX_PATH] = {0};

// Extract a resource to a file
bool ExtractResource(HMODULE hModule, int resourceId,
                     const wchar_t *outputPath) {
  HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCEW(resourceId),
                             MAKEINTRESOURCEW(256));
  if (!hRes)
    return false;

  HGLOBAL hData = LoadResource(hModule, hRes);
  if (!hData)
    return false;

  DWORD size = SizeofResource(hModule, hRes);
  void *data = LockResource(hData);
  if (!data || size == 0)
    return false;

  HANDLE hFile = CreateFileW(outputPath, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE)
    return false;

  DWORD written;
  WriteFile(hFile, data, size, &written, nullptr);
  CloseHandle(hFile);

  return written == size;
}

// Extract all embedded files to temp directory
bool ExtractEmbeddedFiles() {
  // Get temp directory
  wchar_t tempDir[MAX_PATH];
  GetTempPathW(MAX_PATH, tempDir);
  wcscat_s(tempDir, L"DreadmystTracker\\");

  // Create directory if needed
  CreateDirectoryW(tempDir, nullptr);
  wcscpy_s(g_extractedDir, tempDir);

  // Build paths
  wcscpy_s(g_dllPath, tempDir);
  wcscat_s(g_dllPath, L"DreadmystTracker.dll");

  wcscpy_s(g_injectorPath, tempDir);
  wcscat_s(g_injectorPath, L"Injector.exe");

  wcscpy_s(g_unloaderPath, tempDir);
  wcscat_s(g_unloaderPath, L"Unloader.exe");

  HMODULE hModule = GetModuleHandleW(nullptr);

  // Extract files (only if not already present or different size)
  bool ok = true;
  ok = ExtractResource(hModule, IDR_DLL_TRACKER, g_dllPath) && ok;
  ok = ExtractResource(hModule, IDR_EXE_INJECTOR, g_injectorPath) && ok;
  ok = ExtractResource(hModule, IDR_EXE_UNLOADER, g_unloaderPath) && ok;

  return ok;
}

// Shared memory structure
#define TRACKER_SHARED_MEMORY_NAME "DreadmystTrackerSharedMemory"
#define TRACKER_MUTEX_NAME "DreadmystTrackerMutex"

struct SharedTrackerData {
  uint32_t magic;
  int totalKills;
  int totalLootItems;
  int64_t totalGold;
  int totalExp;
  int64_t goldSpent;
  int64_t totalDamage;
  int partyKills;
  int partyLootItems;
  int64_t partyGold;
  int partyExp;
  int lootByQuality[6];
  struct RecentLoot {
    char itemName[64];
    uint8_t quality;
    int amount;
    int64_t timestamp;
  } recentLoot[10];
  int recentLootIndex;
  struct RecentKill {
    char mobName[64];
    int expGained;
    int64_t timestamp;
  } recentKills[10];
  int recentKillIndex;
  bool overlayVisible;
  int64_t sessionStartTime;
  char debugText[512];
  // Chat filter settings
  bool chatFilterEnabled;
  char chatFilterTerms[512];
  bool blockLinkedItems; // Block messages containing item links
  bool useRegexFilter;   // Use regex matching instead of simple substring
};

// Tab IDs - Chat before Debug
enum Tab { TAB_STATS = 0, TAB_LOOT = 1, TAB_FILTER = 2, TAB_DEBUG = 3 };
static int g_activeTab = TAB_STATS;
static int g_hoverTab = -1;

// Tab button rectangles
static RECT g_tabRects[4];

// Global state
HWND g_hwnd = nullptr;
HANDLE g_sharedMem = nullptr;
HANDLE g_mutex = nullptr;
SharedTrackerData *g_data = nullptr;
bool g_dragging = false;
POINT g_dragStart = {0, 0};
POINT g_windowStart = {0, 0};
int64_t g_guiStartTime = 0; // Session start for DPS calculation

// Quality colors
const COLORREF QUALITY_COLORS[] = {
    RGB(128, 128, 128), // QualityLv0 - Grey
    RGB(255, 255, 255), // QualityLv1 - White
    RGB(30, 255, 0),    // QualityLv2 - Green
    RGB(0, 112, 221),   // QualityLv3 - Blue
    RGB(255, 0, 127),   // QualityLv4 - Pink
    RGB(163, 53, 238)   // QualityLv5 - Purple
};

const wchar_t *QUALITY_NAMES[] = {L"Junk", L"Common", L"Uncommon",
                                  L"Rare", L"Epic",   L"Legendary"};

// Colors
const COLORREF CLR_BG = RGB(18, 18, 28);
const COLORREF CLR_HEADER = RGB(35, 35, 55);
const COLORREF CLR_TAB_ACTIVE = RGB(60, 60, 100);
const COLORREF CLR_TAB_HOVER = RGB(50, 50, 80);
const COLORREF CLR_TAB_NORMAL = RGB(30, 30, 50);
const COLORREF CLR_BORDER = RGB(70, 70, 110);
const COLORREF CLR_GOLD = RGB(255, 215, 0);
const COLORREF CLR_TEXT = RGB(200, 200, 200);
const COLORREF CLR_TEXT_DIM = RGB(140, 140, 140);

// Forward declarations for filter state (definitions later in file)
static char g_filterTerms[512];
static HWND g_hFilterEdit;

// Connect to shared memory
bool ConnectSharedMemory() {
  g_sharedMem =
      OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, TRACKER_SHARED_MEMORY_NAME);
  if (!g_sharedMem)
    return false;

  g_data = static_cast<SharedTrackerData *>(MapViewOfFile(
      g_sharedMem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedTrackerData)));
  if (!g_data) {
    CloseHandle(g_sharedMem);
    g_sharedMem = nullptr;
    return false;
  }

  // Set default filter terms if empty
  if (g_data->chatFilterTerms[0] == '\0') {
    strcpy_s(g_data->chatFilterTerms, sizeof(g_data->chatFilterTerms),
             "wts, wtb, wtt, sell, offer, cheap, obo, \\[.*\\]");
  }

  // Sync local filter terms from shared memory
  strcpy_s(g_filterTerms, sizeof(g_filterTerms), g_data->chatFilterTerms);
  if (g_hFilterEdit) {
    SetWindowTextA(g_hFilterEdit, g_filterTerms);
  }

  g_mutex = OpenMutexA(SYNCHRONIZE, FALSE, TRACKER_MUTEX_NAME);
  return true;
}

void DisconnectSharedMemory() {
  if (g_data) {
    UnmapViewOfFile(g_data);
    g_data = nullptr;
  }
  if (g_sharedMem) {
    CloseHandle(g_sharedMem);
    g_sharedMem = nullptr;
  }
  if (g_mutex) {
    CloseHandle(g_mutex);
    g_mutex = nullptr;
  }
}

// Edit control ID for filter input
#define IDC_FILTER_EDIT 1001
#define IDC_FILTER_APPLY 1002
#define IDC_BLOCK_ITEMS_CHECK 1003
#define IDC_USE_REGEX_CHECK 1004

// Global edit HWND
static HWND g_hApplyButton = nullptr;
static HWND g_hBlockItemsCheck = nullptr;
static HWND g_hUseRegexCheck = nullptr;

// Draw a filled rounded-ish rectangle
void FillRoundRect(HDC hdc, RECT *r, COLORREF color) {
  HBRUSH brush = CreateSolidBrush(color);
  FillRect(hdc, r, brush);
  DeleteObject(brush);
}

// Draw tab button
void DrawTab(HDC hdc, int tabIdx, const wchar_t *text, int x, int y, int w,
             int h) {
  COLORREF color = CLR_TAB_NORMAL;
  if (tabIdx == g_activeTab)
    color = CLR_TAB_ACTIVE;
  else if (tabIdx == g_hoverTab)
    color = CLR_TAB_HOVER;

  RECT r = {x, y, x + w, y + h};
  g_tabRects[tabIdx] = r;
  FillRoundRect(hdc, &r, color);

  // Border for active tab
  if (tabIdx == g_activeTab) {
    HPEN pen = CreatePen(PS_SOLID, 1, CLR_GOLD);
    SelectObject(hdc, pen);
    MoveToEx(hdc, x, y + h - 1, nullptr);
    LineTo(hdc, x + w, y + h - 1);
    DeleteObject(pen);
  }

  SetTextColor(hdc, tabIdx == g_activeTab ? CLR_GOLD : CLR_TEXT);
  DrawTextW(hdc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// Helper to draw emoji character with Segoe UI Emoji font, then text with
// regular font
void DrawEmojiText(HDC hdc, int x, int y, const wchar_t *emoji,
                   const wchar_t *text, HFONT regularFont) {
  // Create emoji font
  HFONT emojiFont =
      CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Emoji");

  // Draw emoji
  SelectObject(hdc, emojiFont);
  TextOutW(hdc, x, y, emoji, (int)wcslen(emoji));

  // Get emoji width
  SIZE emojiSize;
  GetTextExtentPoint32W(hdc, emoji, (int)wcslen(emoji), &emojiSize);

  // Draw text with regular font
  SelectObject(hdc, regularFont);
  TextOutW(hdc, x + emojiSize.cx + 2, y, text, (int)wcslen(text));

  DeleteObject(emojiFont);
}

// Draw Stats tab content
void DrawStatsTab(HDC hdc, int startY, RECT *rc) {
  wchar_t buf[256];
  int y = startY;

  // Create regular content font
  HFONT contentFont =
      CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  SelectObject(hdc, contentFont);

  if (g_data && g_data->magic == 0xDEADBEEF) {
    // Kills
    SetTextColor(hdc, RGB(255, 100, 100));
    wsprintfW(buf, L"Kills: %d", g_data->totalKills);
    DrawEmojiText(hdc, 15, y, L"\u2694", buf, contentFont);
    y += 22;

    // Loot
    SetTextColor(hdc, RGB(100, 255, 100));
    wsprintfW(buf, L"Loot: %d items", g_data->totalLootItems);
    DrawEmojiText(hdc, 15, y, L"\U0001F4E6", buf, contentFont);
    y += 22;

    // Gold
    SetTextColor(hdc, CLR_GOLD);
    wsprintfW(buf, L"Gold: %I64d", g_data->totalGold);
    DrawEmojiText(hdc, 15, y, L"\U0001F4B0", buf, contentFont);
    y += 22;

    // Spent (repair costs)
    SetTextColor(hdc, RGB(255, 100, 100));
    wsprintfW(buf, L"Spent: %I64d", g_data->goldSpent);
    DrawEmojiText(hdc, 15, y, L"\U0001F4B8", buf,
                  contentFont); // 💸 money with wings
    y += 22;

    // Exp
    SetTextColor(hdc, RGB(138, 43, 226));
    wsprintfW(buf, L"Exp: %d", g_data->totalExp);
    DrawEmojiText(hdc, 15, y, L"\u2728", buf, contentFont);
    y += 22;

    // Performance metrics (Kills/Min, XP/Hour, DPS)
    // Use global start time tracked by GUI
    if (g_guiStartTime == 0)
      g_guiStartTime = GetTickCount64();
    int64_t nowMs = GetTickCount64();
    int64_t sessionMs = nowMs - g_guiStartTime;
    if (sessionMs < 1000)
      sessionMs = 1000; // Avoid divide by zero

    double sessionSec = sessionMs / 1000.0;
    double sessionMin = sessionMs / 60000.0;
    double sessionHr = sessionMs / 3600000.0;
    double killsPerMin =
        g_data->totalKills / (sessionMin > 0.01 ? sessionMin : 0.01);
    double xpPerHour =
        g_data->totalExp / (sessionHr > 0.001 ? sessionHr : 0.001);

    // DPS from actual damage only (no XP fallback)
    double dps = g_data->totalDamage / sessionSec;
    double dph = dps * 3600.0;

    SetTextColor(hdc, RGB(150, 200, 255));
    _snwprintf_s(buf, 256, _TRUNCATE, L"%.1f kills/min  |  %.0f xp/hr",
                 killsPerMin, xpPerHour);
    TextOutW(hdc, 15, y, buf, (int)wcslen(buf));
    y += 18;

    // DPS display - always show DPS, with 0 if no damage tracked
    SetTextColor(hdc, RGB(255, 150, 50)); // Orange for DPS
    _snwprintf_s(buf, 256, _TRUNCATE, L"DPS: %.1f  |  Dmg: %I64d", dps,
                 g_data->totalDamage);
    TextOutW(hdc, 15, y, buf, (int)wcslen(buf));
    y += 18;
    y += 6;

    // Quality breakdown (compact)
    SetTextColor(hdc, CLR_TEXT_DIM);
    TextOutW(hdc, 15, y, L"Quality Breakdown:", 18);
    y += 18;

    int col = 0;
    for (int i = 0; i < 6; i++) {
      if (g_data->lootByQuality[i] > 0) {
        SetTextColor(hdc, QUALITY_COLORS[i]);
        wsprintfW(buf, L"%s:%d", QUALITY_NAMES[i], g_data->lootByQuality[i]);
        TextOutW(hdc, 20 + (col % 2) * 110, y, buf, (int)wcslen(buf));
        if (col % 2 == 1)
          y += 16;
        col++;
      }
    }
    if (col % 2 == 1)
      y += 16;

  } else {
    SetTextColor(hdc, RGB(255, 80, 80));
    TextOutW(hdc, 15, y, L"Waiting for game...", 19);
    y += 22;
    SetTextColor(hdc, CLR_TEXT_DIM);
    TextOutW(hdc, 15, y, L"Inject DLL first!", 17);
  }
  DeleteObject(contentFont);
}

// Draw Loot tab content
void DrawLootTab(HDC hdc, int startY, RECT *rc) {
  wchar_t buf[256];
  int y = startY;

  if (g_data && g_data->magic == 0xDEADBEEF) {
    SetTextColor(hdc, CLR_TEXT_DIM);
    TextOutW(hdc, 15, y, L"Recent Loot:", 12);
    y += 20;

    bool hasLoot = false;
    for (int i = 0; i < 10; i++) {
      int idx = (g_data->recentLootIndex - 1 - i + 10) % 10;
      if (g_data->recentLoot[idx].itemName[0] != 0) {
        hasLoot = true;
        uint8_t q = g_data->recentLoot[idx].quality;
        if (q > 5)
          q = 1;
        SetTextColor(hdc, QUALITY_COLORS[q]);

        // Convert item name
        wchar_t wname[64];
        MultiByteToWideChar(CP_ACP, 0, g_data->recentLoot[idx].itemName, -1,
                            wname, 64);

        int amt = g_data->recentLoot[idx].amount;
        if (amt > 1)
          wsprintfW(buf, L"- %s x%d", wname, amt);
        else
          wsprintfW(buf, L"- %s", wname);

        TextOutW(hdc, 15, y, buf, (int)wcslen(buf));
        y += 18;

        if (y > rc->bottom - 20)
          break;
      }
    }

    if (!hasLoot) {
      SetTextColor(hdc, CLR_TEXT_DIM);
      TextOutW(hdc, 15, y, L"No loot yet...", 14);
    }
  } else {
    SetTextColor(hdc, CLR_TEXT_DIM);
    TextOutW(hdc, 15, y, L"Not connected", 13);
  }
}

// Draw Debug tab content
void DrawDebugTab(HDC hdc, int startY, RECT *rc) {
  int y = startY;

  if (g_data && g_data->magic == 0xDEADBEEF) {
    SetTextColor(hdc, CLR_TEXT_DIM);
    if (g_data->debugText[0] != 0) {
      char *line = g_data->debugText;
      for (int lineNum = 0; lineNum < 10 && *line; lineNum++) {
        char *lineEnd = line;
        while (*lineEnd && *lineEnd != '\n')
          lineEnd++;

        wchar_t wline[256];
        int len = MultiByteToWideChar(CP_ACP, 0, line, (int)(lineEnd - line),
                                      wline, 255);
        wline[len] = 0;
        TextOutW(hdc, 15, y, wline, len);
        y += 16;

        if (*lineEnd == '\n')
          lineEnd++;
        line = lineEnd;
        if (y > rc->bottom - 20)
          break;
      }
    } else {
      TextOutW(hdc, 15, y, L"No debug output", 15);
    }
  } else {
    SetTextColor(hdc, CLR_TEXT_DIM);
    TextOutW(hdc, 15, y, L"Not connected", 13);
  }
}

// Filter state (local to GUI)
static bool g_filterEnabled = false;
static RECT g_toggleButtonRect = {0};
static RECT g_applyButtonRect = {0};
static HWND g_filterEditHwnd = nullptr;
static bool g_filterControlsCreated = false;

// Create filter controls on the window
void CreateFilterControls(HWND hwnd) {
  if (g_filterControlsCreated)
    return;

  // Create edit control for filter terms - wider box
  g_hFilterEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_filterTerms,
                                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 15,
                                  90, 265, 22, hwnd, (HMENU)IDC_FILTER_EDIT,
                                  GetModuleHandle(nullptr), nullptr);

  // Create Apply button - next to edit box
  g_hApplyButton = CreateWindowExA(
      0, "BUTTON", "Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 220, 115, 60,
      24, hwnd, (HMENU)IDC_FILTER_APPLY, GetModuleHandle(nullptr), nullptr);

  // Create Use Regex checkbox - first checkbox
  g_hUseRegexCheck = CreateWindowExA(
      0, "BUTTON", "Use Regex", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 15,
      115, 100, 20, hwnd, (HMENU)IDC_USE_REGEX_CHECK, GetModuleHandle(nullptr),
      nullptr);

  // Create Block Linked Items checkbox - second checkbox
  g_hBlockItemsCheck = CreateWindowExA(
      0, "BUTTON", "Block Item Links", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
      115, 115, 100, 20, hwnd, (HMENU)IDC_BLOCK_ITEMS_CHECK,
      GetModuleHandle(nullptr), nullptr);

  // Set font for controls
  HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  SendMessage(g_hFilterEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
  SendMessage(g_hApplyButton, WM_SETFONT, (WPARAM)hFont, TRUE);
  SendMessage(g_hUseRegexCheck, WM_SETFONT, (WPARAM)hFont, TRUE);
  SendMessage(g_hBlockItemsCheck, WM_SETFONT, (WPARAM)hFont, TRUE);

  // Set Use Regex checked by default
  SendMessage(g_hUseRegexCheck, BM_SETCHECK, BST_CHECKED, 0);

  g_filterControlsCreated = true;
}

// Show/hide filter controls based on active tab
void UpdateFilterControlsVisibility(int activeTab) {
  if (g_hFilterEdit) {
    ShowWindow(g_hFilterEdit, activeTab == TAB_FILTER ? SW_SHOW : SW_HIDE);
  }
  if (g_hApplyButton) {
    ShowWindow(g_hApplyButton, activeTab == TAB_FILTER ? SW_SHOW : SW_HIDE);
  }
  if (g_hBlockItemsCheck) {
    ShowWindow(g_hBlockItemsCheck, activeTab == TAB_FILTER ? SW_SHOW : SW_HIDE);
  }
  if (g_hUseRegexCheck) {
    ShowWindow(g_hUseRegexCheck, activeTab == TAB_FILTER ? SW_SHOW : SW_HIDE);
  }
}

// Draw Filter tab content
void DrawFilterTab(HDC hdc, int startY, RECT *rc) {
  int y = startY;

  // Create regular content font
  HFONT contentFont =
      CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  SelectObject(hdc, contentFont);

  SetTextColor(hdc, CLR_TEXT);
  TextOutW(hdc, 15, y, L"Filter Terms (comma-separated):", 32);
  y += 20;

  // Draw text input area background
  RECT editRect = {15, y, rc->right - 15, y + 50};
  FillRoundRect(hdc, &editRect, RGB(30, 30, 50));
  HPEN borderPen = CreatePen(PS_SOLID, 1, CLR_BORDER);
  SelectObject(hdc, borderPen);
  SelectObject(hdc, GetStockObject(NULL_BRUSH));
  Rectangle(hdc, editRect.left, editRect.top, editRect.right, editRect.bottom);
  DeleteObject(borderPen);

  // Display current filter terms
  if (g_filterTerms[0]) {
    wchar_t wTerms[512];
    MultiByteToWideChar(CP_ACP, 0, g_filterTerms, -1, wTerms, 512);
    SetTextColor(hdc, CLR_TEXT);
    RECT textRect = {editRect.left + 5, editRect.top + 5, editRect.right - 5,
                     editRect.bottom - 5};
    DrawTextW(hdc, wTerms, -1, &textRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
  } else {
    SetTextColor(hdc, CLR_TEXT_DIM);
    TextOutW(hdc, editRect.left + 5, editRect.top + 5,
             L"(Click to edit filters)", 23);
  }
  y += 60;

  // Skip space for Win32 controls (checkboxes are at y=115)
  y = startY + 90;

  // Draw ON/OFF toggle button
  COLORREF toggleColor = g_filterEnabled ? RGB(50, 180, 50) : RGB(100, 50, 50);
  const wchar_t *toggleText =
      g_filterEnabled ? L"  FILTER ON" : L"  FILTER OFF";
  g_toggleButtonRect = {15, y, 120, y + 30};
  FillRoundRect(hdc, &g_toggleButtonRect, toggleColor);

  // Button border
  borderPen = CreatePen(PS_SOLID, 1,
                        g_filterEnabled ? RGB(80, 220, 80) : RGB(150, 80, 80));
  SelectObject(hdc, borderPen);
  Rectangle(hdc, g_toggleButtonRect.left, g_toggleButtonRect.top,
            g_toggleButtonRect.right, g_toggleButtonRect.bottom);
  DeleteObject(borderPen);

  // Button text
  SetTextColor(hdc, RGB(255, 255, 255));
  DrawTextW(hdc, toggleText, -1, &g_toggleButtonRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  y += 40;

  // Status
  SetTextColor(hdc, CLR_TEXT_DIM);
  if (g_data && g_data->magic == 0xDEADBEEF) {
    TextOutW(hdc, 15, y, L"Connected to game", 17);
  } else {
    TextOutW(hdc, 15, y, L"Waiting for game...", 19);
  }

  DeleteObject(contentFont);
}

// Window procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
  case WM_CREATE:
    CreateFilterControls(hwnd);
    UpdateFilterControlsVisibility(g_activeTab);
    return 0;

  case WM_COMMAND:
    if (LOWORD(wParam) == IDC_FILTER_APPLY) {
      // Get text from edit control
      if (g_hFilterEdit) {
        GetWindowTextA(g_hFilterEdit, g_filterTerms, sizeof(g_filterTerms));

        // Update shared memory
        if (g_data && g_data->magic == 0xDEADBEEF) {
          strcpy_s(g_data->chatFilterTerms, g_filterTerms);
        }

        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    if (LOWORD(wParam) == IDC_BLOCK_ITEMS_CHECK) {
      // Toggle block linked items
      if (g_data && g_data->magic == 0xDEADBEEF) {
        bool checked =
            (SendMessage(g_hBlockItemsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
        g_data->blockLinkedItems = checked;
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    if (LOWORD(wParam) == IDC_USE_REGEX_CHECK) {
      // Toggle use regex filter
      if (g_data && g_data->magic == 0xDEADBEEF) {
        bool checked =
            (SendMessage(g_hUseRegexCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
        g_data->useRegexFilter = checked;
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    break;

  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    // Background
    HBRUSH bgBrush = CreateSolidBrush(CLR_BG);
    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);

    // Header gradient-ish
    RECT header = {0, 0, rc.right, 32};
    FillRoundRect(hdc, &header, CLR_HEADER);

    // Border
    HPEN borderPen = CreatePen(PS_SOLID, 1, CLR_BORDER);
    SelectObject(hdc, borderPen);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, 0, 0, rc.right, rc.bottom);
    DeleteObject(borderPen);

    SetBkMode(hdc, TRANSPARENT);

    // Title
    HFONT titleFont =
        CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SelectObject(hdc, titleFont);
    SetTextColor(hdc, CLR_GOLD);
    TextOutW(hdc, 12, 7, L"Dreadmyst Tracker", 17);
    DeleteObject(titleFont);

    // Tab buttons - use Segoe UI Symbol for icon support
    HFONT tabFont = CreateFontW(12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                                L"Segoe UI Symbol");
    SelectObject(hdc, tabFont);

    int tabW = 65;
    int tabH = 24;
    int tabY = 38;
    DrawTab(hdc, TAB_STATS, L"\x2694 Stats", 10, tabY, tabW,
            tabH); // Crossed swords
    DrawTab(hdc, TAB_LOOT, L"\x2666 Loot", 80, tabY, tabW, tabH);    // Diamond
    DrawTab(hdc, TAB_FILTER, L"\x2709 Chat", 150, tabY, tabW, tabH); // Envelope
    DrawTab(hdc, TAB_DEBUG, L"\x2699 Debug", 220, tabY, tabW, tabH); // Gear
    DeleteObject(tabFont);

    // Content area
    HFONT contentFont =
        CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SelectObject(hdc, contentFont);

    int contentY = tabY + tabH + 10;

    switch (g_activeTab) {
    case TAB_STATS:
      DrawStatsTab(hdc, contentY, &rc);
      break;
    case TAB_LOOT:
      DrawLootTab(hdc, contentY, &rc);
      break;
    case TAB_DEBUG:
      DrawDebugTab(hdc, contentY, &rc);
      break;
    case TAB_FILTER:
      DrawFilterTab(hdc, contentY, &rc);
      break;
    }

    DeleteObject(contentFont);
    EndPaint(hwnd, &ps);
    return 0;
  }

  case WM_TIMER:
    if (!g_data || g_data->magic != 0xDEADBEEF) {
      DisconnectSharedMemory();
      ConnectSharedMemory();
    }
    // Sync local filter terms display with shared memory
    if (g_data && g_data->magic == 0xDEADBEEF) {
      if (strcmp(g_filterTerms, g_data->chatFilterTerms) != 0) {
        strcpy_s(g_filterTerms, sizeof(g_filterTerms), g_data->chatFilterTerms);
        if (g_hFilterEdit) {
          SetWindowTextA(g_hFilterEdit, g_filterTerms);
        }
      }
      // Sync filter enabled state
      g_filterEnabled = g_data->chatFilterEnabled;
      // Sync block items checkbox
      if (g_hBlockItemsCheck) {
        SendMessage(g_hBlockItemsCheck, BM_SETCHECK,
                    g_data->blockLinkedItems ? BST_CHECKED : BST_UNCHECKED, 0);
      }
      // Sync regex checkbox
      if (g_hUseRegexCheck) {
        SendMessage(g_hUseRegexCheck, BM_SETCHECK,
                    g_data->useRegexFilter ? BST_CHECKED : BST_UNCHECKED, 0);
      }
    }
    InvalidateRect(hwnd, nullptr, FALSE);
    return 0;

  case WM_MOUSEMOVE: {
    POINT pt = {LOWORD(lParam), HIWORD(lParam)};

    // Check tab hover
    int newHover = -1;
    for (int i = 0; i < 4; i++) {
      if (PtInRect(&g_tabRects[i], pt)) {
        newHover = i;
        break;
      }
    }
    if (newHover != g_hoverTab) {
      g_hoverTab = newHover;
      InvalidateRect(hwnd, nullptr, FALSE);
    }

    // Dragging
    if (g_dragging) {
      POINT cursor;
      GetCursorPos(&cursor);
      SetWindowPos(hwnd, nullptr, g_windowStart.x + (cursor.x - g_dragStart.x),
                   g_windowStart.y + (cursor.y - g_dragStart.y), 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER);
    }
    return 0;
  }

  case WM_LBUTTONDOWN: {
    POINT pt = {LOWORD(lParam), HIWORD(lParam)};

    // Check tab click
    for (int i = 0; i < 4; i++) {
      if (PtInRect(&g_tabRects[i], pt)) {
        g_activeTab = i;
        UpdateFilterControlsVisibility(g_activeTab);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
    }

    // Filter tab click handling
    if (g_activeTab == TAB_FILTER) {
      // Check toggle button click
      if (PtInRect(&g_toggleButtonRect, pt)) {
        g_filterEnabled = !g_filterEnabled;
        // Update shared memory
        if (g_data && g_data->magic == 0xDEADBEEF) {
          g_data->chatFilterEnabled = g_filterEnabled;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }

      // Check filter text area click - focus the edit control
      if (pt.y >= 90 && pt.y <= 140 && g_hFilterEdit) {
        SetFocus(g_hFilterEdit);
        return 0;
      }
    }

    // Start drag
    g_dragging = true;
    SetCapture(hwnd);
    GetCursorPos(&g_dragStart);
    RECT wr;
    GetWindowRect(hwnd, &wr);
    g_windowStart.x = wr.left;
    g_windowStart.y = wr.top;
    return 0;
  }

  case WM_LBUTTONUP:
    g_dragging = false;
    ReleaseCapture();
    return 0;

  case WM_MOUSELEAVE:
    g_hoverTab = -1;
    InvalidateRect(hwnd, nullptr, FALSE);
    return 0;

  case WM_RBUTTONUP: {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Reset Stats");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, L"Unload DLL");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, L"Exit (Unload && Close)");

    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y,
                             0, hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == 1) {
      // Reset Stats - reset session time and all counters
      g_guiStartTime = GetTickCount64(); // Reset GUI timer
      if (g_data && g_data->magic == 0xDEADBEEF) {
        g_data->totalKills = 0;
        g_data->totalLootItems = 0;
        g_data->totalGold = 0;
        g_data->totalExp = 0;
        g_data->goldSpent = 0;
        g_data->totalDamage = 0;
        g_data->partyKills = 0;
        g_data->partyLootItems = 0;
        g_data->partyGold = 0;
        g_data->partyExp = 0;
        for (int i = 0; i < 6; i++)
          g_data->lootByQuality[i] = 0;
        g_data->sessionStartTime = GetTickCount64();
      }
      InvalidateRect(hwnd, nullptr, FALSE);
    } else if (cmd == 2 || cmd == 3) {
      // Run extracted Unloader.exe
      STARTUPINFOW si = {sizeof(si)};
      PROCESS_INFORMATION pi;
      if (CreateProcessW(g_unloaderPath, nullptr, nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 3000); // Wait up to 3 sec
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
      }
      if (cmd == 2)
        PostQuitMessage(0);
    }
    return 0;
  }

  case WM_DESTROY:
    DisconnectSharedMemory();
    PostQuitMessage(0);
    return 0;

  default:
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
  // Extract embedded files (DLL, Injector, Unloader) to temp
  if (!ExtractEmbeddedFiles()) {
    // Fallback: try using files in same directory
    wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    wchar_t *lastSlash = wcsrchr(exeDir, L'\\');
    if (lastSlash) {
      *(lastSlash + 1) = 0;
      wcscpy_s(g_extractedDir, exeDir);
      wcscpy_s(g_dllPath, exeDir);
      wcscat_s(g_dllPath, L"DreadmystTracker.dll");
      wcscpy_s(g_injectorPath, exeDir);
      wcscat_s(g_injectorPath, L"Injector.exe");
      wcscpy_s(g_unloaderPath, exeDir);
      wcscat_s(g_unloaderPath, L"Unloader.exe");
    }
  }

  // Auto-inject DLL at startup using extracted Injector
  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi;
  // Pass DLL path as argument to injector
  wchar_t cmdLine[MAX_PATH * 2];
  _snwprintf_s(cmdLine, MAX_PATH * 2, _TRUNCATE, L"\"%s\" \"%s\"",
               g_injectorPath, g_dllPath);
  if (CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    WaitForSingleObject(pi.hProcess, 5000); // Wait up to 5 sec for inject
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInstance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"DreadmystTrackerGUI";
  RegisterClassExW(&wc);

  g_hwnd =
      CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                      L"DreadmystTrackerGUI", L"Dreadmyst Tracker", WS_POPUP,
                      100, 100, 300, 380, nullptr, nullptr, hInstance, nullptr);

  if (!g_hwnd)
    return 1;

  SetLayeredWindowAttributes(g_hwnd, 0, 240, LWA_ALPHA);
  ShowWindow(g_hwnd, SW_SHOW);
  UpdateWindow(g_hwnd);

  ConnectSharedMemory();
  SetTimer(g_hwnd, 1, 100, nullptr);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  return (int)msg.wParam;
}
