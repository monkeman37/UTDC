/* UTDC Wizard - minimal Win32 front end for the ROM builder.
 * Pick the UNDERTALE folder, pick where to save, press Build.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "build.h"

#define IDC_GAMEDIR   101
#define IDC_BROWSE1   102
#define IDC_OUTFILE   103
#define IDC_BROWSE2   104
#define IDC_BUILD     105
#define IDC_PROGRESS  106
#define IDC_LOG       107
#define IDC_STATUS    108

#define WM_APP_LOG      (WM_APP + 1)
#define WM_APP_PROGRESS (WM_APP + 2)
#define WM_APP_DONE     (WM_APP + 3)

static HWND g_wnd, g_gameDir, g_outFile, g_build, g_progress, g_log, g_status;
static HFONT g_font;
static volatile LONG g_running = 0;
static BuildContext g_ctx;

typedef struct { double frac; wchar_t text[256]; } ProgressMsg;

static void appendLog(const wchar_t* line) {
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, (WPARAM) len, (LPARAM) len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM) line);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM) L"\r\n");
}

static wchar_t* utf8ToWide(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t* w = (wchar_t*) malloc(sizeof(wchar_t) * (size_t) (n > 0 ? n : 1));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); else w[0] = 0;
    return w;
}

/* Paths are handed to the C runtime as ANSI; use 8.3 short names when a path
   contains characters outside the ANSI code page. */
static char* pathForCrt(const wchar_t* w, int mustExist) {
    wchar_t shortBuf[MAX_PATH * 2];
    const wchar_t* use = w;
    if (mustExist && GetShortPathNameW(w, shortBuf, MAX_PATH * 2)) use = shortBuf;
    int n = WideCharToMultiByte(CP_ACP, 0, use, -1, NULL, 0, NULL, NULL);
    char* s = (char*) malloc((size_t) (n > 0 ? n : 1));
    if (n > 0) WideCharToMultiByte(CP_ACP, 0, use, -1, s, n, NULL, NULL); else s[0] = 0;
    return s;
}

static void onLog(void* user, const char* line) {
    (void) user;
    PostMessageW(g_wnd, WM_APP_LOG, 0, (LPARAM) utf8ToWide(line));
}

static void onProgress(void* user, double frac, const char* status) {
    (void) user;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 50 && frac < 1.0) return;
    last = now;
    ProgressMsg* m = (ProgressMsg*) malloc(sizeof(ProgressMsg));
    m->frac = frac;
    MultiByteToWideChar(CP_UTF8, 0, status, -1, m->text, 255);
    m->text[255] = 0;
    PostMessageW(g_wnd, WM_APP_PROGRESS, 0, (LPARAM) m);
}

typedef struct { char* gameDir; char* outPath; } Job;

static DWORD WINAPI worker(LPVOID p) {
    Job* job = (Job*) p;
    BuildOptions o;
    memset(&o, 0, sizeof o);
    o.gameDir = job->gameDir;
    o.outPath = job->outPath;
    o.format = DISC_FORMAT_GDI;
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.log = onLog;
    g_ctx.progress = onProgress;
    int rc = utdc_build(&g_ctx, &o);
    free(job->gameDir); free(job->outPath); free(job);
    PostMessageW(g_wnd, WM_APP_DONE, (WPARAM) rc, 0);
    return 0;
}

static int fileExistsW(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void startBuild(void) {
    wchar_t dir[MAX_PATH * 2], out[MAX_PATH * 2], probe[MAX_PATH * 2 + 16];
    GetWindowTextW(g_gameDir, dir, MAX_PATH * 2);
    GetWindowTextW(g_outFile, out, MAX_PATH * 2);
    size_t dl = wcslen(dir);
    while (dl > 0 && (dir[dl - 1] == L'\\' || dir[dl - 1] == L'/')) dir[--dl] = 0;
    swprintf(probe, MAX_PATH * 2 + 16, L"%ls\\data.win", dir);
    if (!fileExistsW(probe)) {
        MessageBoxW(g_wnd, L"That folder doesn't contain data.win.\n\nChoose the folder where UNDERTALE is installed (it contains UNDERTALE.exe and data.win).\nIn Steam: right-click UNDERTALE > Manage > Browse local files.", L"UTDC Wizard", MB_ICONWARNING);
        return;
    }
    if (!out[0]) { MessageBoxW(g_wnd, L"Choose where to save the Dreamcast image.", L"UTDC Wizard", MB_ICONWARNING); return; }
    size_t ol = wcslen(out);
    if (ol < 4 || _wcsicmp(out + ol - 4, L".gdi") != 0) { wcscat(out, L".gdi"); SetWindowTextW(g_outFile, out); }

    /* the output folder must exist for short-name conversion */
    wchar_t outDir[MAX_PATH * 2];
    wcscpy(outDir, out);
    wchar_t* slash = wcsrchr(outDir, L'\\');
    if (slash) *slash = 0;
    CreateDirectoryW(outDir, NULL);
    char* outDirA = pathForCrt(outDir, 1);
    char* outA = (char*) malloc(strlen(outDirA) + MAX_PATH * 2);
    char* nameA = pathForCrt(slash ? slash + 1 : out, 0);
    sprintf(outA, "%s\\%s", outDirA, nameA);
    free(outDirA); free(nameA);

    Job* job = (Job*) malloc(sizeof(Job));
    job->gameDir = pathForCrt(dir, 1);
    job->outPath = outA;
    SetWindowTextW(g_log, L"");
    SendMessageW(g_progress, PBM_SETPOS, 0, 0);
    InterlockedExchange(&g_running, 1);
    SetWindowTextW(g_build, L"Cancel");
    EnableWindow(g_gameDir, FALSE); EnableWindow(g_outFile, FALSE);
    HANDLE t = CreateThread(NULL, 0, worker, job, 0, NULL);
    if (t) CloseHandle(t);
}

static void browseFolder(void) {
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = g_wnd;
    bi.lpszTitle = L"Select the UNDERTALE install folder (contains data.win)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(g_gameDir, path);
        CoTaskMemFree(pidl);
    }
}

static void browseSave(void) {
    wchar_t file[MAX_PATH * 2];
    GetWindowTextW(g_outFile, file, MAX_PATH * 2);
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"Dreamcast GD-ROM image (*.gdi)\0*.gdi\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrDefExt = L"gdi";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Save the Dreamcast image (a few extra track files go next to it)";
    if (GetSaveFileNameW(&ofn)) SetWindowTextW(g_outFile, file);
}

static HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(cls == WC_EDITW ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, (HMENU) (INT_PTR) id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM) g_font, TRUE);
    return c;
}

static void guessDefaults(void) {
    static const wchar_t* candidates[] = {
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Undertale",
        L"C:\\Program Files\\Steam\\steamapps\\common\\Undertale",
        L"C:\\GOG Games\\Undertale",
        L"C:\\Program Files (x86)\\GOG Galaxy\\Games\\Undertale",
    };
    wchar_t exeDir[MAX_PATH], probe[MAX_PATH + 16];
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    wchar_t* s = wcsrchr(exeDir, L'\\');
    if (s) *s = 0;
    swprintf(probe, MAX_PATH + 16, L"%ls\\data.win", exeDir);
    if (fileExistsW(probe)) SetWindowTextW(g_gameDir, exeDir);
    else {
        swprintf(probe, MAX_PATH + 16, L"%ls\\Undertale\\data.win", exeDir);
        if (fileExistsW(probe)) { wcscat(exeDir, L"\\Undertale"); SetWindowTextW(g_gameDir, exeDir); }
        else for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
            swprintf(probe, MAX_PATH + 16, L"%ls\\data.win", candidates[i]);
            if (fileExistsW(probe)) { SetWindowTextW(g_gameDir, candidates[i]); break; }
        }
    }
    wchar_t out[MAX_PATH * 2];
    GetModuleFileNameW(NULL, out, MAX_PATH);
    s = wcsrchr(out, L'\\');
    if (s) *s = 0;
    wcscat(out, L"\\UNDERTALE-DC\\UNDERTALE.gdi");
    SetWindowTextW(g_outFile, out);
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_wnd = h;
        NONCLIENTMETRICSW ncm; ncm.cbSize = sizeof ncm;
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        mk(L"STATIC", L"UNDERTALE \x2192 Sega Dreamcast image builder. Uses your own copy of the game; nothing is downloaded.", 0, 14, 12, 560, 20, 0);
        mk(L"STATIC", L"1. Your UNDERTALE folder (contains data.win):", 0, 14, 44, 400, 18, 0);
        g_gameDir = mk(WC_EDITW, L"", ES_AUTOHSCROLL, 14, 64, 470, 24, IDC_GAMEDIR);
        mk(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 492, 63, 84, 26, IDC_BROWSE1);
        mk(L"STATIC", L"2. Save the Dreamcast image as:", 0, 14, 98, 400, 18, 0);
        g_outFile = mk(WC_EDITW, L"", ES_AUTOHSCROLL, 14, 118, 470, 24, IDC_OUTFILE);
        mk(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 492, 117, 84, 26, IDC_BROWSE2);
        g_build = mk(L"BUTTON", L"3. Build", BS_DEFPUSHBUTTON, 14, 156, 120, 32, IDC_BUILD);
        g_status = mk(L"STATIC", L"Ready.", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 146, 164, 430, 18, IDC_STATUS);
        g_progress = mk(PROGRESS_CLASSW, L"", 0, 14, 198, 562, 18, IDC_PROGRESS);
        SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
        g_log = mk(WC_EDITW, L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 14, 226, 562, 170, IDC_LOG);
        guessDefaults();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE1: browseFolder(); break;
        case IDC_BROWSE2: browseSave(); break;
        case IDC_BUILD:
            if (g_running) { g_ctx.cancel = 1; SetWindowTextW(g_status, L"Cancelling..."); }
            else startBuild();
            break;
        }
        return 0;
    case WM_APP_LOG: {
        wchar_t* w = (wchar_t*) lp;
        appendLog(w);
        free(w);
        return 0;
    }
    case WM_APP_PROGRESS: {
        ProgressMsg* m = (ProgressMsg*) lp;
        SendMessageW(g_progress, PBM_SETPOS, (WPARAM) (m->frac * 1000), 0);
        SetWindowTextW(g_status, m->text);
        free(m);
        return 0;
    }
    case WM_APP_DONE:
        InterlockedExchange(&g_running, 0);
        SetWindowTextW(g_build, L"3. Build");
        EnableWindow(g_gameDir, TRUE); EnableWindow(g_outFile, TRUE);
        if (wp == 0) {
            SendMessageW(g_progress, PBM_SETPOS, 1000, 0);
            SetWindowTextW(g_status, L"Done! Open the .gdi in your emulator, or copy the folder to a GDEMU SD card.");
            MessageBoxW(h, L"The Dreamcast image is ready.\n\nOpen UNDERTALE.gdi in Redream or Flycast, or copy the whole output folder to your GDEMU/MODE SD card.\nKeep the .gdi together with the track files.", L"UTDC Wizard", MB_ICONINFORMATION);
        } else {
            SetWindowTextW(g_status, g_ctx.cancel ? L"Cancelled." : L"Build failed - see the log above.");
        }
        return 0;
    case WM_CLOSE:
        if (g_running) { g_ctx.cancel = 1; return 0; }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void) prev; (void) cmd;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
    wc.lpszClassName = L"UtdcWizard";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    RECT r = { 0, 0, 590, 410 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    HWND w = CreateWindowExW(0, L"UtdcWizard", L"UTDC Wizard - UNDERTALE for Dreamcast", WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX),
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    ShowWindow(w, show);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(w, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    CoUninitialize();
    return 0;
}
