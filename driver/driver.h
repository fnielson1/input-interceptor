#ifndef _INTERCEPTOR_DRIVER_H_
#define _INTERCEPTOR_DRIVER_H_

#include <ntddk.h>
#include <wdmsec.h>
#include <ntddkbd.h>
#include <ntddmou.h>
#include <kbdmou.h>
#include <devguid.h>
#include <ntstrsafe.h>

#include "protocol.h"

/* kbdmou.h only defines the keyboard-shaped PSERVICE_CALLBACK_ROUTINE; the
   mouse class driver's ClassService callback has the same PVOID-argument
   shape but no corresponding WDK typedef, so declare it here. */
typedef
VOID
(*PMOUSE_SERVICE_CALLBACK_ROUTINE) (
    _In_ PVOID NormalContext,
    _In_ PVOID SystemArgument1,
    _In_ PVOID SystemArgument2,
    _Inout_ PVOID SystemArgument3
    );

#define INTERCEPTOR_MAX_KEYBOARD 10
#define INTERCEPTOR_MAX_MOUSE    10
#define INTERCEPTOR_MAX_DEVICE   (INTERCEPTOR_MAX_KEYBOARD + INTERCEPTOR_MAX_MOUSE)

#define INTERCEPTOR_POOL_TAG 'tnIP'  /* "PInt", reversed by pool-tag byte order */

/* Per-open queue cap. Not documented anywhere upstream to match; chosen to
   absorb a slow user-mode reader for a while without unbounded pool growth. */
#define INTERCEPTOR_MAX_QUEUED_STROKES 4096

typedef enum _INTERCEPTOR_DEVICE_TYPE
{
    InterceptorControlDevice = 1,
    InterceptorFilterDevice
} INTERCEPTOR_DEVICE_TYPE;

typedef struct _INTERCEPTOR_SLOT INTERCEPTOR_SLOT, *PINTERCEPTOR_SLOT;

/*
 * One entry in an open's read queue. Sized for the larger of the two raw
 * stroke structs (MOUSE_INPUT_DATA); a keyboard slot's entries only use the
 * first sizeof(KEYBOARD_INPUT_DATA) bytes of Data.
 */
typedef struct _INTERCEPTOR_STROKE_ENTRY
{
    LIST_ENTRY Link;
    UCHAR Data[sizeof(MOUSE_INPUT_DATA)];
} INTERCEPTOR_STROKE_ENTRY, *PINTERCEPTOR_STROKE_ENTRY;

/*
 * One open handle on a control device (one InterceptorContext's worth of
 * state for one \\.\interceptionNN device -- interceptor_create_context
 * opens all 20, so a real user-mode context maps to 20 of these). Lives in
 * its Slot's OpenListHead, kept sorted ascending by Precedence.
 */
typedef struct _INTERCEPTOR_OPEN
{
    LIST_ENTRY Link;
    KSPIN_LOCK Lock;                 /* guards Precedence, Filter, Event, QueueHead, QueuedCount */
    PINTERCEPTOR_SLOT Slot;
    LONG Precedence;
    USHORT Filter;
    PKEVENT Event;
    LIST_ENTRY QueueHead;            /* of INTERCEPTOR_STROKE_ENTRY */
    ULONG QueuedCount;
    ULONG OpenSequence;              /* tiebreaker: creation order, ascending */
} INTERCEPTOR_OPEN, *PINTERCEPTOR_OPEN;

/*
 * One of the 20 fixed interceptionNN identities. ControlDeviceObject is
 * permanent (created once in DriverEntry); FilterDeviceObject is the bound
 * Filter Device Object for whichever physical keyboard/mouse currently
 * occupies this slot, or NULL when inert (see driver.c / pnp.c).
 */
typedef struct _INTERCEPTOR_SLOT
{
    ULONG Index;                     /* 0..9 within its kind */
    BOOLEAN IsMouse;
    PDEVICE_OBJECT ControlDeviceObject;

    KSPIN_LOCK Lock;                 /* guards FilterDeviceObject binding, OriginalClass*, HardwareId*, OpenListHead */
    PDEVICE_OBJECT FilterDeviceObject;
    PDEVICE_OBJECT OriginalClassDeviceObject;
    PVOID OriginalClassService;
    WCHAR HardwareId[256];
    USHORT HardwareIdLength;         /* bytes, including the terminator; 0 if none cached */
    LIST_ENTRY OpenListHead;         /* of INTERCEPTOR_OPEN, sorted ascending by Precedence */
    ULONG NextOpenSequence;
} INTERCEPTOR_SLOT;

typedef struct _INTERCEPTOR_CONTROL_EXTENSION
{
    INTERCEPTOR_DEVICE_TYPE Type;    /* InterceptorControlDevice -- must be the first field */
    PINTERCEPTOR_SLOT Slot;
} INTERCEPTOR_CONTROL_EXTENSION, *PINTERCEPTOR_CONTROL_EXTENSION;

typedef struct _INTERCEPTOR_FIDO_EXTENSION
{
    INTERCEPTOR_DEVICE_TYPE Type;    /* InterceptorFilterDevice -- must be the first field */
    PDEVICE_OBJECT LowerDeviceObject;
    PDEVICE_OBJECT PhysicalDeviceObject;
    PINTERCEPTOR_SLOT Slot;
    BOOLEAN IsMouse;
    BOOLEAN Started;
} INTERCEPTOR_FIDO_EXTENSION, *PINTERCEPTOR_FIDO_EXTENSION;

extern INTERCEPTOR_SLOT g_KeyboardSlot[INTERCEPTOR_MAX_KEYBOARD];
extern INTERCEPTOR_SLOT g_MouseSlot[INTERCEPTOR_MAX_MOUSE];
extern FAST_MUTEX g_SlotAllocationLock;
extern PDRIVER_OBJECT g_DriverObject;

/* driver.c */
DRIVER_INITIALIZE DriverEntry;

/* pnp.c */
DRIVER_ADD_DEVICE InterceptorAddDevice;
_Dispatch_type_(IRP_MJ_PNP) DRIVER_DISPATCH InterceptorPnp;
_Dispatch_type_(IRP_MJ_POWER) DRIVER_DISPATCH InterceptorPower;

/* connect.c */
_Dispatch_type_(IRP_MJ_INTERNAL_DEVICE_CONTROL) DRIVER_DISPATCH InterceptorInternalDeviceControl;

/* dispatch.c */
_Dispatch_type_(IRP_MJ_CREATE) _Dispatch_type_(IRP_MJ_CLOSE) DRIVER_DISPATCH InterceptorCreateClose;
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL) DRIVER_DISPATCH InterceptorDeviceControl;

/* queue.c */
PINTERCEPTOR_OPEN InterceptorCreateOpen(_In_ PINTERCEPTOR_SLOT Slot);
VOID InterceptorCloseOpen(_In_ PINTERCEPTOR_OPEN Open);
VOID InterceptorSetOpenPrecedence(_In_ PINTERCEPTOR_OPEN Open, _In_ LONG Precedence);

VOID InterceptorDeliverKeyboardStrokes(
    _In_ PINTERCEPTOR_SLOT Slot,
    _In_opt_ PINTERCEPTOR_OPEN StartAfter,
    _In_ PKEYBOARD_INPUT_DATA InputDataStart,
    _In_ PKEYBOARD_INPUT_DATA InputDataEnd);

VOID InterceptorDeliverMouseStrokes(
    _In_ PINTERCEPTOR_SLOT Slot,
    _In_opt_ PINTERCEPTOR_OPEN StartAfter,
    _In_ PMOUSE_INPUT_DATA InputDataStart,
    _In_ PMOUSE_INPUT_DATA InputDataEnd);

ULONG InterceptorReadStrokes(
    _In_ PINTERCEPTOR_OPEN Open,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _In_ BOOLEAN IsMouse);

#endif
