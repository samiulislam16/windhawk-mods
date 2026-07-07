// ==WindhawkMod==
// @id              macos-shake-to-find-cursor
// @name            11macOS Shake to Find Cursor
// @description     Smoothly enlarges the mouse cursor using pre-cached stepping arrays system-wide.
// @version         1.3.0
// @author          samiulislam16
// @github          https://github.com
// @include         explorer.exe
// @compilerOptions -luser32 -lgdi32 -lshcore
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Shake to Find Cursor (Smooth Animation)

Brings the beloved macOS "Shake mouse pointer to locate" feature to Windows!

This version utilizes a safe, pre-cached rendering pipeline to smoothly 
animate the transition from normal size to your custom enlarged scale.
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

struct ModSettings {
    double scaleFactor;
    int    enlargeDurationMs;
    int    minDirectionChanges;
    double minMovementSpeed;
};

static ModSettings g_cfg;
static constexpr int kAnimationSteps = 4; // 4 pre-calculated stepping nodes
static HCURSOR g_cachedCursors[kAnimationSteps] = { NULL };
static int g_currentAnimIndex = 0;

static constexpr size_t kHistorySize   = 10;
static constexpr int    kMaxTimeWindow = 400;

struct MouseSample {
    POINT pt;
    DWORD time;
};

std::vector<MouseSample> g_history;
bool g_isEnlarged = false;
DWORD g_lastShakeTime = 0;
HANDLE g_hThread = NULL;
bool g_runThread = false;

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
    HGDIOBJ hOldSrc = SelectObject(hdcSrc, iconInfo.hbmColor);
    HGDIOBJ hOldDst = SelectObject(hdcDst, hbmColorNew);
    StretchBlt(hdcDst, 0, 0, newWidth, newHeight, hdcSrc, 0, 0, bmColor.bmWidth, bmColor.bmHeight, SRCCOPY);

    HBITMAP hbmMaskNew = CreateCompatibleBitmap(hdc, newWidth, newHeight);
    SelectObject(hdcSrc, iconInfo.hbmMask);
    SelectObject(hdcDst, hbmMaskNew);
    StretchBlt(hdcDst, 0, 0, newWidth, newHeight, hdcSrc, 0, 0, bmColor.bmWidth, bmColor.bmHeight, SRCCOPY);

    SelectObject(hdcSrc, hOldSrc);
    SelectObject(hdcDst, hOldDst);
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

void FreeCachedCursors() {
    for (int i = 0; i < kAnimationSteps; ++i) {
        if (g_cachedCursors[i]) {
            DestroyCursor(g_cachedCursors[i]);
            g_cachedCursors[i] = NULL;
        }
    }
}

// Generates the sizes exactly ONCE during load/settings modifications
void PreCacheAllCursors() {
    FreeCachedCursors();
    HCURSOR hBase = LoadCursor(NULL, IDC_ARROW);
    if (!hBase) return;

    double maxScale = g_cfg.scaleFactor;
    for (int i = 0; i < kAnimationSteps; ++i) {
        // Linearly distribute scales between 1.25x and maxScale
        double t = (double)i / (kAnimationSteps - 1);
        double stepScale = 1.25 + t * (maxScale - 1.25);
        g_cachedCursors[i] = CreateScaledCursor(hBase, (float)stepScale);
    }
}

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

    PreCacheAllCursors();
}

void RestoreCursor() {
    if (g_isEnlarged) {
        SystemParametersInfo(SPI_SETCURSORS, 0, NULL, SPIF_SENDCHANGE);
        g_isEnlarged = false;
        g_currentAnimIndex = 0;
    }
}

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
        totalDistance += std::sqrt((float)(dx * dx + dy * dy));

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
        g_lastShakeTime = time;

        // Animate UP frame-by-frame smoothly
        if (g_currentAnimIndex < (kAnimationSteps - 1)) {
            g_currentAnimIndex++;
            if (g_cachedCursors[g_currentAnimIndex]) {
                SetSystemCursor(CopyCursor(g_cachedCursors[g_currentAnimIndex]), 32512);
                g_isEnlarged = true;
            }
        }
    } else {
        if (g_isEnlarged && (time - g_lastShakeTime > (DWORD)g_cfg.enlargeDurationMs)) {
            // Animate DOWN step-by-step smoothly
            if (g_currentAnimIndex > 0) {
                g_currentAnimIndex--;
                if (g_cachedCursors[g_currentAnimIndex]) {
                    SetSystemCursor(CopyCursor(g_cachedCursors[g_currentAnimIndex]), 32512);
                }
            } else {
                RestoreCursor();
            }
        }
    }
}

DWORD WINAPI HardwarePollingThread(LPVOID lpParam) {
    while (g_runThread) {
        POINT pt;
        if (GetPhysicalCursorPos(&pt)) {
            ProcessMouseTelemetry(pt);
        }
        // Slightly lengthened sleep constraint to let the eye track the frame increments
        Sleep(32); 
    }
    return 0;
}

BOOL Wh_ModInit(void) {
    LoadSettings();
    
    wchar_t processPath[MAX_PATH];
    GetModuleFileNameW(NULL, processPath, MAX_PATH);
    if (wcsstr(processPath, L"explorer.exe") == nullptr) {
        return TRUE; 
    }

    g_runThread = true;
    g_hThread = CreateThread(NULL, 0, HardwarePollingThread, NULL, 0, NULL);
    return (g_hThread != NULL);
}

void Wh_ModUninit(void) {
    g_runThread = false;
    if (g_hThread) {
        WaitForSingleObject(g_hThread, 500);
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }
    RestoreCursor();
    FreeCachedCursors();
}

void Wh_ModSettingsChanged(void) {
    LoadSettings();
}
