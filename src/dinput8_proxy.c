/*
 * dinput8.dll proxy + minimal ASI loader for Hitman: Blood Money.
 *
 * The game loads dinput8.dll by name, so a copy next to the exe takes
 * precedence over the system one.  This proxy forwards all six real
 * dinput8 exports to the system DLL and, after the loader lock is
 * released, loads every *.asi file from the game directory.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

typedef HRESULT (WINAPI *PFN_DICREATE)(HINSTANCE, DWORD, const void *, void **, void *);
typedef HRESULT (WINAPI *PFN_CANUNLOAD)(void);
typedef HRESULT (WINAPI *PFN_GETCLASSOBJ)(const void *, const void *, void **);
typedef HRESULT (WINAPI *PFN_SERVER)(void);
typedef void *  (WINAPI *PFN_JOYSTICK)(void);

static HMODULE g_real;
static PFN_DICREATE   p_DirectInput8Create;static PFN_CANUNLOAD  p_DllCanUnloadNow;
static PFN_GETCLASSOBJ p_DllGetClassObject;
static PFN_SERVER     p_DllRegisterServer;
static PFN_SERVER     p_DllUnregisterServer;
static PFN_JOYSTICK   p_GetdfDIJoystick;

static char g_log_path[MAX_PATH];

static void loader_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    int n;
    HANDLE h;
    DWORD wrote;

    if (!g_log_path[0])
        return;

    va_start(ap, fmt);
    n = _vsnprintf(buf, sizeof(buf) - 3, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';

    h = CreateFileA(g_log_path, FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, buf, (DWORD)n, &wrote, NULL);
        CloseHandle(h);
    }
}

static void w2a(const wchar_t *w, char *out, int cap)
{
    int i = 0;
    if (!w || !out || cap <= 0)
        return;
    while (w[i] && i < cap - 1) {
        out[i] = (w[i] < 0x80) ? (char)w[i] : '?';
        i++;
    }
    out[i] = 0;
}

static DWORD WINAPI asi_loader(LPVOID param)
{
    wchar_t self[MAX_PATH];
    wchar_t dir[MAX_PATH];
    wchar_t pattern[MAX_PATH];
    wchar_t full[MAX_PATH];
    wchar_t *p;
    WIN32_FIND_DATAW fd;
    HANDLE find;
    int count = 0;

    Sleep(200);                            /* let the loader lock go */

    if (!GetModuleFileNameW((HMODULE)param, self, MAX_PATH))
        return 0;
    lstrcpyW(dir, self);
    p = dir;
    while (*p) p++;
    while (p > dir && *p != L'\\') p--;
    if (*p == L'\\') *p = 0;

    lstrcpyW(pattern, dir);
    lstrcatW(pattern, L"\\*.asi");

    find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        loader_log("no .asi files found in game directory");
        return 0;
    }
    do {
        char name[260];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        w2a(fd.cFileName, name, sizeof(name));
        lstrcpyW(full, dir);
        lstrcatW(full, L"\\");
        lstrcatW(full, fd.cFileName);
        {
            HMODULE m = LoadLibraryW(full);
            if (m) {
                count++;
                loader_log("loaded ASI: %s (%p)", name, m);
            } else {
                loader_log("FAILED to load ASI: %s (error %lu)",
                           name, GetLastError());
            }
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    loader_log("ASI loader done, %d module(s) loaded", count);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    wchar_t sys[MAX_PATH];

    (void)reserved;

    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    DisableThreadLibraryCalls(inst);

    /* log file lives next to the exe */
    g_log_path[0] = 0;
    {
        char exe[MAX_PATH];
        if (GetModuleFileNameA(NULL, exe, MAX_PATH)) {
            char *q = exe;
            while (*q) q++;
            while (q > exe && *q != '\\') q--;
            if (*q == '\\') *q = 0;
            _snprintf(g_log_path, sizeof(g_log_path),
                      "%s\\hbm_poolfix_loader.log", exe);
        }
    }

    /* Load the real 32-bit dinput8 by full path (System32 is redirected
     * to SysWOW64 for this process). */
    if (GetSystemDirectoryW(sys, MAX_PATH)) {
        lstrcatW(sys, L"\\dinput8.dll");
        g_real = LoadLibraryW(sys);
    }
    if (g_real) {
        p_DirectInput8Create    = (PFN_DICREATE)GetProcAddress(g_real, "DirectInput8Create");
        p_DllCanUnloadNow       = (PFN_CANUNLOAD)GetProcAddress(g_real, "DllCanUnloadNow");
        p_DllGetClassObject     = (PFN_GETCLASSOBJ)GetProcAddress(g_real, "DllGetClassObject");
        p_DllRegisterServer     = (PFN_SERVER)GetProcAddress(g_real, "DllRegisterServer");
        p_DllUnregisterServer   = (PFN_SERVER)GetProcAddress(g_real, "DllUnregisterServer");
        p_GetdfDIJoystick       = (PFN_JOYSTICK)GetProcAddress(g_real, "GetdfDIJoystick");
        loader_log("dinput8 proxy: real dinput8 at %p", g_real);
    } else {
        loader_log("dinput8 proxy: could not load system dinput8 (error %lu)",
                   GetLastError());
    }

    {
        HANDLE h = CreateThread(NULL, 0, asi_loader, inst, 0, NULL);
        if (h) CloseHandle(h);
    }

    return TRUE;
}

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver,
                                  const void *iid, void **out,
                                  void *outer)
{
    HRESULT hr;
    if (p_DirectInput8Create) {
        hr = p_DirectInput8Create(hinst, ver, iid, out, outer);
        loader_log("DirectInput8Create(ver=0x%X) -> 0x%08lX out=%p",
                   ver, (unsigned long)hr, out ? *out : NULL);
        return hr;
    }
    return (HRESULT)0x80004005L;          /* E_FAIL */
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    if (p_DllCanUnloadNow)
        return p_DllCanUnloadNow();
    return (HRESULT)0x00000001L;          /* S_FALSE */
}

HRESULT WINAPI DllGetClassObject(const void *clsid,
                                 const void *iid,
                                 void **out)
{
    if (p_DllGetClassObject)
        return p_DllGetClassObject(clsid, iid, out);
    return (HRESULT)0x80004005L;
}

HRESULT WINAPI DllRegisterServer(void)
{
    if (p_DllRegisterServer)
        return p_DllRegisterServer();
    return (HRESULT)0x80004005L;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    if (p_DllUnregisterServer)
        return p_DllUnregisterServer();
    return (HRESULT)0x80004005L;
}

void * WINAPI GetdfDIJoystick(void)
{
    if (p_GetdfDIJoystick)
        return p_GetdfDIJoystick();
    return NULL;
}
