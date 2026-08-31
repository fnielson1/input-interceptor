# input-interceptor

A user-mode C library for intercepting, filtering, and synthesising keyboard and
mouse input on Windows at the device level, below the Win32 input stack.


Because it works at the driver level rather than through `SetWindowsHookEx`, it
sees input that hooks cannot (DirectInput/raw-input games, the console) and its
synthesised strokes are indistinguishable from real hardware to everything above
the driver.

## Requirements

**A kernel driver must be built and installed separately.** This repository
now ships its own driver source under [`driver/`](driver/) — an original
implementation of the same `\\.\interception00`..`19` wire protocol
`src/interceptor.c` speaks, not a copy of or a runtime dependency on any
third-party driver. Without it installed, `interceptor_create_context()`
returns `0` and nothing works.

Building it needs the **Windows Driver Kit** (the "Windows Driver Kit" Visual
Studio component, matching your installed SDK version, or the standalone WDK
installer from Microsoft) in addition to the C++ x64 toolset `src\build-msvc.cmd`
already needs — unlike the DLL, kernel code has no WDK-free build path. Then:

```
driver\build-wdk.cmd
```

Output lands in `driver\build\x64\Debug\`: `interceptor-driver.sys`,
`interceptor-driver.inf`, `interceptor-driver.cat`, `interceptor-driver.cer`.
Install it from an **elevated** command prompt:

```
driver\install-wdk.cmd
```

This enables test-signing mode if it isn't already on (`bcdedit /set
testsigning on` — a locally built driver is not release-signed, so it will
only load with test-signing enabled), imports the build's test certificate
into Trusted Root/Trusted Publishers, and runs `pnputil /add-driver ...
/install`. **Reboot** afterwards — required if test-signing just got turned
on, and recommended regardless so the class filter attaches to your
existing keyboard/mouse.

To remove it later, from an elevated prompt:

```
driver\uninstall-wdk.cmd
```

which unpublishes the driver package and reminds you to `bcdedit /set
testsigning off` (then reboot) once you're done developing against it.

Note this is a system-wide keyboard/mouse class filter: it loads at boot for
every keyboard/mouse Windows enumerates, before anyone logs in, so a bad
build can leave input non-functional until you boot into Safe Mode
(third-party filters don't load there) or a recovery prompt to remove it.

> The driver publishes its device objects as `\\.\interception00` … `\\.\interception19`.
> That name is part of the wire protocol between this library and the driver, so
> [`src/interceptor.c`](src/interceptor.c) still opens `\\.\interception##`
> verbatim. It is deliberately *not* renamed and must stay that way.

Also note the usual filter-driver caveat: a process using this library only
receives input destined for processes at its own integrity level or lower, so to
intercept input to an elevated application you must run elevated yourself.

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
→ `interceptor` in those three casings. Semantics, struct layouts, filter bit
values, and the wire protocol to the driver match, so a program ported this
way behaves identically. `src/`, `driver/`, and `samples/` here are all
original implementations of that same public API and `\\.\interceptionNN`
wire protocol — see [License](#license) — not a derivative port of that
project's actual source. (The one sample with a distinct, separately-credited
origin is `caps2esc`, ported from a different, unrelated upstream project —
see its own table entry and file header.)

The one string that is **not** renamed from that API is the driver device
path `\\.\interception00`, since it is the wire-protocol contract with the
separately built/installed kernel driver in [`driver/`](driver/).

This repository also adds `src\build-msvc.cmd` and `src\interceptor-standalone.rc`
for building with a current Visual Studio toolchain without the WDK, and
`src\dllmain.c` as the entry point for that build.

## Layout

```
src/                         the library
  interceptor.h                public API
  interceptor.c                implementation (talks to the driver via DeviceIoControl)
  dllmain.c                    entry point for the standalone MSVC build
  interceptor.rc               version resource (WDK build)
  interceptor-standalone.rc    version resource (MSVC build)
  build-msvc.cmd               modern Visual Studio build
  buildit.cmd                  legacy WDK build, x86
  buildit-x64.cmd              legacy WDK build, x64
  sources, makefile            WDK build definitions
driver/                      the kernel driver (original implementation, no oblitum dependency)
  protocol.h                   IOCTL codes + filter bits, kept in lockstep with src/interceptor.c
  driver.h, driver.c           DriverEntry/Unload, the 20 permanent \\.\interceptionNN control devices
  pnp.c                        PnP AddDevice / slot binding for each physical keyboard or mouse
  connect.c                    CONNECT_DATA hook that captures real hardware strokes
  dispatch.c                   the 8 IOCTLs (SET/GET_PRECEDENCE, SET/GET_FILTER, SET_EVENT, READ, WRITE, GET_HARDWARE_ID)
  queue.c                      per-open stroke queues and precedence-ordered delivery
  interceptor-driver.inf       class-filter install (LowerFilters on the keyboard/mouse classes)
  interceptor-driver.vcxproj   modern WDK MSBuild driver project
  build-wdk.cmd                modern Visual Studio + WDK build
samples/                     example filters, one per directory
tools/scd.cmd                short-path cd helper used by the WDK build scripts
```

## Legal and safety

This library intercepts and synthesises input system-wide. Use it on machines
you control, for input remapping, accessibility, and automation. Note that
anti-cheat and endpoint-security products commonly flag or block input filter
drivers, and some online games treat them as a terms-of-service violation.

## License

MIT — see [LICENSE](LICENSE) — for the whole repository.

`src/`, `driver/`, and `samples/` are original implementations of the public
API and `\\.\interceptionNN` wire protocol popularized by
[Interception](https://github.com/oblitum/Interception), written without
reference to that project's actual source: `driver/` because that project's
kernel driver source has never been published under any license (it's
offered only as a compiled binary, with source access sold separately as a
commercial license); `src/` and `samples/` as clean-room rewrites — fresh
implementations written from functional specifications of the API, wire
protocol, and each sample's documented behavior only, by someone who had not
seen that project's actual (LGPL-3.0-licensed) library or sample source.
None of these are a derivative of that project's code. The one exception is
`samples/caps2esc`, whose *remapping concept* (not its code) is credited to
a separate, unrelated project, [caps2esc](https://github.com/alexandre/caps2esc)
by Alexandre — see that sample's own file header.
