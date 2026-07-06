// ==WindhawkMod==
// @id              macos-shake-to-find-cursor
// @name            macOS Shake to Find Cursor
// @description     Smoothly enlarges the mouse cursor when shaken back and forth system-wide.
// @version         0.5
// @author          samiulislam16
// @github          https://github.com/samiulislam16
// @include         *
// @compilerOptions -luser32 -lgdi32 -lshcore
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Shake to Find Cursor

Brings the beloved macOS "Shake mouse pointer to locate" feature to Windows!

When you rapidly shake your mouse back and forth, the cursor temporarily
grows larger to help you spot it on screen. Once you stop shaking, it
smoothly animates back to its normal size.

## How to Use
1. Shake your mouse rapidly left-right (or up-down)
2. The cursor enlarges so you can find it easily
3. Stop moving and it shrinks back to normal
4. Restart explorer if needed

## Settings
- *Cursor scale* — how large the cursor grows (e.g. 350 = 3.5×)
- *Enlarge duration* — how long the cursor stays big after shaking stops (ms)
- *Shake sensitivity* — minimum direction changes needed to trigger
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- cursorScale: 350
  $name: Cursor scale (%)
  $description: How large the cursor grows. 200 = 2×, 350 = 3.5×, 500 = 5×.
- enlargeDuration: 500
  $name: Enlarge duration (ms)
  $description: How long the cursor stays enlarged after you stop shaking.
- shakeSensitivity: 4
  $name: Shake sensitivity (direction changes)
  $description: Minimum direction reversals needed to trigger. Lower = easier to trigger.
- minSpeed: 550
  $name: Minimum movement speed (pixels/sec)
  $description: Average speed threshold for shake detection. Lower = easier to trigger.
*/
// ==/WindhawkModSettings==

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#include <cmath>

// ─── Runtime settings (loaded from Windhawk UI) ─────────────────────────────

struct ModSettings {
    double scaleFactor;
    int    enlargeDurationMs;
    int    minDirectionChanges;
    double minMovementSpeed;
};

static ModSettings g_cfg;

static void LoadSettings() {
    int scale = Wh_GetIntSetting(L"cursorScale");
    if (scale <= 100) scale = 350;
    g_cfg.scaleFactor = scale / 100.0;

    g_cfg.enlargeDurationMs = Wh_GetIntSetting(L"enlargeDuration");
    if (g_cfg.enlargeDurationMs <= 0) g_cfg.enlargeDurationMs = 500;

    g_cfg.minDirectionChanges = Wh_GetIntSetting(L"shakeSensitivity");
    if (g_cfg.minDirectionChanges <= 0) g_cfg.minDirectionChanges = 4;

    int speed = Wh_GetIntSetting(L"minSpeed");
    if (speed <= 0) speed = 550;
    g_cfg.minMovementSpeed = (double)speed;
}

// ─── Constants (non-configurable) ───────────────────────────────────────────

static constexpr size_t kHistorySize   = 10;
static constexpr int    kMaxTimeWindow = 400;

struct MouseSample {
    POINT pt;
    DWORD time;
};

// ─── Global state ───────────────────────────────────────────────────────────

std::vector<MouseSample> g_history;
bool g_isEnlarged = false;
HCURSOR g_hNormalCursor = NULL;
HCURSOR g_hEnlargedCursor = NULL;
UINT_PTR g_nResetTimerId = 0;
UINT_PTR g_nPollingTimerId = 0;

// ─── Cursor scaling ─────────────────────────────────────────────────────────

HCURSOR CreateScaledCursor(HCURSOR hSrcCursor, float scale) {
    if (!hSrcCursor) return NULL;
    ICONINFO iconInfo;
    if (!GetIconInfo(hSrcCursor, &iconInfo)) return NULL;

    BITMAP bmColor;
    GetObject(iconInfo.hbmColor, sizeof(BITMAP), &bmColor);

    int newWidth = (int)(bmColor.bmWidth * scale);
    int newHeight = (int)(bmColor.bmHeight * scale);

    HDC hdc = GetDC(NULL);
    HDC hdcSrc = CreateCompatibleDC(hdc);
    HDC hdcDst = CreateCompatibleDC(hdc);

    HBITMAP hbmColorNew = CreateCompatibleBitmap(hdc, newWidth, newHeight);
    SelectObject(hdcSrc, iconInfo.hbmColor);
    SelectObject(hdcDst, hbmColorNew);
    StretchBlt(hdcDst, 0, 0, newWidth, newHeight, hdcSrc, 0, 0, bmColor.bmWidth, bmColor.bmHeight, SRCCOPY);

    HBITMAP hbmMaskNew = CreateCompatibleBitmap(hdc, newWidth, newHeight);
    SelectObject(hdcSrc, iconInfo.hbmMask);
    SelectObject(hdcDst, hbmMaskNew);
    StretchBlt(hdcDst, 0, 0, newWidth, newHeight, hdcSrc, 0, 0, bmColor.bmWidth, bmColor.bmHeight, SRCCOPY);

    DeleteDC(hdcSrc);
    DeleteDC(hdcDst);
    ReleaseDC(NULL, hdc);

    ICONINFO newIconInfo = iconInfo;
    newIconInfo.fIcon = FALSE;
    newIconInfo.xHotspot = (DWORD)(iconInfo.xHotspot * scale);
    newIconInfo.yHotspot = (DWORD)(iconInfo.yHotspot * scale);
    newIconInfo.hbmMask = hbmMaskNew;
    newIconInfo.hbmColor = hbmColorNew;

    HCURSOR hNewCursor = CreateIconIndirect(&newIconInfo);

    DeleteObject(hbmMaskNew);
    DeleteObject(hbmColorNew);
    DeleteObject(iconInfo.hbmMask);
    DeleteObject(iconInfo.hbmColor);

    return hNewCursor;
}

// ─── Cursor restore ─────────────────────────────────────────────────────────

void RestoreCursor() {
    if (g_isEnlarged) {
        SystemParametersInfo(SPI_SETCURSORS, 0, NULL, SPIF_SENDCHANGE);
        g_isEnlarged = false;
    }
}

VOID CALLBACK ResetTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    KillTimer(NULL, g_nResetTimerId);
    g_nResetTimerId = 0;
    RestoreCursor();
}

// ─── Shake detection + trigger ──────────────────────────────────────────────

void ProcessMouseTelemetry(POINT pt) {
    DWORD time = GetTickCount();
    MouseSample sample = { pt, time };
    g_history.push_back(sample);

    if (g_history.size() > kHistorySize) {
        g_history.erase(g_history.begin());
    }

    if (g_history.size() < 4) return;

    int dirChangesX = 0, dirChangesY = 0;
    int lastDirX = 0, lastDirY = 0;
    float totalDistance = 0.0f;

    for (size_t i = 1; i < g_history.size(); ++i) {
        int dx = g_history[i].pt.x - g_history[i - 1].pt.x;
        int dy = g_history[i].pt.y - g_history[i - 1].pt.y;
        totalDistance += sqrt(float(dx * dx + dy * dy));

        int dirX = (dx > 0) ? 1 : ((dx < 0) ? -1 : 0);
        int dirY = (dy > 0) ? 1 : ((dy < 0) ? -1 : 0);

        if (dirX != 0) {
            if (lastDirX != 0 && dirX != lastDirX) dirChangesX++;
            lastDirX = dirX;
        }
        if (dirY != 0) {
            if (lastDirY != 0 && dirY != lastDirY) dirChangesY++;
            lastDirY = dirY;
        }
    }

    DWORD timeDelta = g_history.back().time - g_history.front().time;
    if (timeDelta == 0 || timeDelta > (DWORD)kMaxTimeWindow) return;

    float speed = (totalDistance / (float)timeDelta) * 1000.0f;

    if (speed > g_cfg.minMovementSpeed &&
       (dirChangesX >= g_cfg.minDirectionChanges || dirChangesY >= g_cfg.minDirectionChanges)) {

        if (!g_isEnlarged) {
            g_hNormalCursor = LoadCursor(NULL, IDC_ARROW);
            g_hEnlargedCursor = CreateScaledCursor(g_hNormalCursor, (float)g_cfg.scaleFactor);

            if (g_hEnlargedCursor) {
                SetSystemCursor(CopyCursor(g_hEnlargedCursor), 32512); // OCR_NORMAL
                g_isEnlarged = true;
            }
        }

        if (g_nResetTimerId) KillTimer(NULL, g_nResetTimerId);
        g_nResetTimerId = SetTimer(NULL, 0, g_cfg.enlargeDurationMs, ResetTimerProc);
    }
}

// ─── Polling timer ──────────────────────────────────────────────────────────

VOID CALLBACK PollingTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    CURSORINFO ci = { sizeof(CURSORINFO) };
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
        ProcessMouseTelemetry(ci.ptScreenPos);
    }
}

// ─── Windhawk entry points ──────────────────────────────────────────────────

BOOL Wh_ModInit(void) {
    LoadSettings();
    g_nPollingTimerId = SetTimer(NULL, 0, 16, PollingTimerProc);
    return (g_nPollingTimerId != 0);
}

void Wh_ModUninit(void) {
    if (g_nPollingTimerId) {
        KillTimer(NULL, g_nPollingTimerId);
    }
    if (g_nResetTimerId) {
        KillTimer(NULL, g_nResetTimerId);
    }
    RestoreCursor();
}

void Wh_ModSettingsChanged(void) {
    LoadSettings();
}
