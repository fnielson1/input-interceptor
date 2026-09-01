# input-interceptor

A user-mode C library for intercepting, filtering, and synthesising keyboard and
mouse input on Windows.

## Requirements

None beyond the DLL itself — **no kernel driver, no driver signing, no
test-signing mode, no reboot.** `src/interceptor.c` is implemented entirely on
top of three ordinary Win32 mechanisms: Raw Input (`RegisterRawInputDevices`/
`WM_INPUT`) to tell physical keyboards and mice apart, `WH_KEYBOARD_LL`/
`WH_MOUSE_LL` low-level hooks to withhold a stroke from the rest of the system,
and `SendInput` to release or synthesise one. `interceptor_create_context()`
spins up a small internal worker thread (a hidden message-only window, the two
Raw Input registrations, the two hooks) on first use and tears it down once the
last context is destroyed — nothing else to install.

A kernel-driver implementation of this same API still exists on the [`driver`
branch](../../tree/driver), for anything that specifically needs a synthesised
stroke to be indistinguishable from real hardware (see "Trade-offs" below) —
that version needs the Windows Driver Kit, test-signing, and a reboot, as before.

### Trade-offs versus the driver branch

This library's public API ([`src/interceptor.h`](src/interceptor.h)) is
identical either way, but the user-mode backend gives up a few things the
kernel driver had:

- **A released stroke carries Windows' injected-input marker permanently.**
  Anything `interceptor_send()` releases goes through `SendInput`, which
  stamps `LLKHF_INJECTED`/`LLMHF_INJECTED` on the event; nothing in user mode
  can clear it. Code that checks for that flag (some anti-cheat, some other
  input tools) can tell it apart from real hardware. The driver branch's
  reinjected strokes, synthesized below the class driver, carry no such marker.
- **Exclusive-mode DirectInput/raw-HID-only games, the secure desktop** (UAC
  consent, Ctrl+Alt+Del), **and the console** can bypass both Raw Input and
  low-level hooks entirely, since neither taps in below the Win32 input stack.
- **`INTERCEPTOR_FILTER_KEY_TERMSRV_*` filter bits never match locally** —
  those only arise inside an actual Remote Desktop session's keyboard stack,
  which isn't visible to either mechanism this backend uses.
- **Precedence only orders opens within this process.** The driver could
  arbitrate multiple simultaneous processes system-wide; nothing in user mode
  can override the order Windows itself calls different processes' low-level
  hooks in.
- **Device-slot assignment doesn't survive unplug/replug.** A slot is bound to
  a Raw Input device handle for the life of the process and never freed, so
  reconnecting a keyboard or mouse consumes a new slot rather than rebinding
  to its old one. The driver binds by persistent PnP instance ID, which does
  survive a replug.
- `interceptor_get_hardware_id()` returns the Raw Input device interface path
  (e.g. `\\?\HID#VID_046D&PID_C52B&...`) rather than the driver's bare PnP
  hardware ID string. `VID_`/`PID_` substrings are present for USB HID
  devices either way, but exact-string comparisons against IDs captured from
  the driver build will need updating.

For remapping, accessibility, and automation on a normal desktop session — this
library's stated use case — none of the above usually matters. See
[`src/interceptor.c`](src/interceptor.c)'s file header for the full architecture
writeup, including why the Raw Input/hook correlation is best-effort and fails
open (passes an event through unfiltered) rather than misattributing or losing
it.

Also note the usual UIPI caveat: a process using this library only reliably
sees and can suppress input destined for windows at its own integrity level or
lower, so to intercept input to an elevated application you must run elevated
yourself.

## Building the library

### Modern Visual Studio (recommended)

```
src\build-msvc.cmd
```

Needs any Visual Studio install with the **C++ x64 toolset**; the script locates
it with `vswhere` and calls `vcvars64.bat` itself. Output lands in
`src\build\x64\`: `interceptor.dll`, plus `interceptor.lib` to link against.
The public header stays at `src\interceptor.h`.

`interceptor.c` calls no CRT function, so the DLL links `/NODEFAULTLIB` against
`kernel32` alone — no msvcrt import and no VC++ redistributable dependency on
the target machine.

### Legacy WDK

The original upstream build, kept for reference. It needs the old WDK 7.1 build
environment (`%WDK%\bin\setenv`), which no longer ships with current kits:

```
src\buildit.cmd        rem x86, WXP
src\buildit-x64.cmd    rem x64, WIN7
```

## Using it

Link against `interceptor.lib`, include `interceptor.h`, and ship
`interceptor.dll` alongside your executable. A minimal filter that swaps the
`x` and `y` keys:

```c
#include <interceptor.h>

int main(void)
{
    InterceptorContext context = interceptor_create_context();
    InterceptorDevice  device;
    InterceptorStroke  stroke;

    interceptor_set_filter(context, interceptor_is_keyboard,
                           INTERCEPTOR_FILTER_KEY_DOWN | INTERCEPTOR_FILTER_KEY_UP);

    while (interceptor_receive(context, device = interceptor_wait(context), &stroke, 1) > 0)
    {
        InterceptorKeyStroke *key = (InterceptorKeyStroke *)&stroke;

        if (key->code == 0x2D) key->code = 0x15;        /* x -> y */
        else if (key->code == 0x15) key->code = 0x2D;   /* y -> x */

        interceptor_send(context, device, &stroke, 1);

        if (key->code == 0x01) break;                   /* esc quits */
    }

    interceptor_destroy_context(context);
    return 0;
}
```

The shape of every program is the same: create a context, declare which stroke
kinds you want with `interceptor_set_filter`, then loop on `interceptor_wait` /
`interceptor_receive`. **A stroke you receive is consumed** — it does not reach
the system unless you pass it back with `interceptor_send`. That is what makes
blocking and rewriting possible, and also what makes an unresponsive filter
process freeze the input it filters.

Define `INTERCEPTOR_STATIC` before including the header if you are compiling the
sources directly into your program rather than linking against the DLL.

### API

```c
InterceptorContext    interceptor_create_context(void);
void                  interceptor_destroy_context(InterceptorContext context);

void                  interceptor_set_filter(InterceptorContext, InterceptorPredicate, InterceptorFilter);
InterceptorFilter     interceptor_get_filter(InterceptorContext, InterceptorDevice);

InterceptorDevice     interceptor_wait(InterceptorContext);
InterceptorDevice     interceptor_wait_with_timeout(InterceptorContext, unsigned long milliseconds);
int                   interceptor_receive(InterceptorContext, InterceptorDevice, InterceptorStroke *, unsigned int nstroke);
int                   interceptor_send(InterceptorContext, InterceptorDevice, const InterceptorStroke *, unsigned int nstroke);

InterceptorPrecedence interceptor_get_precedence(InterceptorContext, InterceptorDevice);
void                  interceptor_set_precedence(InterceptorContext, InterceptorDevice, InterceptorPrecedence);

unsigned int          interceptor_get_hardware_id(InterceptorContext, InterceptorDevice, void *buffer, unsigned int size);

int                   interceptor_is_invalid(InterceptorDevice);
int                   interceptor_is_keyboard(InterceptorDevice);
int                   interceptor_is_mouse(InterceptorDevice);
```

Devices are addressed as `INTERCEPTOR_KEYBOARD(0..9)` and `INTERCEPTOR_MOUSE(0..9)`.
`InterceptorStroke` is a raw buffer you cast to `InterceptorKeyStroke` or
`InterceptorMouseStroke` depending on `interceptor_is_keyboard` /
`interceptor_is_mouse`. Full enumerations of key states, mouse states, mouse
flags, and the corresponding filter masks are in
[`src/interceptor.h`](src/interceptor.h).

## Samples

In [`samples/`](samples/), each in its own directory with a WDK `sources` file
and a `buildit.cmd`. They share [`samples/utils.c`](samples/utils.c) and all
build against the library in `src\`.

| Sample | What it does |
| --- | --- |
| [`identify`](samples/identify) | Prints which device index produced each stroke — run this first to find your hardware |
| [`hardwareid`](samples/hardwareid) | Prints the hardware ID string of the device you press a key on |
| [`x2y`](samples/x2y) | Swaps the `x` and `y` keys |
| [`caps2esc`](samples/caps2esc) | Caps Lock acts as Escape when tapped, Ctrl when held. Windows port of [caps2esc](https://github.com/alexandre/caps2esc). Runs windowless — terminate it from Task Manager |
| [`axes`](samples/axes) | Inverts the mouse Y axis |
| [`cadstop`](samples/cadstop) | Blocks Ctrl+Alt+Del by swallowing the third modifier once two are already held |
| [`mathpointer`](samples/mathpointer) | Drives the pointer along parametric curves (rose, spiral, hypotrochoid, butterfly, …) |

Every console sample exits on **Escape**. Since a running filter sits in your
input path, always have a way to kill it — Escape, or Task Manager for
`caps2esc`.

### Building a sample

The samples' `sources` files are set up for the legacy WDK build and link
`..\..\src\objfre_wxp_x86\i386\interceptor.lib`, i.e. the WDK output of the
library. To build one against the `build-msvc.cmd` output instead, compile it
directly from a developer prompt:

```
cl /nologo /EHsc /I src /I samples samples\x2y\x2y.cpp samples\utils.c ^
   src\build\x64\interceptor.lib user32.lib /Fe:x2y.exe
```

## Upstream compatibility

This library's public API is intentionally shaped like
[Interception](https://github.com/oblitum/Interception)'s, a well-known
prior project in this space, so code written against that API is easy to
port:

| There | Here |
| --- | --- |
| `interception_*` functions | `interceptor_*` |
| `Interception*` types | `Interceptor*` |
| `INTERCEPTION_*` macros and enums | `INTERCEPTOR_*` |
| `interception.h` / `.c` / `.dll` / `.lib` | `interceptor.h` / `.c` / `.dll` / `.lib` |

Porting existing code is a case-sensitive search and replace of `interception`
→ `interceptor` in those three casings. Semantics, struct layouts, and filter
bit values match, so a program ported this way behaves identically. `src/`
and `samples/` here are original implementations of that same public API —
see [License](#license) — not a derivative port of that project's actual
source. (The one sample with a distinct, separately-credited origin is
`caps2esc`, ported from a different, unrelated upstream project — see its own
table entry and file header.)

Unlike that project (and unlike this repository's own [`driver`
branch](../../tree/driver)), `main` speaks no `\\.\interceptionNN` device
wire protocol at all — see "Requirements" above for the Raw Input/hook/
SendInput backend `src/interceptor.c` uses instead.

This repository also adds `src\build-msvc.cmd` and `src\interceptor-standalone.rc`
for building with a current Visual Studio toolchain without the WDK, and
`src\dllmain.c` as the entry point for that build.

## Layout

```
src/                         the library
  interceptor.h                public API
  interceptor.c                implementation (Raw Input + WH_KEYBOARD_LL/WH_MOUSE_LL + SendInput)
  dllmain.c                    entry point for the standalone MSVC build
  interceptor.rc               version resource (WDK build)
  interceptor-standalone.rc    version resource (MSVC build)
  build-msvc.cmd               modern Visual Studio build
  buildit.cmd                  legacy WDK build, x86
  buildit-x64.cmd              legacy WDK build, x64
  sources, makefile            WDK build definitions
samples/                     example filters, one per directory
tools/scd.cmd                short-path cd helper used by the WDK build scripts
```

The kernel-driver backend (`driver/`, plus `protocol.h`'s IOCTL codes and the
`\\.\interceptionNN` control devices) lives only on the [`driver`
branch](../../tree/driver), not on `main`.

## Legal and safety

This library intercepts and synthesises input system-wide. Use it on machines
you control, for input remapping, accessibility, and automation. Note that
anti-cheat and endpoint-security products commonly flag or block low-level
input hooks and filter drivers alike, and some online games treat them as a
terms-of-service violation.

## License

MIT — see [LICENSE](LICENSE) — for the whole repository.

`src/` and `samples/` (this branch), and `driver/` (the [`driver`
branch](../../tree/driver)), are original implementations of the public API
popularized by [Interception](https://github.com/oblitum/Interception),
written without reference to that project's actual source: `driver/` because
that project's kernel driver source has never been published under any
license (it's offered only as a compiled binary, with source access sold
separately as a commercial license); `src/` and `samples/` as clean-room
rewrites — fresh implementations written from functional specifications of
the API and each sample's documented behavior only, by someone who had not
seen that project's actual (LGPL-3.0-licensed) library or sample source.
None of these are a derivative of that project's code. The one exception is
`samples/caps2esc`, whose *remapping concept* (not its code) is credited to
a separate, unrelated project, [caps2esc](https://github.com/alexandre/caps2esc)
by Alexandre — see that sample's own file header.
