#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

static FILE *g_log = NULL;
static void Log(const char *fmt, ...) {
    if (!g_log) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fflush(g_log);
}

static DWORD g_playerManagerStatic = 0x011FD0B4;

typedef void *(__fastcall *SlotFn_t)(void *thisPtr, void *unused_edx, unsigned int param2);
static SlotFn_t oSlotFn = NULL;

/* ---- inline hook helper (patches the function's own code, works
   regardless of vtable/IAT caching) ---- */
static void *MakeTrampoline(void *target, int patchLen) {
    unsigned char *tramp = (unsigned char *)VirtualAlloc(NULL, patchLen + 5,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tramp, target, patchLen);
    unsigned char *jmpBack = tramp + patchLen;
    jmpBack[0] = 0xE9;
    intptr_t rel = (intptr_t)((unsigned char *)target + patchLen) - (intptr_t)(jmpBack + 5);
    memcpy(jmpBack + 1, &rel, 4);
    return tramp;
}

static void PatchJump(void *target, void *hookFn, int patchLen) {
    DWORD old, tmp;
    VirtualProtect(target, patchLen, PAGE_EXECUTE_READWRITE, &old);
    unsigned char *t = (unsigned char *)target;
    t[0] = 0xE9;
    intptr_t rel = (intptr_t)hookFn - (intptr_t)(t + 5);
    memcpy(t + 1, &rel, 4);
    for (int i = 5; i < patchLen; i++) t[i] = 0x90;
    VirtualProtect(target, patchLen, old, &tmp);
}

typedef SHORT(WINAPI *GetAsyncKeyState_t)(int);
static GetAsyncKeyState_t oGetAsyncKeyState = NULL;
static volatile int g_freecamActive = 0;

/* player's own world position (distinct from the camera transform), used to
   freeze the character in place while freecam is active by continuously
   re-writing it every frame, overriding whatever movement code changed it to. */
static DWORD g_localPlayerAddr = 0;
static float g_frozenPX, g_frozenPY, g_frozenPZ;
#define PLAYER_POS_OFFSET 0x684

/* MainConsole: found by resolving the same static-storage pattern used for
   PlayerManager, but MainConsole is a static/global object (not heap), so
   this address is stable across runs. We drive it exactly like a human
   typing at the console: fill its input-line buffer (obj+0x64) and length
   (obj+0xEC), then invoke its "on character" vtable method (slot+0x14) with
   '\r' to submit -- the game's own code builds the std::string and calls
   ExecuteCommand internally, so we never have to construct one ourselves. */
#define MAINCONSOLE_STATIC 0x0112FE80
#define CONSOLE_LINEBUF_OFFSET 0x64
#define CONSOLE_LINELEN_OFFSET 0xEC
static DWORD g_mainConsoleAddr = 0;
static int g_hudVisible = 1;

typedef void (__fastcall *OnCharFn_t)(void *, void *, char);

static void ExecConsoleCommand(DWORD consoleObj, const char *cmd) {
    if (!consoleObj) return;
    __try {
        size_t len = strlen(cmd);
        if (len >= 100) return; /* stay well inside the line buffer */
        char *buf = (char *)(consoleObj + CONSOLE_LINEBUF_OFFSET);
        memcpy(buf, cmd, len + 1);
        *(DWORD *)(consoleObj + CONSOLE_LINELEN_OFFSET) = (DWORD)len;

        DWORD vtable = *(DWORD *)consoleObj;
        OnCharFn_t onChar = (OnCharFn_t)(*(void **)(vtable + 0x14));
        onChar((void *)consoleObj, NULL, '\r');
        Log("console exec: %s\n", cmd);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("EXCEPTION executing console command: %s\n", cmd);
    }
}

static int IsMovementKey(int vKey) {
    return vKey == 'W' || vKey == 'A' || vKey == 'S' || vKey == 'D' ||
           vKey == VK_SPACE || vKey == VK_LCONTROL;
}

static SHORT WINAPI hkGetAsyncKeyState(int vKey) {
    if (g_freecamActive && IsMovementKey(vKey)) return 0;
    return oGetAsyncKeyState(vKey);
}

/* our own freecam control code must always see the REAL key state,
   bypassing the hook above (which fakes 0 for movement keys to the game) */
#define RealKey(vk) (oGetAsyncKeyState ? oGetAsyncKeyState(vk) : GetAsyncKeyState(vk))

typedef BOOL(WINAPI *GetKeyboardState_t)(PBYTE);
static GetKeyboardState_t oGetKeyboardState = NULL;
static const int kMovementVKs[] = { 'W', 'A', 'S', 'D', VK_SPACE, VK_LCONTROL, VK_LBUTTON, VK_RBUTTON };

static BOOL WINAPI hkGetKeyboardState(PBYTE lpKeyState) {
    BOOL r = oGetKeyboardState(lpKeyState);
    if (r && g_freecamActive && lpKeyState) {
        for (size_t i = 0; i < sizeof(kMovementVKs) / sizeof(kMovementVKs[0]); i++) {
            lpKeyState[kMovementVKs[i]] &= 0x7F; /* clear high bit (pressed) */
        }
    }
    return r;
}

/* generously oversized: the real structure returned by the game may contain
   more than just the 16-float transform (extra camera params the caller
   might read past our matrix), so we copy the real data as a base each
   frame and only overwrite the transform portion, instead of returning a
   bare 16-float buffer that would leave anything beyond it uninitialized. */
static unsigned char g_camBuf[1024];
#define g_camMatrix ((float *)g_camBuf)
static float g_camX, g_camY, g_camZ, g_camYaw, g_camPitch;
static int g_haveMouseAnchor = 0;
static POINT g_lastMouse;
static ULONGLONG g_lastTick = 0;

static void BuildMatrix(float *m, float ex, float ey, float ez, float yaw, float pitch) {
    /* forward vector */
    float fx = cosf(pitch) * sinf(yaw);
    float fy = sinf(pitch);
    float fz = cosf(pitch) * cosf(yaw);
    /* right = cross(worldUp(0,1,0), forward) -- matches this engine's handedness */
    float rx = fz, ry = 0.0f, rz = -fx;
    float rlen = sqrtf(rx*rx + rz*rz);
    if (rlen < 0.0001f) rlen = 1.0f;
    rx /= rlen; rz /= rlen;
    /* up = cross(forward, right) */
    float ux = fy*rz - fz*ry;
    float uy = fz*rx - fx*rz;
    float uz = fx*ry - fy*rx;

    /* row-major 4x4, matches observed layout: row0=right,row1=up,row2=forward,row3=pos */
    m[0]=rx; m[1]=ry; m[2]=rz; m[3]=0;
    m[4]=ux; m[5]=uy; m[6]=uz; m[7]=0;
    m[8]=fx; m[9]=fy; m[10]=fz; m[11]=0;
    m[12]=ex; m[13]=ey; m[14]=ez; m[15]=1;
}

/* The camera object seen right after process launch (menu/hero-select preview
   scene) is a DIFFERENT C++ class - and therefore a different vtable - from
   the one actually used once the player is in a live match. Patching only the
   first vtable we ever see means the hook silently does nothing once the
   player joins a real game. So instead of patching once, we keep a small
   table of every distinct vtable we've patched (and each one's original
   slot+0x10 function), and hkSlotFn_impl looks up thisPtr's own vtable in
   that table each call to know which original function to forward to. A
   background loop in MainThread keeps re-resolving the live camera and adds
   any newly-seen vtable to the table on the fly. */
#define MAX_PATCHED_VTABLES 8
static DWORD g_patchedVtables[MAX_PATCHED_VTABLES];
static SlotFn_t g_patchedOrigFns[MAX_PATCHED_VTABLES];
static volatile int g_patchedCount = 0;

static void *__fastcall hkSlotFn(void *thisPtr, void *edx_unused, unsigned int param2);

static void PatchVtableSlot(DWORD vtable) {
    int count = g_patchedCount;
    for (int i = 0; i < count; i++) {
        if (g_patchedVtables[i] == vtable) return; /* already patched */
    }
    if (count >= MAX_PATCHED_VTABLES) return;

    void **slotAddr = (void **)(vtable + 0x10);
    void *origFn = *slotAddr;
    if (origFn == (void *)hkSlotFn) return; /* somehow already our own hook */

    DWORD oldProtect, tmp;
    VirtualProtect(slotAddr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
    *slotAddr = (void *)hkSlotFn;
    VirtualProtect(slotAddr, 4, oldProtect, &tmp);

    g_patchedOrigFns[count] = (SlotFn_t)origFn;
    g_patchedVtables[count] = vtable; /* publish vtable last: a reader that
        sees the incremented count below always finds a fully-written slot */
    g_patchedCount = count + 1;
    Log("patched camera vtable=0x%08X slot+0x10 original fn=%p (total patched=%d)\n",
        vtable, origFn, count + 1);
}

static SlotFn_t FindOrigFnForVtable(DWORD vtable) {
    int count = g_patchedCount;
    for (int i = 0; i < count; i++) {
        if (g_patchedVtables[i] == vtable) return g_patchedOrigFns[i];
    }
    return oSlotFn; /* fallback: shouldn't happen, but forward through anyway
        rather than crash if some other codepath reaches an unpatched vtable */
}

static void __fastcall hkSlotFn_impl(void *thisPtr, unsigned int param2, void **retSlot) {
    SlotFn_t myOrig = FindOrigFnForVtable(*(DWORD *)thisPtr);

    /* write the frozen player position BEFORE the original runs, so any
       pivot-sync logic inside it observes the frozen value from the start
       of the frame instead of a stale/inconsistent one written after. */
    if (g_freecamActive && g_localPlayerAddr) {
        __try {
            float *pp = (float *)(g_localPlayerAddr + PLAYER_POS_OFFSET);
            pp[0] = g_frozenPX; pp[1] = g_frozenPY; pp[2] = g_frozenPZ;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }

    void *realResult = (void *)myOrig(thisPtr, NULL, param2);

    static int prevF10 = 0;
    int f10 = (RealKey(VK_F10) & 0x8000) != 0;
    if (f10 && !prevF10) {
        g_freecamActive = !g_freecamActive;
        Log("F10 toggled -> freecam=%d\n", g_freecamActive);
        if (g_freecamActive && realResult) {
            float *rm = (float *)realResult;
            g_camX = rm[12]; g_camY = rm[13]; g_camZ = rm[14];
            g_camYaw = atan2f(rm[8], rm[10]);
            g_camPitch = asinf(rm[9] > 1 ? 1 : (rm[9] < -1 ? -1 : rm[9]));
            g_haveMouseAnchor = 0;
            Log("freecam start pos=(%.2f,%.2f,%.2f) yaw=%.2f pitch=%.2f\n",
                g_camX, g_camY, g_camZ, g_camYaw, g_camPitch);

            if (g_localPlayerAddr) {
                __try {
                    float *pp = (float *)(g_localPlayerAddr + PLAYER_POS_OFFSET);
                    g_frozenPX = pp[0]; g_frozenPY = pp[1]; g_frozenPZ = pp[2];
                    Log("froze player pos=(%.2f,%.2f,%.2f)\n", g_frozenPX, g_frozenPY, g_frozenPZ);
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    Log("failed to read player pos for freeze\n");
                }
            }
        }
    }
    prevF10 = f10;

    /* F9: toggle the HUD, independent of freecam state */
    static int prevF9 = 0;
    int f9 = (RealKey(VK_F9) & 0x8000) != 0;
    if (f9 && !prevF9 && g_mainConsoleAddr) {
        g_hudVisible = !g_hudVisible;
        ExecConsoleCommand(g_mainConsoleAddr, g_hudVisible ? "renderer.drawhud 1" : "renderer.drawhud 0");
    }
    prevF9 = f9;

    if (!g_freecamActive) {
        *retSlot = realResult;
        return;
    }

    /* position-freeze disabled: LocalPlayer+0x684 turned out to feed the
       camera's own tracked position too, so freezing it also froze the
       free-flying camera -- worse than the original "character keeps
       moving" limitation, so leaving that limitation in place for now. */

    ULONGLONG now = GetTickCount64();
    float dt = g_lastTick ? (float)(now - g_lastTick) / 1000.0f : 0.016f;
    if (dt > 0.1f) dt = 0.1f;
    g_lastTick = now;

    POINT p;
    GetCursorPos(&p);
    if (g_haveMouseAnchor) {
        float dx = (float)(p.x - g_lastMouse.x);
        float dy = (float)(p.y - g_lastMouse.y);
        g_camYaw += dx * 0.003f;
        g_camPitch -= dy * 0.003f;
        if (g_camPitch > 1.5f) g_camPitch = 1.5f;
        if (g_camPitch < -1.5f) g_camPitch = -1.5f;
    }
    g_lastMouse = p;
    g_haveMouseAnchor = 1;

    float speed = (RealKey(VK_LSHIFT) & 0x8000) ? 40.0f : 12.0f;
    float fx = cosf(g_camPitch) * sinf(g_camYaw);
    float fy = sinf(g_camPitch);
    float fz = cosf(g_camPitch) * cosf(g_camYaw);
    float rx = cosf(g_camYaw), rz = -sinf(g_camYaw);

    /* arrow keys instead of WASD: the game doesn't use them for character
       movement, so the camera and character controls can never collide and
       nothing needs to be blocked. */
    if (RealKey(VK_UP) & 0x8000) { g_camX += fx*speed*dt; g_camY += fy*speed*dt; g_camZ += fz*speed*dt; }
    if (RealKey(VK_DOWN) & 0x8000) { g_camX -= fx*speed*dt; g_camY -= fy*speed*dt; g_camZ -= fz*speed*dt; }
    if (RealKey(VK_RIGHT) & 0x8000) { g_camX += rx*speed*dt; g_camZ += rz*speed*dt; }
    if (RealKey(VK_LEFT) & 0x8000) { g_camX -= rx*speed*dt; g_camZ -= rz*speed*dt; }
    if (RealKey(VK_SPACE) & 0x8000) { g_camY += speed*dt; }
    if (RealKey(VK_LCONTROL) & 0x8000) { g_camY -= speed*dt; }

    /* preserve any fields beyond the 16-float transform that the caller may
       also read from the real structure, by using it as our base each frame */
    if (realResult) {
        __try {
            memcpy(g_camBuf, realResult, sizeof(g_camBuf));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            /* realResult had less mapped memory than sizeof(g_camBuf); the
               untouched tail of g_camBuf keeps whatever it had before,
               which is still safe (zero-initialized static storage). */
        }
    }
    BuildMatrix(g_camMatrix, g_camX, g_camY, g_camZ, g_camYaw, g_camPitch);
    *retSlot = g_camMatrix;
}

/* naked-ish trampoline: __fastcall gives us (ecx=this, edx=unused, stack=param2).
   we forward to the impl with an explicit retSlot so we can return an
   arbitrary pointer via normal C (avoids hand-written asm for the return). */
static void *__fastcall hkSlotFn(void *thisPtr, void *edx_unused, unsigned int param2) {
    void *ret = NULL;
    hkSlotFn_impl(thisPtr, param2, &ret);
    return ret;
}

static DWORD ReadDword(DWORD addr) { return *(volatile DWORD *)addr; }

static DWORD WINAPI MainThread(LPVOID param) {
    char logPath[MAX_PATH];
    if (GetTempPathA(MAX_PATH, logPath)) {
        strcat(logPath, "bfh_freecam.log");
        g_log = fopen(logPath, "w");
    }
    Log("=== freecam.dll injected ===\n");

    __try {
        DWORD mc = ReadDword(MAINCONSOLE_STATIC);
        Log("MainConsole=0x%08X\n", mc);
        if (mc) {
            DWORD mcVtable = ReadDword(mc);
            /* sanity check: vtable pointer should land inside the exe image */
            if (mcVtable > 0x00400000 && mcVtable < 0x02000000) {
                g_mainConsoleAddr = mc;
                Log("MainConsole vtable=0x%08X (accepted)\n", mcVtable);
            } else {
                Log("MainConsole vtable=0x%08X looks invalid, HUD toggle disabled\n", mcVtable);
            }
        }

        /* the game may have just been launched (menus, not spawned yet), so
           PlayerManager's local-player slot and the camera pointer it leads
           to are legitimately null until the user actually spawns. Retry
           patiently instead of aborting, so this DLL can be injected right
           at process launch and just wait for the player to get in-game. */
        DWORD playerManagerPtr = 0, localPlayer = 0, cameraPtr = 0, vtable = 0;
        int attempt = 0;
        const int MAX_ATTEMPTS = 600; /* ~20 minutes at 2s between tries */
        for (attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
            playerManagerPtr = ReadDword(g_playerManagerStatic);
            if (playerManagerPtr) {
                localPlayer = ReadDword(playerManagerPtr + 0x6C);
                if (localPlayer) {
                    cameraPtr = ReadDword(localPlayer + 0xA8);
                    if (cameraPtr) {
                        vtable = ReadDword(cameraPtr);
                        if (vtable) break;
                    }
                }
            }
            if (attempt == 0 || attempt % 15 == 0) {
                Log("waiting for player to spawn (attempt %d) pm=0x%08X lp=0x%08X cam=0x%08X\n",
                    attempt, playerManagerPtr, localPlayer, cameraPtr);
            }
            Sleep(2000);
        }
        if (!vtable) { Log("gave up waiting for player to spawn\n"); return 0; }
        Log("PlayerManagerPtr=0x%08X LocalPlayer=0x%08X CameraPtr=0x%08X Camera vtable=0x%08X (after %d attempt(s))\n",
            playerManagerPtr, localPlayer, cameraPtr, vtable, attempt + 1);
        g_localPlayerAddr = localPlayer;

        PatchVtableSlot(vtable);
        oSlotFn = g_patchedOrigFns[0]; /* kept for FindOrigFnForVtable's fallback path */

        HMODULE hUser32 = GetModuleHandleA("user32.dll");
        void *realGAKS = hUser32 ? (void *)GetProcAddress(hUser32, "GetAsyncKeyState") : NULL;
        if (realGAKS) {
            if (*(unsigned char *)realGAKS == 0xE9) {
                /* already hooked by an earlier injected copy of this DLL:
                   copying its bytes into a fresh trampoline would relocate a
                   relative JMP and jump into garbage. just call straight
                   into it instead -- a normal CALL to a fixed address is
                   always safe, unlike copying relative-jump bytes elsewhere. */
                oGetAsyncKeyState = (GetAsyncKeyState_t)realGAKS;
                Log("GetAsyncKeyState already hooked, reusing existing hook @ %p\n", realGAKS);
            } else {
                oGetAsyncKeyState = (GetAsyncKeyState_t)MakeTrampoline(realGAKS, 5);
                PatchJump(realGAKS, (void *)hkGetAsyncKeyState, 5);
                Log("GetAsyncKeyState hooked @ %p\n", realGAKS);
            }
        } else {
            Log("could not find GetAsyncKeyState, WASD blocking disabled\n");
        }

        void *realGKS = NULL; /* temporarily disabled to isolate a click-crash */
        if (realGKS) {
            if (*(unsigned char *)realGKS == 0xE9) {
                oGetKeyboardState = (GetKeyboardState_t)realGKS;
                Log("GetKeyboardState already hooked, reusing existing hook @ %p\n", realGKS);
            } else {
                oGetKeyboardState = (GetKeyboardState_t)MakeTrampoline(realGKS, 5);
                PatchJump(realGKS, (void *)hkGetKeyboardState, 5);
                Log("GetKeyboardState hooked @ %p\n", realGKS);
            }
        } else {
            Log("could not find GetKeyboardState\n");
        }

        Log("Hook installed. F10 to toggle freecam.\n");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("EXCEPTION during setup, code=0x%08X\n", GetExceptionCode());
    }

    /* Keep watching: the live camera object/class can change after this
       point (menu/preview camera -> real in-match camera, death cam,
       spectator, vehicles, ...), so keep patching any newly-seen vtable
       instead of only ever hooking the first one found above. */
    for (;;) {
        Sleep(1000);
        __try {
            DWORD pm = ReadDword(g_playerManagerStatic);
            if (!pm) continue;
            DWORD lp = ReadDword(pm + 0x6C);
            if (!lp) continue;
            g_localPlayerAddr = lp;
            DWORD cam = ReadDword(lp + 0xA8);
            if (!cam) continue;
            DWORD vt = ReadDword(cam);
            if (!vt) continue;
            PatchVtableSlot(vt);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
    }
    return TRUE;
}
