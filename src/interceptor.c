/*
 * interceptor.c -- user-mode-only implementation of the interceptor.h public
 * API. No kernel driver, no signing, no test-signing mode, no reboot.
 *
 * Architecture
 * ------------
 * Three system-wide Win32 mechanisms stand in for the three things the
 * kernel class filter driver (see the `driver` branch) used to give us:
 *
 *   - Raw Input (RegisterRawInputDevices/WM_INPUT) tells us WHICH physical
 *     keyboard or mouse produced an event (HANDLE per device) and hands us
 *     RAWKEYBOARD/RAWMOUSE payloads that happen to share field-for-field
 *     semantics with the NT KEYBOARD_INPUT_DATA/MOUSE_INPUT_DATA structs
 *     the driver spoke -- see InterceptorRawKeyStroke/InterceptorRawMouseStroke
 *     below. Raw Input is monitor-only: it cannot withhold an event from
 *     the rest of the system.
 *   - WH_KEYBOARD_LL/WH_MOUSE_LL low-level hooks are the only user-mode
 *     mechanism that CAN withhold an event (return 1 from the hook to
 *     swallow it). But a hook callback carries no per-device identity.
 *   - SendInput() re-injects a stroke a caller releases via
 *     interceptor_send(), the user-mode analog of the driver's WRITE IOCTL.
 *
 * Correlating a hook call with the raw-input event that produced it is done
 * with a small FIFO per event kind (g_RealKeyboardPending/g_RealMousePending):
 * a genuine hardware event's WM_INPUT is (empirically, not documentedly)
 * queued to our worker thread before the corresponding low-level hook
 * fires, both being dispatched through that same thread's message pump, so
 * "pop the oldest still-unmatched raw-input entry when the hook fires"
 * reliably pairs them up. If that assumption is ever violated for a given
 * event the FIFO is empty at hook time and we fail OPEN -- pass the event
 * through unfiltered -- rather than risk misattributing or losing it.
 * interceptor_send()'s own SendInput calls are correlated the same way
 * through a second pair of FIFOs (g_InjectedKeyboardPending/…Mouse…), which
 * also carry which Open resumed the chain, mirroring the driver's
 * IOCTL_WRITE "resume delivery from the next open after this one" rule (see
 * InterceptorDeliverStroke).
 *
 * Known gaps versus the kernel driver (see README's "Trade-offs" section):
 *   - A stroke released via interceptor_send() carries SendInput's
 *     LLKHF_INJECTED/LLMHF_INJECTED flag forever; nothing in user mode can
 *     clear it, so anything downstream that checks that flag (some
 *     anti-cheat, some other input tools) can tell it apart from real
 *     hardware. The driver's reinjected strokes carry no such marker.
 *   - Exclusive-mode DirectInput/raw-HID-only games, the secure desktop
 *     (UAC consent, Ctrl+Alt+Del), and the console can bypass both Raw
 *     Input and low-level hooks entirely.
 *   - INTERCEPTOR_FILTER_KEY_TERMSRV_* bits never match locally -- those
 *     only arise inside an actual Remote Desktop session's keyboard stack,
 *     which neither Raw Input nor WH_KEYBOARD_LL surface.
 *   - Precedence now only orders opens within this process; the driver
 *     could arbitrate multiple simultaneous processes system-wide, but
 *     nothing in user mode can override the order Windows itself calls
 *     different processes' low-level hooks in.
 *   - A slot is assigned to a raw-input device HANDLE for the life of the
 *     engine and is never freed, so unplugging and replugging a device
 *     consumes a new slot rather than rebinding to its old one (the driver
 *     binds by persistent PnP instance ID, which survives replug).
 *
 * No CRT dependency: only kernel32/user32-exported Win32 APIs are used. No
 * malloc, no <stdio.h>, no CRT string routines.
 */

#if !defined(INTERCEPTOR_STATIC) && !defined(INTERCEPTOR_EXPORT)
#define INTERCEPTOR_EXPORT
#endif

#include "interceptor.h"

#include <windows.h>

/* No CRT is linked (/NODEFAULTLIB), but under optimization MSVC lowers
   CopyMemory/ZeroMemory (RtlCopyMemory/RtlZeroMemory, themselves #defined
   to memcpy/memset in <winnt.h> for user-mode code) and even plain manual
   copy loops into calls to the memcpy/memset symbols. Providing them
   ourselves is simpler than avoiding every pattern the optimizer might
   recognize; #pragma function stops the compiler from treating the names
   specially (which would otherwise fight our own definitions below). */
#pragma function(memcpy, memset)

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)value;
    return dst;
}

/* --------------------------------------------------------------------- *
 * Raw wire-format strokes shared by every path below: Raw Input fills
 * them, the hook engine matches/queues them, interceptor_send() builds
 * SendInput() calls from them, and interceptor_receive() translates them
 * back to the caller-facing InterceptorKeyStroke/InterceptorMouseStroke
 * shapes. Field layout mirrors NT's KEYBOARD_INPUT_DATA/MOUSE_INPUT_DATA
 * (and, not by coincidence, RAWKEYBOARD/RAWMOUSE -- both are views onto the
 * same underlying NT input abstraction).
 * --------------------------------------------------------------------- */

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

#define INTERCEPTOR_STACK_STROKE_CAPACITY 32

/* Per-open queue cap, matching the driver's own (undocumented upstream)
   choice: absorb a slow reader for a while without unbounded growth. */
#define INTERCEPTOR_MAX_QUEUED_STROKES 4096

/* --------------------------------------------------------------------- *
 * Stroke translation between the caller-facing shapes and the raw wire
 * structs. Unchanged from the driver-backed implementation.
 * --------------------------------------------------------------------- */

static void InterceptorKeyStrokesToRaw(const InterceptorStroke *strokes, unsigned int count, InterceptorRawKeyStroke *raw)
{
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        const InterceptorKeyStroke *stroke = (const InterceptorKeyStroke *)&strokes[i];

        raw[i].UnitId          = 0;
        raw[i].MakeCode        = stroke->code;
        raw[i].Flags           = stroke->state;
        raw[i].Reserved        = 0;
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
        raw[i].Flags           = stroke->flags;
        raw[i].ButtonFlags     = stroke->state;
        raw[i].ButtonData      = (unsigned short)stroke->rolling;
        raw[i].RawButtons      = 0;
        raw[i].LastX           = (long)stroke->x;
        raw[i].LastY           = (long)stroke->y;
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
        stroke->flags       = raw[i].Flags;
        stroke->rolling     = (short)raw[i].ButtonData;
        stroke->x           = (int)raw[i].LastX;
        stroke->y           = (int)raw[i].LastY;
        stroke->information = (unsigned int)raw[i].ExtraInformation;
    }
}

/* --------------------------------------------------------------------- *
 * Per-open queues and the precedence-ordered slot chain. One INTERCEPTOR_OPEN
 * per (context, device) pair, same as the driver's per-\\.\interceptionNN
 * handle open -- interceptor_create_context() still implicitly "opens" all
 * 20 identities.
 * --------------------------------------------------------------------- */

typedef struct INTERCEPTOR_STROKE_NODE
{
    struct INTERCEPTOR_STROKE_NODE *Next;
    unsigned char Data[sizeof(InterceptorRawMouseStroke)];
} INTERCEPTOR_STROKE_NODE;

typedef struct INTERCEPTOR_SLOT INTERCEPTOR_SLOT;

typedef struct INTERCEPTOR_OPEN
{
    struct INTERCEPTOR_OPEN *Next;   /* slot's OpenListHead, ascending precedence */
    INTERCEPTOR_SLOT *Slot;
    LONG Precedence;
    unsigned short Filter;
    unsigned short MonitorFilter;    /* interceptor_set_monitor -- see interceptor.h */
    HANDLE Event;
    ULONG OpenSequence;
    INTERCEPTOR_STROKE_NODE *QueueHead;
    INTERCEPTOR_STROKE_NODE *QueueTail;
    ULONG QueuedCount;
} INTERCEPTOR_OPEN;

struct INTERCEPTOR_SLOT
{
    BOOL IsMouse;
    HANDLE RawDeviceHandle;          /* NULL until a physical device is seen */
    WCHAR HardwareId[256];
    USHORT HardwareIdLength;         /* bytes, including terminator */
    INTERCEPTOR_OPEN *OpenListHead;  /* ascending precedence, OpenSequence tiebreak */
    ULONG NextOpenSequence;
    CRITICAL_SECTION Lock;           /* guards OpenListHead + every Open hung off it */
};

static INTERCEPTOR_SLOT g_KeyboardSlot[INTERCEPTOR_MAX_KEYBOARD];
static INTERCEPTOR_SLOT g_MouseSlot[INTERCEPTOR_MAX_MOUSE];
static CRITICAL_SECTION g_SlotTableLock; /* guards RawDeviceHandle/HardwareId assignment */

static void InterceptorInsertOpenSorted_Locked(INTERCEPTOR_SLOT *slot, INTERCEPTOR_OPEN *open)
{
    INTERCEPTOR_OPEN **link = &slot->OpenListHead;

    while (*link != NULL)
    {
        INTERCEPTOR_OPEN *existing = *link;

        if (existing->Precedence > open->Precedence ||
            (existing->Precedence == open->Precedence && existing->OpenSequence > open->OpenSequence))
            break;

        link = &existing->Next;
    }

    open->Next = *link;
    *link = open;
}

static void InterceptorUnlinkOpen_Locked(INTERCEPTOR_SLOT *slot, INTERCEPTOR_OPEN *open)
{
    INTERCEPTOR_OPEN **link = &slot->OpenListHead;

    while (*link != NULL)
    {
        if (*link == open)
        {
            *link = open->Next;
            open->Next = NULL;
            return;
        }
        link = &(*link)->Next;
    }
}

/* --------------------------------------------------------------------- *
 * Filter matching -- ported from the driver's queue.c, operating on the
 * raw wire structs instead of KEYBOARD_INPUT_DATA/MOUSE_INPUT_DATA (their
 * Flags/ButtonFlags bit values are identical).
 * --------------------------------------------------------------------- */

static BOOL InterceptorKeyboardMatches(const InterceptorRawKeyStroke *stroke, unsigned short filter)
{
    unsigned short flags = stroke->Flags;

    if ((flags & INTERCEPTOR_KEY_UP) == 0)
    {
        if (filter & INTERCEPTOR_FILTER_KEY_DOWN) return TRUE;
    }
    else
    {
        if (filter & INTERCEPTOR_FILTER_KEY_UP) return TRUE;
    }

    if ((flags & INTERCEPTOR_KEY_E0) && (filter & INTERCEPTOR_FILTER_KEY_E0)) return TRUE;
    if ((flags & INTERCEPTOR_KEY_E1) && (filter & INTERCEPTOR_FILTER_KEY_E1)) return TRUE;
    /* TERMSRV_* bits never appear locally -- see the file header. */

    return FALSE;
}

static BOOL InterceptorMouseMatches(const InterceptorRawMouseStroke *stroke, unsigned short filter)
{
    unsigned short buttonFlags = stroke->ButtonFlags;

    if (buttonFlags == 0)
        return (filter & INTERCEPTOR_FILTER_MOUSE_MOVE) != 0;

    return (filter & buttonFlags) != 0;
}

/*
 * Walks Slot's open chain starting just after StartAfter (or from the head
 * if NULL, i.e. a genuine-hardware delivery), queues the stroke into the
 * first open whose filter matches, and returns whether it was captured.
 * Mirrors driver/queue.c's InterceptorDeliverGeneric chain-of-responsibility,
 * minus the "forward to real hardware" tail -- here the caller (the hook
 * proc) does that itself via CallNextHookEx when this returns FALSE.
 *
 * Every open in the slot -- not just the ones the chain walk reaches -- is
 * separately offered the stroke against its MonitorFilter afterward, win or
 * lose; a monitor match never affects `captured` or the chain. See
 * interceptor_set_monitor in interceptor.h.
 */

/* Caller holds slot->Lock. Shared by the capturing chain walk and the
   monitor pass below -- queueing a stroke means the same thing either way,
   just reached for a different reason. */
static void InterceptorEnqueueStroke_Locked(INTERCEPTOR_OPEN *open, const unsigned char *rawData, size_t strideBytes)
{
    if (open->QueuedCount >= INTERCEPTOR_MAX_QUEUED_STROKES)
        return; /* dropped rather than let it fall through out of order (captures) or grow unbounded (monitors) */

    {
        INTERCEPTOR_STROKE_NODE *node =
            (INTERCEPTOR_STROKE_NODE *)HeapAlloc(GetProcessHeap(), 0, sizeof(INTERCEPTOR_STROKE_NODE));

        if (node == NULL)
            return;

        CopyMemory(node->Data, rawData, strideBytes);
        node->Next = NULL;

        if (open->QueueTail != NULL) open->QueueTail->Next = node;
        else open->QueueHead = node;
        open->QueueTail = node;
        open->QueuedCount++;

        if (open->QueuedCount == 1)
            SetEvent(open->Event);
    }
}

static void InterceptorDeliverMonitors_Locked(INTERCEPTOR_SLOT *slot, const unsigned char *rawData, size_t strideBytes)
{
    INTERCEPTOR_OPEN *candidate;

    for (candidate = slot->OpenListHead; candidate != NULL; candidate = candidate->Next)
    {
        BOOL matches;

        if (candidate->MonitorFilter == 0)
            continue;

        matches = slot->IsMouse
            ? InterceptorMouseMatches((const InterceptorRawMouseStroke *)rawData, candidate->MonitorFilter)
            : InterceptorKeyboardMatches((const InterceptorRawKeyStroke *)rawData, candidate->MonitorFilter);

        if (matches)
            InterceptorEnqueueStroke_Locked(candidate, rawData, strideBytes);
    }
}

static BOOL InterceptorDeliverStroke(INTERCEPTOR_SLOT *slot, INTERCEPTOR_OPEN *startAfter, const unsigned char *rawData)
{
    INTERCEPTOR_OPEN *candidate;
    BOOL started = (startAfter == NULL);
    BOOL captured = FALSE;
    size_t strideBytes = slot->IsMouse ? sizeof(InterceptorRawMouseStroke) : sizeof(InterceptorRawKeyStroke);

    EnterCriticalSection(&slot->Lock);

    for (candidate = slot->OpenListHead; candidate != NULL && !captured; candidate = candidate->Next)
    {
        BOOL matches;

        if (!started)
        {
            if (candidate == startAfter) started = TRUE;
            continue;
        }

        matches = slot->IsMouse
            ? InterceptorMouseMatches((const InterceptorRawMouseStroke *)rawData, candidate->Filter)
            : InterceptorKeyboardMatches((const InterceptorRawKeyStroke *)rawData, candidate->Filter);

        if (matches)
        {
            InterceptorEnqueueStroke_Locked(candidate, rawData, strideBytes);
            captured = TRUE;
        }
    }

    InterceptorDeliverMonitors_Locked(slot, rawData, strideBytes);

    LeaveCriticalSection(&slot->Lock);
    return captured;
}

/* --------------------------------------------------------------------- *
 * Correlation FIFOs: pair a low-level hook call with the event that
 * produced it (see the file header). Real* is filled by WM_INPUT and
 * drained by non-injected hook calls; Injected* is filled by
 * interceptor_send() and drained by injected hook calls.
 * --------------------------------------------------------------------- */

#define INTERCEPTOR_PENDING_CAPACITY 512

typedef struct
{
    INTERCEPTOR_SLOT *Slot;
    INTERCEPTOR_OPEN *StartAfter;    /* NULL for a real-hardware entry */
    unsigned char Data[sizeof(InterceptorRawMouseStroke)];
} INTERCEPTOR_PENDING_ENTRY;

typedef struct
{
    INTERCEPTOR_PENDING_ENTRY Entries[INTERCEPTOR_PENDING_CAPACITY];
    int Head;
    int Count;
    CRITICAL_SECTION Lock;
} INTERCEPTOR_PENDING_QUEUE;

static INTERCEPTOR_PENDING_QUEUE g_RealKeyboardPending;
static INTERCEPTOR_PENDING_QUEUE g_RealMousePending;
static INTERCEPTOR_PENDING_QUEUE g_InjectedKeyboardPending;
static INTERCEPTOR_PENDING_QUEUE g_InjectedMousePending;

static void InterceptorPendingPush(INTERCEPTOR_PENDING_QUEUE *queue, const INTERCEPTOR_PENDING_ENTRY *entry)
{
    EnterCriticalSection(&queue->Lock);
    if (queue->Count < INTERCEPTOR_PENDING_CAPACITY)
    {
        int tail = (queue->Head + queue->Count) % INTERCEPTOR_PENDING_CAPACITY;
        queue->Entries[tail] = *entry;
        queue->Count++;
    }
    /* Full (a pathological burst): drop. The hook call this would have fed
       fails open (see InterceptorKeyboardHookProc/MouseHookProc) instead of
       stalling or misattributing. */
    LeaveCriticalSection(&queue->Lock);
}

static BOOL InterceptorPendingPop(INTERCEPTOR_PENDING_QUEUE *queue, INTERCEPTOR_PENDING_ENTRY *out)
{
    BOOL got = FALSE;

    EnterCriticalSection(&queue->Lock);
    if (queue->Count > 0)
    {
        *out = queue->Entries[queue->Head];
        queue->Head = (queue->Head + 1) % INTERCEPTOR_PENDING_CAPACITY;
        queue->Count--;
        got = TRUE;
    }
    LeaveCriticalSection(&queue->Lock);

    return got;
}

/* --------------------------------------------------------------------- *
 * Device identity: map a Raw Input device HANDLE to one of the 10
 * keyboard/10 mouse slots, first-seen order, permanently for the life of
 * the engine (see the file header's replug caveat).
 * --------------------------------------------------------------------- */

static INTERCEPTOR_SLOT *InterceptorResolveSlot(BOOL isMouse, HANDLE hDevice)
{
    INTERCEPTOR_SLOT *table = isMouse ? g_MouseSlot : g_KeyboardSlot;
    int count = isMouse ? INTERCEPTOR_MAX_MOUSE : INTERCEPTOR_MAX_KEYBOARD;
    int i;
    INTERCEPTOR_SLOT *free_slot = NULL;
    INTERCEPTOR_SLOT *result = NULL;

    EnterCriticalSection(&g_SlotTableLock);

    for (i = 0; i < count; i++)
    {
        if (table[i].RawDeviceHandle == hDevice) { result = &table[i]; break; }
        if (free_slot == NULL && table[i].RawDeviceHandle == NULL) free_slot = &table[i];
    }

    if (result == NULL && free_slot != NULL)
    {
        UINT charCount = 0;

        free_slot->RawDeviceHandle = hDevice;
        free_slot->HardwareIdLength = 0;

        if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, NULL, &charCount) == 0 &&
            charCount > 0 && charCount < (sizeof(free_slot->HardwareId) / sizeof(WCHAR)))
        {
            UINT got = GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, free_slot->HardwareId, &charCount);

            if (got != (UINT)-1)
            {
                free_slot->HardwareId[(sizeof(free_slot->HardwareId) / sizeof(WCHAR)) - 1] = 0;
                free_slot->HardwareIdLength =
                    (USHORT)((lstrlenW(free_slot->HardwareId) + 1) * sizeof(WCHAR));
            }
        }

        result = free_slot;
    }

    LeaveCriticalSection(&g_SlotTableLock);
    return result;
}

/*
 * Pre-populates slots for every keyboard/mouse already attached when the
 * engine starts, via GetRawInputDeviceList -- the proactive counterpart to
 * InterceptorResolveSlot's otherwise-reactive, WM_INPUT-triggered
 * assignment. Without this, a caller that enumerates devices (e.g. to list
 * them for a picker) immediately after interceptor_create_context() would
 * see nothing until each one produced a real keystroke or mouse move,
 * unlike the driver, which enumerated via PnP at attach time. A device that
 * arrives after this runs is still picked up reactively, same as before.
 */
static void InterceptorEnumerateExistingDevices(void)
{
    UINT count = 0;
    RAWINPUTDEVICELIST *list;

    if (GetRawInputDeviceList(NULL, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
        return;

    list = (RAWINPUTDEVICELIST *)HeapAlloc(GetProcessHeap(), 0, (size_t)count * sizeof(RAWINPUTDEVICELIST));
    if (list == NULL) return;

    if (GetRawInputDeviceList(list, &count, sizeof(RAWINPUTDEVICELIST)) != (UINT)-1)
    {
        UINT i;
        for (i = 0; i < count; i++)
        {
            if (list[i].dwType == RIM_TYPEKEYBOARD)
                InterceptorResolveSlot(FALSE, list[i].hDevice);
            else if (list[i].dwType == RIM_TYPEMOUSE)
                InterceptorResolveSlot(TRUE, list[i].hDevice);
        }
    }

    HeapFree(GetProcessHeap(), 0, list);
}

/* --------------------------------------------------------------------- *
 * The engine: one worker thread (started on the first context, stopped
 * after the last) owning a message-only window, the two Raw Input
 * registrations, and the two low-level hooks.
 * --------------------------------------------------------------------- */

static CRITICAL_SECTION g_EngineLock;
static LONG g_EngineRefCount = 0;
static HANDLE g_EngineThread = NULL;
static DWORD g_EngineThreadId = 0;
static HWND g_MessageWindow = NULL;
static HHOOK g_KeyboardHook = NULL;
static HHOOK g_MouseHook = NULL;
static HANDLE g_EngineReadyEvent = NULL;
static BOOL g_EngineStartFailed = FALSE;

static const WCHAR INTERCEPTOR_WNDCLASS[] = L"InterceptorEngineWindow";

static void InterceptorHandleRawInput(HRAWINPUT hRawInput)
{
    BYTE stackBuffer[256];
    BYTE *buffer = stackBuffer;
    UINT size = sizeof(stackBuffer);
    BOOL heapUsed = FALSE;
    RAWINPUT *raw;

    if (GetRawInputData(hRawInput, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1 || size == 0)
        return;

    if (size > sizeof(stackBuffer))
    {
        buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size);
        if (buffer == NULL) return;
        heapUsed = TRUE;
    }

    if (GetRawInputData(hRawInput, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) != size)
    {
        if (heapUsed) HeapFree(GetProcessHeap(), 0, buffer);
        return;
    }

    raw = (RAWINPUT *)buffer;

    if (raw->header.dwType == RIM_TYPEKEYBOARD && raw->header.hDevice != NULL)
    {
        INTERCEPTOR_SLOT *slot = InterceptorResolveSlot(FALSE, raw->header.hDevice);

        if (slot != NULL)
        {
            INTERCEPTOR_PENDING_ENTRY entry;
            InterceptorRawKeyStroke *keyRaw = (InterceptorRawKeyStroke *)entry.Data;

            entry.Slot = slot;
            entry.StartAfter = NULL;
            keyRaw->UnitId = 0;
            keyRaw->MakeCode = raw->data.keyboard.MakeCode;
            keyRaw->Flags = raw->data.keyboard.Flags;
            keyRaw->Reserved = 0;
            keyRaw->ExtraInformation = raw->data.keyboard.ExtraInformation;

            InterceptorPendingPush(&g_RealKeyboardPending, &entry);
        }
    }
    else if (raw->header.dwType == RIM_TYPEMOUSE && raw->header.hDevice != NULL)
    {
        INTERCEPTOR_SLOT *slot = InterceptorResolveSlot(TRUE, raw->header.hDevice);

        if (slot != NULL)
        {
            INTERCEPTOR_PENDING_ENTRY entry;
            InterceptorRawMouseStroke *mouseRaw = (InterceptorRawMouseStroke *)entry.Data;

            entry.Slot = slot;
            entry.StartAfter = NULL;
            mouseRaw->UnitId = 0;
            mouseRaw->Flags = raw->data.mouse.usFlags;
            mouseRaw->ButtonFlags = raw->data.mouse.usButtonFlags;
            mouseRaw->ButtonData = raw->data.mouse.usButtonData;
            mouseRaw->RawButtons = raw->data.mouse.ulRawButtons;
            mouseRaw->LastX = raw->data.mouse.lLastX;
            mouseRaw->LastY = raw->data.mouse.lLastY;
            mouseRaw->ExtraInformation = raw->data.mouse.ulExtraInformation;

            InterceptorPendingPush(&g_RealMousePending, &entry);
        }
    }

    if (heapUsed) HeapFree(GetProcessHeap(), 0, buffer);
}

static LRESULT CALLBACK InterceptorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INPUT:
        InterceptorHandleRawInput((HRAWINPUT)lParam);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static LRESULT CALLBACK InterceptorKeyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT *info = (const KBDLLHOOKSTRUCT *)lParam;
        BOOL injected = (info->flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)) != 0;
        INTERCEPTOR_PENDING_QUEUE *queue = injected ? &g_InjectedKeyboardPending : &g_RealKeyboardPending;
        INTERCEPTOR_PENDING_ENTRY entry;

        UNREFERENCED_PARAMETER(wParam);

        if (InterceptorPendingPop(queue, &entry) && InterceptorDeliverStroke(entry.Slot, entry.StartAfter, entry.Data))
            return 1;
    }

    return CallNextHookEx(NULL, code, wParam, lParam);
}

static LRESULT CALLBACK InterceptorMouseHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        const MSLLHOOKSTRUCT *info = (const MSLLHOOKSTRUCT *)lParam;
        BOOL injected = (info->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) != 0;
        INTERCEPTOR_PENDING_QUEUE *queue = injected ? &g_InjectedMousePending : &g_RealMousePending;
        INTERCEPTOR_PENDING_ENTRY entry;

        UNREFERENCED_PARAMETER(wParam);

        if (InterceptorPendingPop(queue, &entry) && InterceptorDeliverStroke(entry.Slot, entry.StartAfter, entry.Data))
            return 1;
    }

    return CallNextHookEx(NULL, code, wParam, lParam);
}

static DWORD WINAPI InterceptorEngineThreadProc(LPVOID param)
{
    WNDCLASSEXW wc;
    RAWINPUTDEVICE rid[2];
    MSG msg;
    BOOL ok = TRUE;
    ATOM classAtom;

    UNREFERENCED_PARAMETER(param);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = InterceptorWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = INTERCEPTOR_WNDCLASS;
    classAtom = RegisterClassExW(&wc);
    if (classAtom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { ok = FALSE; goto ready; }

    g_MessageWindow = CreateWindowExW(0, INTERCEPTOR_WNDCLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (g_MessageWindow == NULL) { ok = FALSE; goto ready; }

    rid[0].usUsagePage = 1; rid[0].usUsage = 6; /* generic desktop / keyboard */
    rid[0].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = g_MessageWindow;
    rid[1].usUsagePage = 1; rid[1].usUsage = 2; /* generic desktop / mouse */
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[1].hwndTarget = g_MessageWindow;

    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) { ok = FALSE; goto ready; }

    InterceptorEnumerateExistingDevices();

    g_KeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, InterceptorKeyboardHookProc, GetModuleHandleW(NULL), 0);
    g_MouseHook = SetWindowsHookExW(WH_MOUSE_LL, InterceptorMouseHookProc, GetModuleHandleW(NULL), 0);
    if (g_KeyboardHook == NULL || g_MouseHook == NULL) ok = FALSE;

ready:
    g_EngineStartFailed = !ok;
    SetEvent(g_EngineReadyEvent);
    if (!ok) goto cleanup;

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

cleanup:
    if (g_KeyboardHook != NULL) { UnhookWindowsHookEx(g_KeyboardHook); g_KeyboardHook = NULL; }
    if (g_MouseHook != NULL) { UnhookWindowsHookEx(g_MouseHook); g_MouseHook = NULL; }

    if (g_MessageWindow != NULL)
    {
        rid[0].dwFlags = RIDEV_REMOVE; rid[0].hwndTarget = NULL;
        rid[1].dwFlags = RIDEV_REMOVE; rid[1].hwndTarget = NULL;
        RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));

        DestroyWindow(g_MessageWindow);
        g_MessageWindow = NULL;
    }

    UnregisterClassW(INTERCEPTOR_WNDCLASS, wc.hInstance);
    return ok ? 0 : 1;
}

/* Both must be called with g_EngineLock held. */

static BOOL InterceptorEngineStart(void)
{
    if (g_EngineRefCount > 0) { g_EngineRefCount++; return TRUE; }

    g_EngineReadyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_EngineReadyEvent == NULL) return FALSE;

    g_EngineStartFailed = FALSE;
    g_EngineThread = CreateThread(NULL, 0, InterceptorEngineThreadProc, NULL, 0, &g_EngineThreadId);
    if (g_EngineThread == NULL)
    {
        CloseHandle(g_EngineReadyEvent);
        g_EngineReadyEvent = NULL;
        return FALSE;
    }

    WaitForSingleObject(g_EngineReadyEvent, INFINITE);
    CloseHandle(g_EngineReadyEvent);
    g_EngineReadyEvent = NULL;

    if (g_EngineStartFailed)
    {
        WaitForSingleObject(g_EngineThread, INFINITE);
        CloseHandle(g_EngineThread);
        g_EngineThread = NULL;
        g_EngineThreadId = 0;
        return FALSE;
    }

    g_EngineRefCount = 1;
    return TRUE;
}

static void InterceptorEngineStop(void)
{
    if (g_EngineRefCount == 0) return;

    g_EngineRefCount--;
    if (g_EngineRefCount > 0) return;

    PostThreadMessageW(g_EngineThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_EngineThread, INFINITE);
    CloseHandle(g_EngineThread);
    g_EngineThread = NULL;
    g_EngineThreadId = 0;
}

/* --------------------------------------------------------------------- *
 * One-time global init. Deliberately not done from DllMain: interceptor.c
 * can also be compiled directly into a program (INTERCEPTOR_STATIC) or
 * built through the legacy WDK path, neither of which runs dllmain.c.
 * --------------------------------------------------------------------- */

static INIT_ONCE g_InitOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK InterceptorGlobalInit(PINIT_ONCE initOnce, PVOID param, PVOID *context)
{
    int i;

    UNREFERENCED_PARAMETER(initOnce);
    UNREFERENCED_PARAMETER(param);
    UNREFERENCED_PARAMETER(context);

    InitializeCriticalSection(&g_EngineLock);
    InitializeCriticalSection(&g_SlotTableLock);
    InitializeCriticalSection(&g_RealKeyboardPending.Lock);
    InitializeCriticalSection(&g_RealMousePending.Lock);
    InitializeCriticalSection(&g_InjectedKeyboardPending.Lock);
    InitializeCriticalSection(&g_InjectedMousePending.Lock);

    for (i = 0; i < INTERCEPTOR_MAX_KEYBOARD; i++)
    {
        InitializeCriticalSection(&g_KeyboardSlot[i].Lock);
        g_KeyboardSlot[i].IsMouse = FALSE;
    }
    for (i = 0; i < INTERCEPTOR_MAX_MOUSE; i++)
    {
        InitializeCriticalSection(&g_MouseSlot[i].Lock);
        g_MouseSlot[i].IsMouse = TRUE;
    }

    return TRUE;
}

static void InterceptorEnsureGlobalInit(void)
{
    InitOnceExecuteOnce(&g_InitOnce, InterceptorGlobalInit, NULL, NULL);
}

static INTERCEPTOR_SLOT *InterceptorSlotForDevice(InterceptorDevice device)
{
    if (interceptor_is_keyboard(device))
        return &g_KeyboardSlot[device - INTERCEPTOR_KEYBOARD(0)];
    return &g_MouseSlot[device - INTERCEPTOR_MOUSE(0)];
}

/* --------------------------------------------------------------------- *
 * Opaque context.
 * --------------------------------------------------------------------- */

typedef struct
{
    INTERCEPTOR_OPEN *Open[INTERCEPTOR_MAX_DEVICE];
    HANDLE Event[INTERCEPTOR_MAX_DEVICE]; /* same handles as Open[i]->Event */
} InterceptorContextData;

static void InterceptorDestroyContextData(InterceptorContextData *ctx)
{
    int i;

    if (ctx == NULL) return;

    for (i = 0; i < INTERCEPTOR_MAX_DEVICE; i++)
    {
        INTERCEPTOR_OPEN *open = ctx->Open[i];
        INTERCEPTOR_SLOT *slot;

        if (open == NULL) continue;
        slot = open->Slot;

        EnterCriticalSection(&slot->Lock);
        InterceptorUnlinkOpen_Locked(slot, open);
        {
            INTERCEPTOR_STROKE_NODE *node = open->QueueHead;
            while (node != NULL)
            {
                INTERCEPTOR_STROKE_NODE *next = node->Next;
                HeapFree(GetProcessHeap(), 0, node);
                node = next;
            }
        }
        LeaveCriticalSection(&slot->Lock);

        CloseHandle(open->Event);
        HeapFree(GetProcessHeap(), 0, open);
    }

    HeapFree(GetProcessHeap(), 0, ctx);

    EnterCriticalSection(&g_EngineLock);
    InterceptorEngineStop();
    LeaveCriticalSection(&g_EngineLock);
}

InterceptorContext INTERCEPTOR_API interceptor_create_context(void)
{
    InterceptorContextData *ctx;
    int i;
    BOOL engineStarted;

    InterceptorEnsureGlobalInit();

    ctx = (InterceptorContextData *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(InterceptorContextData));
    if (ctx == NULL) return (InterceptorContext)0;

    EnterCriticalSection(&g_EngineLock);
    engineStarted = InterceptorEngineStart();
    LeaveCriticalSection(&g_EngineLock);

    if (!engineStarted)
    {
        HeapFree(GetProcessHeap(), 0, ctx);
        return (InterceptorContext)0;
    }

    for (i = 0; i < INTERCEPTOR_MAX_DEVICE; i++)
    {
        INTERCEPTOR_SLOT *slot = InterceptorSlotForDevice(i + 1);
        INTERCEPTOR_OPEN *open = (INTERCEPTOR_OPEN *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(INTERCEPTOR_OPEN));
        HANDLE ev;

        if (open == NULL) { InterceptorDestroyContextData(ctx); return (InterceptorContext)0; }

        ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (ev == NULL)
        {
            HeapFree(GetProcessHeap(), 0, open);
            InterceptorDestroyContextData(ctx);
            return (InterceptorContext)0;
        }

        open->Slot = slot;
        open->Event = ev;

        EnterCriticalSection(&slot->Lock);
        open->OpenSequence = slot->NextOpenSequence++;
        InterceptorInsertOpenSorted_Locked(slot, open);
        LeaveCriticalSection(&slot->Lock);

        ctx->Open[i] = open;
        ctx->Event[i] = ev;
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

static INTERCEPTOR_OPEN *InterceptorGetOpen(const InterceptorContextData *ctx, InterceptorDevice device)
{
    if (ctx == NULL || interceptor_is_invalid(device))
        return NULL;
    return ctx->Open[device - 1];
}

InterceptorPrecedence INTERCEPTOR_API interceptor_get_precedence(InterceptorContext context, InterceptorDevice device)
{
    INTERCEPTOR_OPEN *open = InterceptorGetOpen((const InterceptorContextData *)context, device);
    LONG value;

    if (open == NULL) return 0;

    EnterCriticalSection(&open->Slot->Lock);
    value = open->Precedence;
    LeaveCriticalSection(&open->Slot->Lock);

    return (InterceptorPrecedence)value;
}

void INTERCEPTOR_API interceptor_set_precedence(InterceptorContext context, InterceptorDevice device, InterceptorPrecedence precedence)
{
    INTERCEPTOR_OPEN *open = InterceptorGetOpen((const InterceptorContextData *)context, device);
    INTERCEPTOR_SLOT *slot;

    if (open == NULL) return;
    slot = open->Slot;

    EnterCriticalSection(&slot->Lock);
    InterceptorUnlinkOpen_Locked(slot, open);
    open->Precedence = (LONG)precedence;
    InterceptorInsertOpenSorted_Locked(slot, open);
    LeaveCriticalSection(&slot->Lock);
}

InterceptorFilter INTERCEPTOR_API interceptor_get_filter(InterceptorContext context, InterceptorDevice device)
{
    INTERCEPTOR_OPEN *open = InterceptorGetOpen((const InterceptorContextData *)context, device);
    unsigned short value;

    if (open == NULL) return 0;

    EnterCriticalSection(&open->Slot->Lock);
    value = open->Filter;
    LeaveCriticalSection(&open->Slot->Lock);

    return (InterceptorFilter)value;
}

void INTERCEPTOR_API interceptor_set_filter(InterceptorContext context, InterceptorPredicate predicate, InterceptorFilter filter)
{
    InterceptorContextData *ctx = (InterceptorContextData *)context;
    InterceptorDevice device;

    if (ctx == NULL || predicate == NULL) return;

    for (device = 1; device <= INTERCEPTOR_MAX_DEVICE; device++)
    {
        if (predicate(device))
        {
            INTERCEPTOR_OPEN *open = ctx->Open[device - 1];

            if (open != NULL)
            {
                EnterCriticalSection(&open->Slot->Lock);
                open->Filter = (unsigned short)filter;
                LeaveCriticalSection(&open->Slot->Lock);
            }
        }
    }
}

InterceptorFilter INTERCEPTOR_API interceptor_get_monitor(InterceptorContext context, InterceptorDevice device)
{
    INTERCEPTOR_OPEN *open = InterceptorGetOpen((const InterceptorContextData *)context, device);
    unsigned short value;

    if (open == NULL) return 0;

    EnterCriticalSection(&open->Slot->Lock);
    value = open->MonitorFilter;
    LeaveCriticalSection(&open->Slot->Lock);

    return (InterceptorFilter)value;
}

void INTERCEPTOR_API interceptor_set_monitor(InterceptorContext context, InterceptorPredicate predicate, InterceptorFilter filter)
{
    InterceptorContextData *ctx = (InterceptorContextData *)context;
    InterceptorDevice device;

    if (ctx == NULL || predicate == NULL) return;

    for (device = 1; device <= INTERCEPTOR_MAX_DEVICE; device++)
    {
        if (predicate(device))
        {
            INTERCEPTOR_OPEN *open = ctx->Open[device - 1];

            if (open != NULL)
            {
                EnterCriticalSection(&open->Slot->Lock);
                open->MonitorFilter = (unsigned short)filter;
                LeaveCriticalSection(&open->Slot->Lock);
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

    if (ctx == NULL) return 0;

    result = WaitForMultipleObjects((DWORD)INTERCEPTOR_MAX_DEVICE, ctx->Event, FALSE, (DWORD)milliseconds);

    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + (DWORD)INTERCEPTOR_MAX_DEVICE)
        return (InterceptorDevice)(result - WAIT_OBJECT_0 + 1);

    return 0;
}

/* --------------------------------------------------------------------- *
 * Send / receive.
 * --------------------------------------------------------------------- */

static void InterceptorBuildKeyboardInput(const InterceptorRawKeyStroke *raw, INPUT *input)
{
    ZeroMemory(input, sizeof(*input));
    input->type = INPUT_KEYBOARD;
    input->ki.wVk = 0;
    input->ki.wScan = raw->MakeCode;
    input->ki.dwFlags = KEYEVENTF_SCANCODE;
    if (raw->Flags & INTERCEPTOR_KEY_UP) input->ki.dwFlags |= KEYEVENTF_KEYUP;
    if (raw->Flags & INTERCEPTOR_KEY_E0) input->ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    /* No Win32 equivalent for KEY_E1/TERMSRV_* synthesis -- see file header. */
    input->ki.dwExtraInfo = (ULONG_PTR)raw->ExtraInformation;
}

static void InterceptorBuildMouseInput(const InterceptorRawMouseStroke *raw, INPUT *input)
{
    DWORD flags = 0;
    unsigned short b = raw->ButtonFlags;

    ZeroMemory(input, sizeof(*input));
    input->type = INPUT_MOUSE;

    if (b == 0)
    {
        flags |= MOUSEEVENTF_MOVE;
    }
    else
    {
        if (b & INTERCEPTOR_MOUSE_LEFT_BUTTON_DOWN)   flags |= MOUSEEVENTF_LEFTDOWN;
        if (b & INTERCEPTOR_MOUSE_LEFT_BUTTON_UP)     flags |= MOUSEEVENTF_LEFTUP;
        if (b & INTERCEPTOR_MOUSE_RIGHT_BUTTON_DOWN)  flags |= MOUSEEVENTF_RIGHTDOWN;
        if (b & INTERCEPTOR_MOUSE_RIGHT_BUTTON_UP)    flags |= MOUSEEVENTF_RIGHTUP;
        if (b & INTERCEPTOR_MOUSE_MIDDLE_BUTTON_DOWN) flags |= MOUSEEVENTF_MIDDLEDOWN;
        if (b & INTERCEPTOR_MOUSE_MIDDLE_BUTTON_UP)   flags |= MOUSEEVENTF_MIDDLEUP;
        if (b & INTERCEPTOR_MOUSE_BUTTON_4_DOWN)      { flags |= MOUSEEVENTF_XDOWN; input->mi.mouseData = XBUTTON1; }
        if (b & INTERCEPTOR_MOUSE_BUTTON_4_UP)        { flags |= MOUSEEVENTF_XUP;   input->mi.mouseData = XBUTTON1; }
        if (b & INTERCEPTOR_MOUSE_BUTTON_5_DOWN)      { flags |= MOUSEEVENTF_XDOWN; input->mi.mouseData = XBUTTON2; }
        if (b & INTERCEPTOR_MOUSE_BUTTON_5_UP)        { flags |= MOUSEEVENTF_XUP;   input->mi.mouseData = XBUTTON2; }
        if (b & INTERCEPTOR_MOUSE_WHEEL)              { flags |= MOUSEEVENTF_WHEEL;  input->mi.mouseData = (DWORD)(short)raw->ButtonData; }
        if (b & INTERCEPTOR_MOUSE_HWHEEL)             { flags |= MOUSEEVENTF_HWHEEL; input->mi.mouseData = (DWORD)(short)raw->ButtonData; }

        if (raw->LastX != 0 || raw->LastY != 0) flags |= MOUSEEVENTF_MOVE;
    }

    if (raw->Flags & INTERCEPTOR_MOUSE_MOVE_ABSOLUTE)   flags |= MOUSEEVENTF_ABSOLUTE;
    if (raw->Flags & INTERCEPTOR_MOUSE_VIRTUAL_DESKTOP) flags |= MOUSEEVENTF_VIRTUALDESK;

    input->mi.dx = raw->LastX;
    input->mi.dy = raw->LastY;
    input->mi.dwFlags = flags;
    input->mi.dwExtraInfo = (ULONG_PTR)raw->ExtraInformation;
}

int INTERCEPTOR_API interceptor_send(InterceptorContext context, InterceptorDevice device, const InterceptorStroke *stroke, unsigned int nstroke)
{
    InterceptorContextData *ctx = (InterceptorContextData *)context;
    INTERCEPTOR_OPEN *open;
    BOOL isMouse;
    unsigned int i;
    unsigned int accepted = 0;

    if (ctx == NULL || stroke == NULL || nstroke == 0) return 0;
    open = InterceptorGetOpen(ctx, device);
    if (open == NULL) return 0;

    isMouse = interceptor_is_mouse(device);

    for (i = 0; i < nstroke; i++)
    {
        unsigned char rawData[sizeof(InterceptorRawMouseStroke)];
        INTERCEPTOR_PENDING_ENTRY entry;
        INPUT input;

        if (isMouse)
        {
            InterceptorMouseStrokesToRaw(&stroke[i], 1, (InterceptorRawMouseStroke *)rawData);
            InterceptorBuildMouseInput((const InterceptorRawMouseStroke *)rawData, &input);
        }
        else
        {
            InterceptorKeyStrokesToRaw(&stroke[i], 1, (InterceptorRawKeyStroke *)rawData);
            InterceptorBuildKeyboardInput((const InterceptorRawKeyStroke *)rawData, &input);
        }

        entry.Slot = open->Slot;
        entry.StartAfter = open;
        CopyMemory(entry.Data, rawData, sizeof(rawData));

        InterceptorPendingPush(isMouse ? &g_InjectedMousePending : &g_InjectedKeyboardPending, &entry);

        if (SendInput(1, &input, sizeof(INPUT)) == 1) accepted++;
    }

    return (int)accepted;
}

int INTERCEPTOR_API interceptor_receive(InterceptorContext context, InterceptorDevice device, InterceptorStroke *stroke, unsigned int nstroke)
{
    InterceptorContextData *ctx = (InterceptorContextData *)context;
    INTERCEPTOR_OPEN *open;
    INTERCEPTOR_SLOT *slot;
    BOOL isMouse;
    size_t strideBytes;
    unsigned char stackBuffer[INTERCEPTOR_STACK_STROKE_CAPACITY * sizeof(InterceptorRawMouseStroke)];
    unsigned char *buffer = stackBuffer;
    BOOL heapUsed = FALSE;
    unsigned int produced = 0;

    if (ctx == NULL || stroke == NULL || nstroke == 0) return 0;
    open = InterceptorGetOpen(ctx, device);
    if (open == NULL) return 0;

    slot = open->Slot;
    isMouse = interceptor_is_mouse(device);
    strideBytes = isMouse ? sizeof(InterceptorRawMouseStroke) : sizeof(InterceptorRawKeyStroke);

    if (nstroke > INTERCEPTOR_STACK_STROKE_CAPACITY)
    {
        buffer = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (size_t)nstroke * strideBytes);
        if (buffer == NULL) return 0;
        heapUsed = TRUE;
    }

    EnterCriticalSection(&slot->Lock);
    while (produced < nstroke && open->QueueHead != NULL)
    {
        INTERCEPTOR_STROKE_NODE *node = open->QueueHead;

        open->QueueHead = node->Next;
        if (open->QueueHead == NULL) open->QueueTail = NULL;

        CopyMemory(buffer + (size_t)produced * strideBytes, node->Data, strideBytes);
        HeapFree(GetProcessHeap(), 0, node);

        open->QueuedCount--;
        produced++;
    }
    if (open->QueueHead == NULL) ResetEvent(open->Event);
    LeaveCriticalSection(&slot->Lock);

    if (isMouse)
        InterceptorRawToMouseStrokes((const InterceptorRawMouseStroke *)buffer, produced, stroke);
    else
        InterceptorRawToKeyStrokes((const InterceptorRawKeyStroke *)buffer, produced, stroke);

    if (heapUsed) HeapFree(GetProcessHeap(), 0, buffer);

    return (int)produced;
}

/* --------------------------------------------------------------------- *
 * Hardware id -- the Raw Input device interface path (e.g.
 * "\\?\HID#VID_046D&PID_C52B&...") rather than the driver's bare PnP
 * hardware ID string. VID_/PID_ substrings are present for USB HID
 * devices either way, but exact-string comparisons against IDs captured
 * from the driver build will need updating.
 * --------------------------------------------------------------------- */

unsigned int INTERCEPTOR_API interceptor_get_hardware_id(InterceptorContext context, InterceptorDevice device, void *hardware_id_buffer, unsigned int buffer_size)
{
    INTERCEPTOR_OPEN *open = InterceptorGetOpen((const InterceptorContextData *)context, device);
    INTERCEPTOR_SLOT *slot;
    USHORT length;

    if (open == NULL) return 0;
    slot = open->Slot;

    EnterCriticalSection(&g_SlotTableLock);
    length = slot->HardwareIdLength;
    if (length > buffer_size) length = 0;
    if (length > 0 && hardware_id_buffer != NULL) CopyMemory(hardware_id_buffer, slot->HardwareId, length);
    LeaveCriticalSection(&g_SlotTableLock);

    return length;
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
