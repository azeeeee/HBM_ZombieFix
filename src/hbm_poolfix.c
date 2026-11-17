/*
 * hbm_poolfix.asi
 * ------------------------------------------------------------------
 * Hitman: Blood Money - ZPoolAlloc capacity fix.
 *
 * The game's ZPoolAlloc allocator (0x44C430) is a first-fit allocator
 * over a fixed arena that lives in the game image's .data/.bss.  The
 * arena used by ZPoolAllocLinkSortRefTab ("Animation::ActiveAnimation::
 * s_AnimationCallbackAllocator", 0x9AB3AC, 34,816 bytes) can be
 * exhausted by the M08 zombie easter egg.  When it is, 0x44C430
 * returns NULL and 0x42E3C4 executes an int 3 assert -> process dies.
 *
 * This module hooks the allocator entry and its free path:
 *   - 0x44C430  alloc(pool, size)      -> pool_alloc_dispatch
 *   - 0x44C5C8  free(pool, ptr)        -> pool_free_dispatch
 *
 * It keeps the original arena untouched.  When the original pool
 * cannot satisfy a request, the block is carved out of extra arenas
 * that this module creates with VirtualAlloc, using the game's own
 * allocator code on a second pool object.  Frees are routed back to
 * whichever arena owns the pointer.
 *
 * The exe is Steam-DRM packed: on-disk .text is encrypted.  Patching
 * is therefore deferred until the runtime code matches the known
 * unpacked byte patterns.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* --- game RVAs (runtime image base is normally 0x00400000) --------- */
#define RVA_ALLOC        0x4C430u   /* ZPoolAlloc::alloc(size)          */
#define RVA_ALLOC_RESUME 0x4C438u   /* after the 8 bytes we steal       */
#define RVA_FREE_ENTRY   0x4C5C0u   /* ZPoolAlloc::free(ptr)            */
#define RVA_FREE_CHECK   0x4C5C8u   /* cmp eax,[ecx+0xc] / jbe assert   */
#define RVA_FREE_CONT    0x4C5CDu   /* continue normal free path        */
#define RVA_FREE_ASSERT  0x4C5D2u   /* int 3 site inside free           */
#define RVA_INSERT       0x2E3A0u   /* ZPoolAllocLinkSortRefTab::insert */
#define RVA_TABLE_VTABLE 0x35D9ACu  /* vtable of ZPoolAllocLinkSortRefTab  */
#define RVA_POOL_COUNT   0x4ABB08u  /* byte: number of registered pools */
#define RVA_POOL_ARRAY   0x4ABB10u  /* PoolObj*[32]                     */
#define RVA_CRASH_POOL   0x5AB3ACu  /* the 34,816 byte pool             */
#define RVA_CRASH_ARENA  0x5A27D8u  /* its arena                        */

#define EXT_POOL_MAX     16
#define EXT_ARENA_MIN    (8u * 1024u * 1024u)
#define EXT_ARENA_SLACK  0x10000u

typedef struct PoolObj {
    unsigned used;        /* +0x00 bytes allocated in this arena      */
    unsigned highwater;   /* +0x04                                    */
    unsigned name;        /* +0x08 const char* description (debug)    */
    unsigned arena;       /* +0x0C arena base                         */
    unsigned arena_size;  /* +0x10 arena size                         */
    unsigned head;        /* +0x14 lowest 8-byte free descriptor      */
    unsigned pad[10];
} PoolObj;

/* --- state -------------------------------------------------------- */
static unsigned char *g_base;
static void         *g_alloc_resume;
static void         *g_free_entry;
static void         *g_free_cont;
static void         *g_free_assert;

static PoolObj      *g_ext_pool[EXT_POOL_MAX];
static unsigned char *g_ext_arena[EXT_POOL_MAX];
static unsigned      g_ext_arena_size[EXT_POOL_MAX];
static volatile LONG g_ext_count;

static CRITICAL_SECTION g_ext_lock;
static CRITICAL_SECTION g_log_lock;
static DWORD            g_tls_bypass = TLS_OUT_OF_INDEXES;
static volatile LONG    g_fallback_count;

static char g_exe_dir[MAX_PATH];
static char g_log_path[MAX_PATH];
static void *g_insert_fn;

/* implemented below; referenced from the assembly hooks */
void * __fastcall pool_alloc_dispatch(void *pool, unsigned size);
int __cdecl pool_free_dispatch(void *ptr, void *pool);

/* ------------------------------------------------------------------ */
/* logging                                                            */
/* ------------------------------------------------------------------ */
static void log_msg(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    int n;
    HANDLE h;
    DWORD wrote;
    SYSTEMTIME st;

    if (!g_log_path[0])
        return;

    GetLocalTime(&st);
    n = _snprintf(buf, sizeof(buf) - 96,
                  "[%02u:%02u:%02u.%03u] ",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_start(ap, fmt);
    if (n < 0) n = 0;
    n += _vsnprintf(buf + n, sizeof(buf) - n - 3, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)sizeof(buf) - 3;
    if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';

    EnterCriticalSection(&g_log_lock);
    h = CreateFileA(g_log_path, FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, buf, (DWORD)n, &wrote, NULL);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_log_lock);
}

/* ------------------------------------------------------------------ */
/* assembly helpers                                                   */
/* ------------------------------------------------------------------ */

/* Original 0x44C430 prologue (8 bytes) followed by a jump back into
 * the game at 0x44C438.  Called with the exact original ABI:
 *   ecx = pool, [esp+4] = size, returns with "ret 4". */
__declspec(naked) void alloc_trampoline(void)
{
    __asm {
        mov  eax, dword ptr [ecx + 0x10]
        push esi
        mov  esi, dword ptr [esp + 8]
        jmp  dword ptr [g_alloc_resume]
    }
}

/* Call the game allocator at 0x44C430 (through the trampoline) with
 * cdecl arguments. */
__declspec(naked) void *orig_alloc_call(void *pool, unsigned size)
{
    __asm {
        push ebp
        mov  ebp, esp
        mov  ecx, dword ptr [ebp + 8]
        push dword ptr [ebp + 12]
        call alloc_trampoline
        pop  ebp
        ret
    }
}

/* Call the game free function at 0x44C5C0 with cdecl arguments. */
__declspec(naked) void orig_free_call(void *pool, void *ptr)
{
    __asm {
        push ebp
        mov  ebp, esp
        mov  ecx, dword ptr [ebp + 8]
        push dword ptr [ebp + 12]
        call dword ptr [g_free_entry]
        pop  ebp
        ret
    }
}

/* Replacement for 0x44C430.  Must preserve ebx/esi/edi/ebp and do
 * "ret 4" (__thiscall with one stack argument). */
__declspec(naked) void alloc_hook_entry(void)
{
    __asm {
        push ebx
        push ebp
        push esi
        push edi
        mov  edx, dword ptr [esp + 20]   /* original size argument */
        call pool_alloc_dispatch
        pop  edi
        pop  esi
        pop  ebp
        pop  ebx
        ret  4
    }
}

/* Replacement for 0x44C5C8 (cmp eax,[ecx+0xc] / jbe 0x44C5D2).
 * eax = ptr, ecx = pool, [esp+4] = ptr. */
__declspec(naked) void free_hook_entry(void)
{
    __asm {
        push ecx
        push eax
        push ecx
        push eax
        call pool_free_dispatch
        add  esp, 8
        pop  edx                         /* saved ptr */
        pop  ecx                         /* saved pool */
        test eax, eax
        je   normal_path
        cmp  eax, 1
        je   handled
        jmp  dword ptr [g_free_assert]   /* pointer <= arena base */
    normal_path:
        mov  eax, edx
        jmp  dword ptr [g_free_cont]
    handled:
        ret  4
    }
}

/* self-test helpers */
__declspec(naked) void *test_alloc(void *pool, unsigned size)
{
    __asm {
        push ebp
        mov  ebp, esp
        mov  ecx, dword ptr [ebp + 8]
        push dword ptr [ebp + 12]
        call alloc_hook_entry
        pop  ebp
        ret
    }
}

__declspec(naked) void test_free(void *pool, void *ptr)
{
    __asm {
        push ebp
        mov  ebp, esp
        mov  ecx, dword ptr [ebp + 8]
        push dword ptr [ebp + 12]
        call dword ptr [g_free_entry]
        pop  ebp
        ret
    }
}

/* unsigned *table->insert(value)  (ZPoolAllocLinkSortRefTab::insert) */
__declspec(naked) unsigned *test_insert(void *table, unsigned value)
{
    __asm {
        push ebp
        mov  ebp, esp
        mov  ecx, dword ptr [ebp + 8]
        push dword ptr [ebp + 12]
        call dword ptr [g_insert_fn]
        pop  ebp
        ret
    }
}

/* free one node block through the table's vtable slot +0x84 */
__declspec(naked) void test_table_free_block(void *table, void *block)
{
    __asm {
        push ebp
        mov  ebp, esp
        mov  ecx, dword ptr [ebp + 8]
        mov  eax, dword ptr [ecx]            /* vtable */
        push dword ptr [ebp + 12]
        call dword ptr [eax + 0x84]
        pop  ebp
        ret
    }
}

/* keep the naked helpers from being dropped by the linker */
static void *const g_keep_alive[] = {
    (void *)alloc_trampoline,
    (void *)orig_alloc_call,
    (void *)orig_free_call,
    (void *)alloc_hook_entry,
    (void *)free_hook_entry,
    (void *)test_alloc,
    (void *)test_free,
    (void *)test_insert,
    (void *)test_table_free_block
};

/* ------------------------------------------------------------------ */
/* external arena management                                          */
/* ------------------------------------------------------------------ */
static int ext_pool_create(unsigned min_request)
{
    unsigned size = EXT_ARENA_MIN;
    unsigned char *arena;
    PoolObj *pool;
    unsigned head;

    if (min_request > EXT_ARENA_MIN - EXT_ARENA_SLACK)
        size = (min_request + EXT_ARENA_SLACK + 0xFFFFu) & ~0xFFFFu;

    arena = (unsigned char *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
                                          PAGE_READWRITE);
    if (!arena)
        return 0;

    pool = (PoolObj *)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_READWRITE);
    if (!pool) {
        VirtualFree(arena, 0, MEM_RELEASE);
        return 0;
    }
    memset(pool, 0, 0x1000);

    pool->name       = (unsigned)(ULONG_PTR)"HBM_PoolFix external arena";
    pool->arena      = (unsigned)(ULONG_PTR)arena;
    pool->arena_size = size;
    pool->used       = 0;
    pool->highwater  = 0;

    /* One free descriptor covering the whole arena, exactly like the
     * game's own pool constructor leaves it. */
    head = (unsigned)(ULONG_PTR)(arena + size - 8);
    *(unsigned *)(ULONG_PTR)head       = size - 8;
    *(unsigned *)(ULONG_PTR)(head + 4) = (unsigned)(ULONG_PTR)arena;
    pool->head = head;

    {
        LONG i = InterlockedIncrement(&g_ext_count) - 1;
        if (i >= EXT_POOL_MAX) {
            InterlockedDecrement(&g_ext_count);
            VirtualFree(pool, 0, MEM_RELEASE);
            VirtualFree(arena, 0, MEM_RELEASE);
            return 0;
        }
        g_ext_pool[i]       = pool;
        g_ext_arena[i]      = arena;
        g_ext_arena_size[i] = size;
    }

    log_msg("created external arena #%ld: pool=%p arena=%p size=0x%X",
            (long)(g_ext_count - 1), pool, arena, size);
    return 1;
}

/* Caller must hold g_ext_lock. */
static void *ext_alloc(unsigned size)
{
    LONG n = g_ext_count;
    LONG i;
    void *p;

    for (i = 0; i < n; i++) {
        p = orig_alloc_call(g_ext_pool[i], size);
        if (p)
            return p;
    }

    if (n < EXT_POOL_MAX && ext_pool_create(size)) {
        p = orig_alloc_call(g_ext_pool[g_ext_count - 1], size);
        if (p)
            return p;
    }
    return NULL;
}

/* Caller must hold g_ext_lock. */
static PoolObj *ext_owner(void *ptr)
{
    unsigned p = (unsigned)(ULONG_PTR)ptr;
    LONG n = g_ext_count;
    LONG i;
    for (i = 0; i < n; i++) {
        unsigned b = (unsigned)(ULONG_PTR)g_ext_arena[i];
        if (p >= b && p < b + g_ext_arena_size[i])
            return g_ext_pool[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* hooks                                                              */
/* ------------------------------------------------------------------ */
void * __fastcall pool_alloc_dispatch(void *pool, unsigned size)
{
    void *p = orig_alloc_call(pool, size);
    if (p)
        return p;

    EnterCriticalSection(&g_ext_lock);
    p = ext_alloc(size);
    LeaveCriticalSection(&g_ext_lock);

    if (p)
        log_msg("fallback alloc: pool=%p size=0x%X -> %p", pool, size, p);
    else
        log_msg("FATAL: fallback alloc failed: pool=%p size=0x%X", pool, size);
    if (p)
        InterlockedIncrement(&g_fallback_count);
    return p;
}

/* returns 0 = normal in-arena pointer, 1 = handled external, 2 = invalid */
int __cdecl pool_free_dispatch(void *ptr, void *pool)
{
    PoolObj *owner;
    unsigned p = (unsigned)(ULONG_PTR)ptr;
    PoolObj *po = (PoolObj *)pool;

    if (g_tls_bypass != TLS_OUT_OF_INDEXES && TlsGetValue(g_tls_bypass))
        return 0;                       /* nested call from ext free */

    EnterCriticalSection(&g_ext_lock);
    owner = ext_owner(ptr);
    if (owner) {
        TlsSetValue(g_tls_bypass, (void *)1);
        orig_free_call(owner, ptr);
        TlsSetValue(g_tls_bypass, NULL);
        LeaveCriticalSection(&g_ext_lock);
        return 1;
    }
    LeaveCriticalSection(&g_ext_lock);

    if (!po)
        return 2;
    if (p <= po->arena || p >= po->head)
        return 2;
    return 0;
}

/* ------------------------------------------------------------------ */
/* patching                                                           */
/* ------------------------------------------------------------------ */
static int memory_readable(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)))
        return 0;
    if (mbi.State != MEM_COMMIT)
        return 0;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        return 0;
    return ((SIZE_T)((const unsigned char *)mbi.BaseAddress + mbi.RegionSize -
                     (const unsigned char *)p) >= n);
}

static int write_code(void *addr, const void *src, SIZE_T len)
{
    DWORD old, tmp;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) {
        log_msg("VirtualProtect(%p) failed: %lu", addr, GetLastError());
        return 0;
    }
    memcpy(addr, src, len);
    VirtualProtect(addr, len, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
    return 1;
}

static int patch_jmp(void *addr, void *target, SIZE_T len)
{
    unsigned char buf[16];
    SIZE_T i;
    if (len < 5 || len > sizeof(buf))
        return 0;
    buf[0] = 0xE9;
    *(int *)(buf + 1) = (int)((const unsigned char *)target -
                              ((const unsigned char *)addr + 5));
    for (i = 5; i < len; i++)
        buf[i] = 0x90;
    return write_code(addr, buf, len);
}

/* ------------------------------------------------------------------ */
/* thread helpers for an atomic patch + self-test                     */
/* ------------------------------------------------------------------ */
#define MAX_SUSPEND 512

static int suspend_others(HANDLE *out, int max)
{
    int n = 0;
    HANDLE snap;
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == tid)
                continue;
            if (n >= max)
                break;
            {
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE,
                                      te.th32ThreadID);
                if (h) {
                    if (SuspendThread(h) != (DWORD)-1)
                        out[n++] = h;
                    else
                        CloseHandle(h);
                }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

static void resume_all(HANDLE *h, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        ResumeThread(h[i]);
        CloseHandle(h[i]);
    }
}

/* ------------------------------------------------------------------ */
/* self test (opt-in)                                                 */
/* ------------------------------------------------------------------ */
static int env_true(const char *name)
{
    char v[16];
    DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    if (n == 0 || n >= sizeof(v))
        return 0;
    return (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T');
}

static int self_test_enabled(void)
{
    char path[MAX_PATH];
    if (env_true("HBM_POOLFIX_SELFTEST"))
        return 1;
    _snprintf(path, sizeof(path), "%s\\hbm_poolfix_selftest.on", g_exe_dir);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static PoolObj *find_target_pool(void)
{
    unsigned count = *(unsigned char *)(g_base + RVA_POOL_COUNT);
    unsigned i;
    if (count > 32)
        count = 32;
    for (i = 0; i < count; i++) {
        PoolObj *p = *(PoolObj **)(g_base + RVA_POOL_ARRAY + i * 4);
        if (!p)
            continue;
        if (p->arena == (unsigned)(ULONG_PTR)(g_base + RVA_CRASH_ARENA))
            return p;
    }
    if (count)
        return *(PoolObj **)(g_base + RVA_POOL_ARRAY + (count - 1) * 4);
    return NULL;
}

static void run_self_test(void)
{
    PoolObj *pool;
    void *p, *q;
    unsigned used0;
    unsigned char *b;
    unsigned i;
    int ok = 1;

    pool = find_target_pool();
    if (!pool) {
        log_msg("selftest: no target pool found");
        return;
    }

    used0 = pool->used;
    p = test_alloc(pool, 0x40000);
    if (!p) {
        log_msg("selftest: FAIL alloc returned NULL");
        return;
    }
    if (!ext_owner(p)) {
        log_msg("selftest: FAIL block %p is not in an external arena", p);
        ok = 0;
    }
    if (*(unsigned *)((unsigned char *)p - 4) != 0x40000) {
        log_msg("selftest: FAIL block header is 0x%X, expected 0x40000",
                *(unsigned *)((unsigned char *)p - 4));
        ok = 0;
    }
    if (pool->used != used0) {
        log_msg("selftest: FAIL original pool accounting changed (%u -> %u)",
                used0, pool->used);
        ok = 0;
    }

    b = (unsigned char *)p;
    for (i = 0; i < 0x40000; i += 4096)
        b[i] = (unsigned char)(i >> 12);

    test_free(pool, p);

    for (i = 0; i < 0x40000; i += 4096) {
        if (b[i] != (unsigned char)(i >> 12)) {
            log_msg("selftest: FAIL block contents changed at +0x%X", i);
            ok = 0;
            break;
        }
    }
    if (pool->used != used0) {
        log_msg("selftest: FAIL original pool accounting after free (%u -> %u)",
                used0, pool->used);
        ok = 0;
    }

    q = test_alloc(pool, 0x40000);
    if (q != p) {
        log_msg("selftest: note: block not reused (first=%p second=%p)", p, q);
    }
    if (q)
        test_free(pool, q);

    log_msg("selftest: %s (pool=%p, block=%p)", ok ? "PASS" : "FAIL",
            pool, p);
}

/* ------------------------------------------------------------------ */
/* optional stress test: reproduces the real crash path               */
/* ------------------------------------------------------------------ */
static union { unsigned char b[0x40]; unsigned u; } g_fake_table;

static void stress_test(PoolObj *pool)
{
    void *blocks[512];
    int nblocks = 0, ext_blocks = 0;
    unsigned i;
    unsigned used_before = pool->used;
    unsigned char *p;

    memset(&g_fake_table, 0, sizeof(g_fake_table));
    *(void **)(g_fake_table.b + 0x00) = (void *)(g_base + RVA_TABLE_VTABLE);
    *(unsigned *)(g_fake_table.b + 0x0C) = 0x20;
    *(unsigned *)(g_fake_table.b + 0x10) = 0x140;
    *(unsigned *)(g_fake_table.b + 0x18) = 0x0A;
    *(unsigned *)(g_fake_table.b + 0x2C) = (unsigned)(ULONG_PTR)pool;

    /* ~63 node blocks: the original 34 KB arena only holds ~26 */
    for (i = 0; i < 2000; i++) {
        if (!test_insert(g_fake_table.b, 0x10000000u + i)) {
            log_msg("stress: insert %u returned NULL", i);
            break;
        }
    }

    p = *(unsigned char **)(g_fake_table.b + 4);
    while (p && nblocks < (int)(sizeof(blocks) / sizeof(blocks[0]))) {
        blocks[nblocks++] = p;
        if (ext_owner(p))
            ext_blocks++;
        p = *(unsigned char **)(p + 4);
    }

    log_msg("stress: %u inserts, %d blocks (%d external), pool used=0x%X",
            i, nblocks, ext_blocks, pool->used);

    for (i = 0; i < (unsigned)nblocks; i++)
        test_table_free_block(g_fake_table.b, blocks[i]);

    log_msg("stress: freed, original pool used 0x%X -> 0x%X, "
            "fallback allocations so far=%ld, %s",
            used_before, pool->used, g_fallback_count,
            (pool->used == used_before && ext_blocks > 0) ? "PASS" : "CHECK");
}

/* ------------------------------------------------------------------ */
/* install                                                            */
/* ------------------------------------------------------------------ */
static const unsigned char kAllocPattern[8] =
    { 0x8B, 0x41, 0x10, 0x56, 0x8B, 0x74, 0x24, 0x08 };
static const unsigned char kFreePattern[5] =
    { 0x3B, 0x41, 0x0C, 0x76, 0x05 };
static const unsigned char kInsertPattern[8] =
    { 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x04, 0x85, 0xC0 };

static int code_ready(void)
{
    unsigned char count;
    unsigned i, good = 0;

    if (!memory_readable(g_base + RVA_ALLOC, 8) ||
        !memory_readable(g_base + RVA_FREE_CHECK, 5) ||
        !memory_readable(g_base + RVA_INSERT, 8) ||
        !memory_readable(g_base + RVA_POOL_COUNT, 0x20))
        return 0;

    if (memcmp(g_base + RVA_ALLOC, kAllocPattern, 8) != 0)
        return 0;
    if (memcmp(g_base + RVA_FREE_CHECK, kFreePattern, 5) != 0)
        return 0;
    if (memcmp(g_base + RVA_INSERT, kInsertPattern, 8) != 0)
        return 0;

    count = *(unsigned char *)(g_base + RVA_POOL_COUNT);
    if (count == 0 || count > 32)
        return 0;
    for (i = 0; i < count; i++) {
        PoolObj *p = *(PoolObj **)(g_base + RVA_POOL_ARRAY + i * 4);
        if (!p)
            continue;
        if (p->arena >= (unsigned)(ULONG_PTR)g_base &&
            p->arena < (unsigned)(ULONG_PTR)(g_base + 0x5C8000) &&
            p->arena_size > 0x1000 && p->head > p->arena &&
            p->head < p->arena + p->arena_size)
            good++;
    }
    return good == count;
}

static DWORD WINAPI init_thread(LPVOID unused)
{
    int i;
    int ready = 0;
    int patched_free = 0, patched_alloc = 0;
    HANDLE suspended[MAX_SUSPEND];
    int nsusp = 0;

    (void)unused;

    g_base = (unsigned char *)GetModuleHandleW(NULL);
    if (!g_base) {
        log_msg("GetModuleHandle(NULL) failed: %lu", GetLastError());
        return 0;
    }

    log_msg("hbm_poolfix: module base %p (expected %p)",
            g_base, (void *)0x400000);

    /* wait for Steam DRM to finish unpacking the .text section */
    for (i = 0; i < 2400; i++) {          /* up to 120 s */
        if (code_ready()) {
            ready = 1;
            break;
        }
        Sleep(50);
    }
    if (!ready) {
        log_msg("hbm_poolfix: timeout waiting for unpacked game code; "
                "not patching");
        return 0;
    }
    Sleep(300);                            /* let unpacking settle */
    if (!code_ready()) {
        log_msg("hbm_poolfix: code pattern changed while settling; "
                "not patching");
        return 0;
    }

    g_alloc_resume = g_base + RVA_ALLOC_RESUME;
    g_free_entry   = g_base + RVA_FREE_ENTRY;
    g_free_cont    = g_base + RVA_FREE_CONT;
    g_free_assert  = g_base + RVA_FREE_ASSERT;
    g_insert_fn    = g_base + RVA_INSERT;

    /* make sure at least one external arena exists */
    EnterCriticalSection(&g_ext_lock);
    if (g_ext_count == 0)
        ext_pool_create(0);
    LeaveCriticalSection(&g_ext_lock);
    if (g_ext_count == 0) {
        log_msg("hbm_poolfix: could not create external arena; not patching");
        return 0;
    }

    nsusp = suspend_others(suspended, MAX_SUSPEND);

    patched_free = patch_jmp(g_base + RVA_FREE_CHECK,
                             (void *)free_hook_entry, 5);
    patched_alloc = patch_jmp(g_base + RVA_ALLOC,
                              (void *)alloc_hook_entry, 8);

    log_msg("hbm_poolfix: hooks installed: free=%d alloc=%d "
            "(%d threads suspended)", patched_free, patched_alloc, nsusp);

    if (patched_free && patched_alloc && self_test_enabled()) {
        PoolObj *pool = find_target_pool();
        run_self_test();
        if (pool)
            stress_test(pool);
    }

    resume_all(suspended, nsusp);

    return 0;
}

/* ------------------------------------------------------------------ */
/* DLL entry                                                          */
/* ------------------------------------------------------------------ */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    HANDLE h;

    (void)reserved;

    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    DisableThreadLibraryCalls(inst);

    g_log_path[0] = 0;
    {
        char exe[MAX_PATH];
        char *p;
        if (GetModuleFileNameA(NULL, exe, sizeof(exe))) {
            p = strrchr(exe, '\\');
            if (p) {
                *p = 0;
                _snprintf(g_exe_dir, sizeof(g_exe_dir), "%s", exe);
                _snprintf(g_log_path, sizeof(g_log_path),
                          "%s\\hbm_poolfix.log", exe);
            }
        }
    }

    InitializeCriticalSection(&g_ext_lock);
    InitializeCriticalSection(&g_log_lock);
    g_tls_bypass = TlsAlloc();

    h = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);

    return TRUE;
}
