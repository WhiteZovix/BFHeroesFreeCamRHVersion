# Battlefield Heroes — Freecam DLL

A small injected DLL that adds a free-flying camera (`F10`) and a HUD toggle
(`F9`) to Battlefield Heroes 1.42 (`BFHeroes.exe`, 32-bit). Built for use with
the BFHeroes Launcher, which injects it automatically on every game launch —
no manual steps needed by players.

## Controls

| Key | Action |
|---|---|
| `F10` | Toggle freecam (detaches the camera from the player) |
| `F9` | Toggle the in-game HUD (independent of freecam state) |
| Arrow keys | Move the camera while in freecam |
| Mouse | Look around while in freecam |
| `Space` | Move up |
| `Left Ctrl` | Move down |
| `Left Shift` | 3x movement speed |

The player's character is **not** frozen while in freecam — WASD still moves
it if pressed. Freecam intentionally uses the arrow keys instead of WASD so
the two controls never collide and nothing needs to be blocked.

## Files

- `freecam.c` — DLL source (single file, no external deps beyond the CRT and `user32.lib`)
- `freecam.dll` — prebuilt 32-bit release build
- `inject.ps1` — `CreateRemoteThread` + `LoadLibraryA` injector, must run under 32-bit PowerShell (the game is a 32-bit process)

## How it works

**Injection.** `inject.ps1` does a standard remote-thread `LoadLibraryA`
injection. It must run as a 32-bit process — a 64-bit injector resolves
`LoadLibraryA` from the wrong-bitness `kernel32.dll` and the call silently
"succeeds" while doing nothing.

**Finding the game's objects.** `PlayerManager` lives at a fixed static
address (`0x011FD0B4`). From there: `PlayerManager+0x6C` → `LocalPlayer`,
`LocalPlayer+0xA8` → the active `Camera` object, whose vtable slot `+0x10` is
the per-frame function that returns the camera's transform matrix. All of
these are null until the player actually spawns, so `MainThread` retries
every 2s (up to ~20 minutes) until the chain resolves.

**Camera hook.** Rather than patching the function's machine code, the DLL
overwrites a single vtable slot pointer (`vtable+0x10`) to point at its own
function. This is far safer than inline byte-patching for a C++ virtual
method and requires no trampoline-of-trampoline handling.

Important subtlety: **the menu/hero-select preview camera and the real
in-match camera are different C++ classes with different vtables.** Patching
only the vtable seen right after process launch means the hook does nothing
once the player actually joins a game. `freecam.c` handles this by keeping a
small table of every distinct camera vtable it has seen (`PatchVtableSlot` /
`FindOrigFnForVtable`, capped at `MAX_PATCHED_VTABLES = 8`) and a background
loop in `MainThread` that keeps re-resolving the live camera every second and
patches any newly-seen vtable on the fly — covering menu → in-match →
spectator/death-cam transitions without needing to know each class up front.

When freecam is active, the hooked function builds its own view matrix
(`BuildMatrix`) from a tracked position/yaw/pitch instead of returning the
game's own result, and reads mouse delta + arrow keys/Space/Ctrl/Shift each
frame to fly around.

**HUD toggle.** There's no in-game console UI, but `MainConsole` (a static
object at `0x0112FE80`) still has one internally. `ExecConsoleCommand` drives
it exactly like a human typing: it fills the console's raw input-line buffer
and length field, then invokes the console's own "on character" vtable
method (`slot+0x14`) with `'\r'` — the game's own code builds the
`std::string` and executes the command (`renderer.drawhud 0` / `1`), so
there's no need to reconstruct MSVC's `std::string` ABI by hand.

**Key detection.** An inline hook on `user32.dll`'s `GetAsyncKeyState`
intercepts all callers (game and DLL alike). If a second copy of this DLL
gets injected into an already-hooked process, `MainThread` detects the
existing `0xE9 JMP` at the function's first byte and calls straight into it
instead of re-trampolining — copying an already-relocated relative jump into
a fresh trampoline would corrupt its offset and crash the game.

## Building

Requires an x86 (32-bit target) MSVC toolchain:

```bat
cl /LD /MT /O2 /EHa freecam.c /Fe:freecam.dll user32.lib
```
