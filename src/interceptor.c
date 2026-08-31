/*
 * interceptor.c -- original, from-scratch user-mode implementation of the
 * interceptor.h public API and its \\.\interceptionNN wire protocol.
 *
 * This file is a clean-room rewrite: it was produced only from
 *   - interceptor.h (the pre-existing, unchanged public API), and
 *   - driver/protocol.h, driver/driver.h, driver/dispatch.c (this
 *     repository's own kernel-mode side of the protocol, itself an
 *     original implementation),
 * plus a functional specification of the required behavior. The prior
 * version of this file (a renamed port of a third party's LGPL-licensed
 * library) was never consulted while writing this one -- see the License
 * section of the top-level README.
 *
 * No CRT dependency: only kernel32-exported Win32 APIs are used
 * (CreateFileW, CloseHandle, CreateEventW, DeviceIoControl,
 * WaitForMultipleObjects, HeapAlloc/HeapFree/GetProcessHeap). No malloc,
 * no <stdio.h>, no CRT string routines. Written in a C89-compatible style
 * (declarations at the top of blocks) so it also builds cleanly as C99/C++.
 */

/* build-msvc.cmd already passes /DINTERCEPTOR_EXPORT on the command line;
   only define it here for a build that doesn't (e.g. compiling this file
   directly some other way), so neither path redefines the other's macro. */
#if !defined(INTERCEPTOR_STATIC) && !defined(INTERCEPTOR_EXPORT)
#define INTERCEPTOR_EXPORT
#endif

#include "interceptor.h"

#include <windows.h>
#include <winioctl.h>

/* --------------------------------------------------------------------- *
 * Wire-protocol constants (mirrors driver/protocol.h's IOCTL codes and
 * the raw NT input-data ABI; computed locally per the spec rather than
 * assuming any shared header between user mode and kernel mode).
 * --------------------------------------------------------------------- */

#define IOCTL_INTERCEPTOR_SET_PRECEDENCE  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_INTERCEPTOR_GET_PRECEDENCE  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_INTERCEPTOR_SET_FILTER      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_INTERCEPTOR_GET_FILTER      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_INTERCEPTOR_SET_EVENT       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_INTERCEPTOR_WRITE           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_INTERCEPTOR_READ            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x840, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_INTERCEPTOR_GET_HARDWARE_ID CTL_CODE(FILE_DEVICE_UNKNOWN, 0x880, METHOD_BUFFERED, FILE_ANY_ACCESS)

/*
 * Raw on-the-wire stroke layouts. These reproduce the real NT
 * KEYBOARD_INPUT_DATA / MOUSE_INPUT_DATA ABI (Microsoft's structure
 * definitions, not creative expression) byte-for-byte, because the
 * driver reads WRITE input / fills READ output directly as these types.
 * <ntddkbd.h>'s real KEYBOARD_INPUT_DATA is wrapped in 1-byte struct
 * packing upstream (its four USHORT fields would otherwise leave the
 * trailing ULONG naturally aligned at a 12-byte total instead of the
 * true 10-byte wire size), so this local copy packs to 1 byte too;
 * MOUSE_INPUT_DATA happens to come out the same size either way.
 */
#pragma pack(push, 1)

typedef struct
{
    unsigned short UnitId;
    unsigned short MakeCode;
    unsigned short Flags;
    unsigned short Reserved;
    unsigned long  ExtraInformation;
} InterceptorRawKeyStroke; /* 10 bytes */

typedef struct
{
    unsigned short UnitId;
    unsigned short Flags;
    unsigned short ButtonFlags;
    unsigned short ButtonData;
    unsigned long  RawButtons;
    long           LastX;
    long           LastY;
    unsigned long  ExtraInformation;
} InterceptorRawMouseStroke; /* 24 bytes */

#pragma pack(pop)

/* Small batches (the common case: one or a handful of strokes per call)
   are staged on the stack to avoid a heap round-trip; anything larger
   spills to the process heap. */
#define INTERCEPTOR_STACK_STROKE_CAPACITY 32

/* --------------------------------------------------------------------- *
 * Opaque context.
 * --------------------------------------------------------------------- */

typedef struct
{
    HANDLE Device[INTERCEPTOR_MAX_DEVICE];
    HANDLE Event[INTERCEPTOR_MAX_DEVICE];
} InterceptorContextData;

/* --------------------------------------------------------------------- *
 * Small internal helpers.
 * --------------------------------------------------------------------- */

/* Fills path[] with L"\\.\interceptionNN" for index in [0,19], without
   any CRT/user32 formatting helper (no wsprintf -- that's user32, and
   this file links kernel32 only). */
static void InterceptorBuildDevicePath(WCHAR path[32], int index)
{
    static const WCHAR prefix[] = L"\\\\.\\interception";
    int i = 0;

    while (prefix[i] != 0)
    {
        path[i] = prefix[i];
        i++;
    }

    path[i]     = (WCHAR)(L'0' + (index / 10));
    path[i + 1] = (WCHAR)(L'0' + (index % 10));
    path[i + 2] = 0;
}

static int InterceptorHandleIsOpen(HANDLE handle)
{
    return handle != NULL && handle != INVALID_HANDLE_VALUE;
}

/* Returns the device handle for a validated context/device pair, or NULL
   if the context is NULL, the device id is out of range, or (defensively)
   the slot was never successfully opened. */
static HANDLE InterceptorGetDeviceHandle(const InterceptorContextData *ctx, InterceptorDevice device)
{
    HANDLE handle;

    if (ctx == NULL || interceptor_is_invalid(device))
        return NULL;

    handle = ctx->Device[device - 1];
    if (!InterceptorHandleIsOpen(handle))
        return NULL;

    return handle;
}

static void InterceptorDestroyContextData(InterceptorContextData *ctx)
{
    int i;

    if (ctx == NULL)
        return;

    for (i = 0; i < INTERCEPTOR_MAX_DEVICE; i++)
    {
        if (ctx->Event[i] != NULL)
            CloseHandle(ctx->Event[i]);

        if (InterceptorHandleIsOpen(ctx->Device[i]))
            CloseHandle(ctx->Device[i]);
    }

    HeapFree(GetProcessHeap(), 0, ctx);
}

/* --------------------------------------------------------------------- *
 * Stroke translation between the caller-facing InterceptorKeyStroke /
 * InterceptorMouseStroke shapes and the raw wire structs.
 *
 * Field mapping for mice follows the well-documented public semantics of
 * MOUSE_INPUT_DATA: ButtonFlags carries which buttons changed state (the
 * same bit values as InterceptorMouseStroke.state / InterceptorMouseState),
 * Flags carries movement mode (relative/absolute -- InterceptorMouseFlag),
 * ButtonData carries wheel/hwheel rotation amount as a signed 16-bit delta
 * (InterceptorMouseStroke.rolling), and LastX/LastY/ExtraInformation map
 * straight across. RawButtons has no counterpart in InterceptorMouseStroke
 * and UnitId has no counterpart in either stroke type, so both are zeroed
 * on send and ignored on receive.
 * --------------------------------------------------------------------- */

static void InterceptorKeyStrokesToRaw(const InterceptorStroke *strokes, unsigned int count, InterceptorRawKeyStroke *raw)
{
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        const InterceptorKeyStroke *stroke = (const InterceptorKeyStroke *)&strokes[i];

        raw[i].UnitId          = 0;
        raw[i].MakeCode        = stroke->code;
        raw[i].Flags            = stroke->state;
        raw[i].Reserved         = 0;
        raw[i].ExtraInformation = (unsigned long)stroke->information;
    }
}

static void InterceptorRawToKeyStrokes(const InterceptorRawKeyStroke *raw, unsigned int count, InterceptorStroke *strokes)
{
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        InterceptorKeyStroke *stroke = (InterceptorKeyStroke *)&strokes[i];

        stroke->code        = raw[i].MakeCode;
        stroke->state       = raw[i].Flags;
        stroke->information = (unsigned int)raw[i].ExtraInformation;
    }
}

static void InterceptorMouseStrokesToRaw(const InterceptorStroke *strokes, unsigned int count, InterceptorRawMouseStroke *raw)
{
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        const InterceptorMouseStroke *stroke = (const InterceptorMouseStroke *)&strokes[i];

        raw[i].UnitId          = 0;
        raw[i].Flags            = stroke->flags;
        raw[i].ButtonFlags      = stroke->state;
        raw[i].ButtonData       = (unsigned short)stroke->rolling;
        raw[i].RawButtons       = 0;
        raw[i].LastX            = (long)stroke->x;
        raw[i].LastY            = (long)stroke->y;
        raw[i].ExtraInformation = (unsigned long)stroke->information;
    }
}

static void InterceptorRawToMouseStrokes(const InterceptorRawMouseStroke *raw, unsigned int count, InterceptorStroke *strokes)
{
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        InterceptorMouseStroke *stroke = (InterceptorMouseStroke *)&strokes[i];

        stroke->state       = raw[i].ButtonFlags;
        stroke->flags        = raw[i].Flags;
        stroke->rolling      = (short)raw[i].ButtonData;
        stroke->x            = (int)raw[i].LastX;
        stroke->y            = (int)raw[i].LastY;
        stroke->information  = (unsigned int)raw[i].ExtraInformation;
    }
}

/* --------------------------------------------------------------------- *
 * Context lifecycle.
 * --------------------------------------------------------------------- */

InterceptorContext INTERCEPTOR_API interceptor_create_context(void)
{
    InterceptorContextData *ctx;
    int i;

    ctx = (InterceptorContextData *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(InterceptorContextData));
    if (ctx == NULL)
        return (InterceptorContext)0;

    for (i = 0; i < INTERCEPTOR_MAX_DEVICE; i++)
    {
        ctx->Device[i] = INVALID_HANDLE_VALUE;
        ctx->Event[i] = NULL;
    }

    for (i = 0; i < INTERCEPTOR_MAX_DEVICE; i++)
    {
        WCHAR path[32];
        HANDLE fileHandle;
        HANDLE eventHandle;
        HANDLE eventPair[2];
        DWORD bytesReturned;

        InterceptorBuildDevicePath(path, i);

        fileHandle = CreateFileW(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (fileHandle == INVALID_HANDLE_VALUE)
        {
            InterceptorDestroyContextData(ctx);
            return (InterceptorContext)0;
        }
        ctx->Device[i] = fileHandle;

        /* Manual-reset: the driver sets it when a stroke is queued and
           leaves it set until the caller drains the queue via READ, so a
           waiter that wakes late still observes the signaled state. */
        eventHandle = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (eventHandle == NULL)
        {
            InterceptorDestroyContextData(ctx);
            return (InterceptorContext)0;
        }
        ctx->Event[i] = eventHandle;

        eventPair[0] = eventHandle;
        eventPair[1] = NULL;
        bytesReturned = 0;
        if (!DeviceIoControl(fileHandle, IOCTL_INTERCEPTOR_SET_EVENT, eventPair, sizeof(eventPair), NULL, 0, &bytesReturned, NULL))
        {
            InterceptorDestroyContextData(ctx);
            return (InterceptorContext)0;
        }
    }

    return (InterceptorContext)ctx;
}

void INTERCEPTOR_API interceptor_destroy_context(InterceptorContext context)
{
    InterceptorDestroyContextData((InterceptorContextData *)context);
}

/* --------------------------------------------------------------------- *
 * Precedence / filter.
 * --------------------------------------------------------------------- */

InterceptorPrecedence INTERCEPTOR_API interceptor_get_precedence(InterceptorContext context, InterceptorDevice device)
{
    HANDLE handle;
    int value = 0;
    DWORD bytesReturned = 0;

    handle = InterceptorGetDeviceHandle((const InterceptorContextData *)context, device);
    if (handle == NULL)
        return 0;

    if (!DeviceIoControl(handle, IOCTL_INTERCEPTOR_GET_PRECEDENCE, NULL, 0, &value, sizeof(value), &bytesReturned, NULL))
        return 0;

    return (InterceptorPrecedence)value;
}

void INTERCEPTOR_API interceptor_set_precedence(InterceptorContext context, InterceptorDevice device, InterceptorPrecedence precedence)
{
    HANDLE handle;
    int value;
    DWORD bytesReturned = 0;

    handle = InterceptorGetDeviceHandle((const InterceptorContextData *)context, device);
    if (handle == NULL)
        return;

    value = (int)precedence;
    DeviceIoControl(handle, IOCTL_INTERCEPTOR_SET_PRECEDENCE, &value, sizeof(value), NULL, 0, &bytesReturned, NULL);
}

InterceptorFilter INTERCEPTOR_API interceptor_get_filter(InterceptorContext context, InterceptorDevice device)
{
    HANDLE handle;
    unsigned short value = 0;
    DWORD bytesReturned = 0;

    handle = InterceptorGetDeviceHandle((const InterceptorContextData *)context, device);
    if (handle == NULL)
        return 0;

    if (!DeviceIoControl(handle, IOCTL_INTERCEPTOR_GET_FILTER, NULL, 0, &value, sizeof(value), &bytesReturned, NULL))
        return 0;

    return (InterceptorFilter)value;
}

void INTERCEPTOR_API interceptor_set_filter(InterceptorContext context, InterceptorPredicate predicate, InterceptorFilter filter)
{
    InterceptorContextData *ctx = (InterceptorContextData *)context;
    InterceptorDevice device;
    unsigned short value = (unsigned short)filter;

    if (ctx == NULL || predicate == NULL)
        return;

    for (device = 1; device <= INTERCEPTOR_MAX_DEVICE; device++)
    {
        if (predicate(device))
        {
            HANDLE handle = InterceptorGetDeviceHandle(ctx, device);
            if (handle != NULL)
            {
                DWORD bytesReturned = 0;
                DeviceIoControl(handle, IOCTL_INTERCEPTOR_SET_FILTER, &value, sizeof(value), NULL, 0, &bytesReturned, NULL);
            }
        }
    }
}

/* --------------------------------------------------------------------- *
 * Waiting.
 * --------------------------------------------------------------------- */

InterceptorDevice INTERCEPTOR_API interceptor_wait(InterceptorContext context)
{
    return interceptor_wait_with_timeout(context, INFINITE);
}

InterceptorDevice INTERCEPTOR_API interceptor_wait_with_timeout(InterceptorContext context, unsigned long milliseconds)
{
    InterceptorContextData *ctx = (InterceptorContextData *)context;
    DWORD result;

    if (ctx == NULL)
        return 0;

    result = WaitForMultipleObjects((DWORD)INTERCEPTOR_MAX_DEVICE, ctx->Event, FALSE, (DWORD)milliseconds);

    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + (DWORD)INTERCEPTOR_MAX_DEVICE)
        return (InterceptorDevice)(result - WAIT_OBJECT_0 + 1);

    /* WAIT_TIMEOUT, WAIT_FAILED, or a WAIT_ABANDONED_0.. result (events
       can't be abandoned, but treat that range the same as any other
       non-signal outcome) all fall through here. */
    return 0;
}

/* --------------------------------------------------------------------- *
 * Send / receive.
 * --------------------------------------------------------------------- */

int INTERCEPTOR_API interceptor_send(InterceptorContext context, InterceptorDevice device, const InterceptorStroke *stroke, unsigned int nstroke)
{
    HANDLE handle;
    unsigned int elementSize;
    DWORD totalBytes;
    DWORD bytesReturned;
    BOOL ok;
    unsigned char stackBuffer[INTERCEPTOR_STACK_STROKE_CAPACITY * sizeof(InterceptorRawMouseStroke)];
    void *buffer;
    int heapUsed;

    if (stroke == NULL || nstroke == 0)
        return 0;

    handle = InterceptorGetDeviceHandle((const InterceptorContextData *)context, device);
    if (handle == NULL)
        return 0;

    elementSize = interceptor_is_keyboard(device)
        ? (unsigned int)sizeof(InterceptorRawKeyStroke)
        : (unsigned int)sizeof(InterceptorRawMouseStroke);

    /* Guard nstroke * elementSize against overflowing the DWORD byte
       count DeviceIoControl takes. */
    if (nstroke > (0xFFFFFFFFu / elementSize))
        return 0;

    totalBytes = (DWORD)(nstroke * elementSize);

    heapUsed = 0;
    if (nstroke <= INTERCEPTOR_STACK_STROKE_CAPACITY)
    {
        buffer = stackBuffer;
    }
    else
    {
        buffer = HeapAlloc(GetProcessHeap(), 0, totalBytes);
        if (buffer == NULL)
            return 0;
        heapUsed = 1;
    }

    if (interceptor_is_keyboard(device))
        InterceptorKeyStrokesToRaw(stroke, nstroke, (InterceptorRawKeyStroke *)buffer);
    else
        InterceptorMouseStrokesToRaw(stroke, nstroke, (InterceptorRawMouseStroke *)buffer);

    bytesReturned = 0;
    ok = DeviceIoControl(handle, IOCTL_INTERCEPTOR_WRITE, buffer, totalBytes, NULL, 0, &bytesReturned, NULL);

    if (heapUsed)
        HeapFree(GetProcessHeap(), 0, buffer);

    if (!ok)
        return 0;

    return (int)(bytesReturned / elementSize);
}

int INTERCEPTOR_API interceptor_receive(InterceptorContext context, InterceptorDevice device, InterceptorStroke *stroke, unsigned int nstroke)
{
    HANDLE handle;
    unsigned int elementSize;
    DWORD totalBytes;
    DWORD bytesReturned;
    BOOL ok;
    unsigned char stackBuffer[INTERCEPTOR_STACK_STROKE_CAPACITY * sizeof(InterceptorRawMouseStroke)];
    void *buffer;
    int heapUsed;
    unsigned int receivedCount;

    if (stroke == NULL || nstroke == 0)
        return 0;

    handle = InterceptorGetDeviceHandle((const InterceptorContextData *)context, device);
    if (handle == NULL)
        return 0;

    elementSize = interceptor_is_keyboard(device)
        ? (unsigned int)sizeof(InterceptorRawKeyStroke)
        : (unsigned int)sizeof(InterceptorRawMouseStroke);

    if (nstroke > (0xFFFFFFFFu / elementSize))
        return 0;

    totalBytes = (DWORD)(nstroke * elementSize);

    heapUsed = 0;
    if (nstroke <= INTERCEPTOR_STACK_STROKE_CAPACITY)
    {
        buffer = stackBuffer;
    }
    else
    {
        buffer = HeapAlloc(GetProcessHeap(), 0, totalBytes);
        if (buffer == NULL)
            return 0;
        heapUsed = 1;
    }

    bytesReturned = 0;
    ok = DeviceIoControl(handle, IOCTL_INTERCEPTOR_READ, NULL, 0, buffer, totalBytes, &bytesReturned, NULL);

    if (!ok)
    {
        if (heapUsed)
            HeapFree(GetProcessHeap(), 0, buffer);
        return 0;
    }

    receivedCount = bytesReturned / elementSize;

    if (interceptor_is_keyboard(device))
        InterceptorRawToKeyStrokes((const InterceptorRawKeyStroke *)buffer, receivedCount, stroke);
    else
        InterceptorRawToMouseStrokes((const InterceptorRawMouseStroke *)buffer, receivedCount, stroke);

    if (heapUsed)
        HeapFree(GetProcessHeap(), 0, buffer);

    return (int)receivedCount;
}

/* --------------------------------------------------------------------- *
 * Hardware id.
 * --------------------------------------------------------------------- */

unsigned int INTERCEPTOR_API interceptor_get_hardware_id(InterceptorContext context, InterceptorDevice device, void *hardware_id_buffer, unsigned int buffer_size)
{
    HANDLE handle;
    DWORD bytesReturned = 0;

    handle = InterceptorGetDeviceHandle((const InterceptorContextData *)context, device);
    if (handle == NULL)
        return 0;

    if (!DeviceIoControl(handle, IOCTL_INTERCEPTOR_GET_HARDWARE_ID, NULL, 0, hardware_id_buffer, (DWORD)buffer_size, &bytesReturned, NULL))
        return 0;

    return (unsigned int)bytesReturned;
}

/* --------------------------------------------------------------------- *
 * Device id classification.
 * --------------------------------------------------------------------- */

int INTERCEPTOR_API interceptor_is_invalid(InterceptorDevice device)
{
    return (device < INTERCEPTOR_KEYBOARD(0) || device > INTERCEPTOR_MOUSE(INTERCEPTOR_MAX_MOUSE - 1)) ? 1 : 0;
}

int INTERCEPTOR_API interceptor_is_keyboard(InterceptorDevice device)
{
    return (device >= INTERCEPTOR_KEYBOARD(0) && device <= INTERCEPTOR_KEYBOARD(INTERCEPTOR_MAX_KEYBOARD - 1)) ? 1 : 0;
}

int INTERCEPTOR_API interceptor_is_mouse(InterceptorDevice device)
{
    return (device >= INTERCEPTOR_MOUSE(0) && device <= INTERCEPTOR_MOUSE(INTERCEPTOR_MAX_MOUSE - 1)) ? 1 : 0;
}
