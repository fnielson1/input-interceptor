/*
 * queue.c -- per-open stroke queues, and the chain-of-responsibility
 * delivery engine shared by real hardware arrival (connect.c) and
 * IOCTL_WRITE reinjection (dispatch.c).
 *
 * Original design (not verified against or copied from oblitum's driver --
 * see the "Precedence / multi-open chain-of-responsibility" section of the
 * plan this was built from; that driver's source has never been published).
 *
 * A slot's open handles are kept sorted ascending by precedence: lower
 * precedence sees a stroke first. Each stroke is offered to opens in that
 * order; the first whose filter mask matches withholds it (queues it, does
 * not forward further); if none match, it is forwarded to the real
 * ClassService untouched. Reinjecting a withheld stroke via IOCTL_WRITE
 * resumes the same walk from the *next* open after the one that reinjected
 * it, so a filter's edited/passed-through output flows only to the
 * remaining downstream filters and, eventually, to real hardware -- never
 * back into the filter that just released it.
 */

#include "driver.h"

typedef BOOLEAN (*INTERCEPTOR_STROKE_MATCHES)(_In_ PVOID RawStroke, _In_ USHORT Filter);

static BOOLEAN InterceptorKeyboardMatches(_In_ PVOID RawStroke, _In_ USHORT Filter)
{
    PKEYBOARD_INPUT_DATA stroke = (PKEYBOARD_INPUT_DATA)RawStroke;
    USHORT flags = stroke->Flags;

    if ((flags & KEY_BREAK) == 0)
    {
        if (Filter & INTERCEPTOR_FILTER_KEY_DOWN) return TRUE;
    }
    else
    {
        if (Filter & INTERCEPTOR_FILTER_KEY_UP) return TRUE;
    }

    if ((flags & KEY_E0) && (Filter & INTERCEPTOR_FILTER_KEY_E0)) return TRUE;
    if ((flags & KEY_E1) && (Filter & INTERCEPTOR_FILTER_KEY_E1)) return TRUE;
    if ((flags & KEY_TERMSRV_SET_LED) && (Filter & INTERCEPTOR_FILTER_KEY_TERMSRV_SET_LED)) return TRUE;
    if ((flags & KEY_TERMSRV_SHADOW) && (Filter & INTERCEPTOR_FILTER_KEY_TERMSRV_SHADOW)) return TRUE;
    if ((flags & KEY_TERMSRV_VKPACKET) && (Filter & INTERCEPTOR_FILTER_KEY_TERMSRV_VKPACKET)) return TRUE;

    return FALSE;
}

static BOOLEAN InterceptorMouseMatches(_In_ PVOID RawStroke, _In_ USHORT Filter)
{
    PMOUSE_INPUT_DATA stroke = (PMOUSE_INPUT_DATA)RawStroke;
    USHORT buttonFlags = stroke->ButtonFlags;

    if (buttonFlags == 0)
        return (Filter & INTERCEPTOR_FILTER_MOUSE_MOVE) != 0;

    return (Filter & buttonFlags) != 0;
}

static VOID InterceptorInsertOpenSorted_Locked(_In_ PINTERCEPTOR_SLOT Slot, _In_ PINTERCEPTOR_OPEN Open)
{
    PLIST_ENTRY entry;

    for (entry = Slot->OpenListHead.Flink; entry != &Slot->OpenListHead; entry = entry->Flink)
    {
        PINTERCEPTOR_OPEN existing = CONTAINING_RECORD(entry, INTERCEPTOR_OPEN, Link);

        if (existing->Precedence > Open->Precedence ||
            (existing->Precedence == Open->Precedence && existing->OpenSequence > Open->OpenSequence))
            break;
    }

    InsertTailList(entry, &Open->Link);
}

PINTERCEPTOR_OPEN InterceptorCreateOpen(_In_ PINTERCEPTOR_SLOT Slot)
{
    PINTERCEPTOR_OPEN open;
    KIRQL irql;

    open = (PINTERCEPTOR_OPEN)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(INTERCEPTOR_OPEN), INTERCEPTOR_POOL_TAG);
    if (open == NULL) return NULL;

    RtlZeroMemory(open, sizeof(*open));
    open->Slot = Slot;
    KeInitializeSpinLock(&open->Lock);
    InitializeListHead(&open->QueueHead);

    KeAcquireSpinLock(&Slot->Lock, &irql);
    open->OpenSequence = Slot->NextOpenSequence++;
    InterceptorInsertOpenSorted_Locked(Slot, open);
    KeReleaseSpinLock(&Slot->Lock, irql);

    return open;
}

VOID InterceptorCloseOpen(_In_ PINTERCEPTOR_OPEN Open)
{
    PINTERCEPTOR_SLOT slot = Open->Slot;
    KIRQL irql;

    KeAcquireSpinLock(&slot->Lock, &irql);
    RemoveEntryList(&Open->Link);
    KeReleaseSpinLock(&slot->Lock, irql);

    /* Any strokes still queued here were withheld from the chain and are now
       lost -- consistent with the README's existing caveat that an
       unresponsive filter freezes/loses the input it filters. */
    KeAcquireSpinLock(&Open->Lock, &irql);
    while (!IsListEmpty(&Open->QueueHead))
    {
        PLIST_ENTRY entry = RemoveHeadList(&Open->QueueHead);
        ExFreePoolWithTag(CONTAINING_RECORD(entry, INTERCEPTOR_STROKE_ENTRY, Link), INTERCEPTOR_POOL_TAG);
    }
    if (Open->Event != NULL) ObDereferenceObject(Open->Event);
    KeReleaseSpinLock(&Open->Lock, irql);

    ExFreePoolWithTag(Open, INTERCEPTOR_POOL_TAG);
}

VOID InterceptorSetOpenPrecedence(_In_ PINTERCEPTOR_OPEN Open, _In_ LONG Precedence)
{
    PINTERCEPTOR_SLOT slot = Open->Slot;
    KIRQL slotIrql, openIrql;

    KeAcquireSpinLock(&slot->Lock, &slotIrql);
    RemoveEntryList(&Open->Link);

    KeAcquireSpinLock(&Open->Lock, &openIrql);
    Open->Precedence = Precedence;
    KeReleaseSpinLock(&Open->Lock, openIrql);

    InterceptorInsertOpenSorted_Locked(slot, Open);
    KeReleaseSpinLock(&slot->Lock, slotIrql);
}

static VOID InterceptorDeliverGeneric(
    _In_ PINTERCEPTOR_SLOT Slot,
    _In_opt_ PINTERCEPTOR_OPEN StartAfter,
    _In_ PUCHAR InputDataStart,
    _In_ PUCHAR InputDataEnd,
    _In_ ULONG StrideBytes,
    _In_ INTERCEPTOR_STROKE_MATCHES Matches,
    _In_ BOOLEAN IsMouse)
{
    UCHAR stackBuffer[32 * sizeof(MOUSE_INPUT_DATA)];
    PUCHAR heapBuffer = NULL;
    PUCHAR passBuffer = stackBuffer;
    ULONG passCapacityBytes = sizeof(stackBuffer);
    ULONG passLengthBytes = 0;
    ULONG totalBytes = (ULONG)(InputDataEnd - InputDataStart);
    ULONG consumed;
    PUCHAR cursor;
    PDEVICE_OBJECT originalClassDeviceObject;
    PVOID originalClassService;

    /* Read once, outside the per-stroke loop: these only change at CONNECT
       (before any stroke can arrive) and at REMOVE (which Windows only lets
       proceed once the underlying stack has quiesced), so a lock-free read
       here is a deliberate, low-risk simplification. */
    originalClassDeviceObject = Slot->OriginalClassDeviceObject;
    originalClassService = Slot->OriginalClassService;
    if (originalClassService == NULL) return;

    if (totalBytes > passCapacityBytes)
    {
        heapBuffer = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, totalBytes, INTERCEPTOR_POOL_TAG);
        if (heapBuffer != NULL)
        {
            passBuffer = heapBuffer;
            passCapacityBytes = totalBytes;
        }
        /* Allocation failed: fall back to flushing the stack buffer in
           chunks as it fills, in the loop below. */
    }

    for (cursor = InputDataStart; cursor < InputDataEnd; cursor += StrideBytes)
    {
        KIRQL slotIrql;
        PLIST_ENTRY entry;
        BOOLEAN started = (StartAfter == NULL);
        BOOLEAN captured = FALSE;

        KeAcquireSpinLock(&Slot->Lock, &slotIrql);

        for (entry = Slot->OpenListHead.Flink; entry != &Slot->OpenListHead && !captured; entry = entry->Flink)
        {
            PINTERCEPTOR_OPEN candidate = CONTAINING_RECORD(entry, INTERCEPTOR_OPEN, Link);
            KIRQL openIrql;

            if (!started)
            {
                if (candidate == StartAfter) started = TRUE;
                continue;
            }

            KeAcquireSpinLock(&candidate->Lock, &openIrql);
            if (Matches(cursor, candidate->Filter))
            {
                PINTERCEPTOR_STROKE_ENTRY queueEntry = NULL;

                if (candidate->QueuedCount < INTERCEPTOR_MAX_QUEUED_STROKES)
                    queueEntry = (PINTERCEPTOR_STROKE_ENTRY)ExAllocatePool2(
                        POOL_FLAG_NON_PAGED, sizeof(INTERCEPTOR_STROKE_ENTRY), INTERCEPTOR_POOL_TAG);

                if (queueEntry != NULL)
                {
                    RtlCopyMemory(queueEntry->Data, cursor, StrideBytes);
                    InsertTailList(&candidate->QueueHead, &queueEntry->Link);
                    candidate->QueuedCount += 1;
                    if (candidate->QueuedCount == 1 && candidate->Event != NULL)
                        KeSetEvent(candidate->Event, IO_NO_INCREMENT, FALSE);
                }
                /* Queue full or allocation failed: the stroke is still
                   withheld from the chain (dropped) rather than let it fall
                   through to hardware out of order. */
                captured = TRUE;
            }
            KeReleaseSpinLock(&candidate->Lock, openIrql);
        }

        KeReleaseSpinLock(&Slot->Lock, slotIrql);

        if (!captured)
        {
            if (passLengthBytes + StrideBytes > passCapacityBytes)
            {
                if (IsMouse)
                    ((PMOUSE_SERVICE_CALLBACK_ROUTINE)originalClassService)(originalClassDeviceObject,
                        (PMOUSE_INPUT_DATA)passBuffer, (PMOUSE_INPUT_DATA)(passBuffer + passLengthBytes), &consumed);
                else
                    ((PSERVICE_CALLBACK_ROUTINE)originalClassService)(originalClassDeviceObject,
                        (PKEYBOARD_INPUT_DATA)passBuffer, (PKEYBOARD_INPUT_DATA)(passBuffer + passLengthBytes), &consumed);
                passLengthBytes = 0;
            }
            RtlCopyMemory(passBuffer + passLengthBytes, cursor, StrideBytes);
            passLengthBytes += StrideBytes;
        }
    }

    if (passLengthBytes > 0)
    {
        if (IsMouse)
            ((PMOUSE_SERVICE_CALLBACK_ROUTINE)originalClassService)(originalClassDeviceObject,
                (PMOUSE_INPUT_DATA)passBuffer, (PMOUSE_INPUT_DATA)(passBuffer + passLengthBytes), &consumed);
        else
            ((PSERVICE_CALLBACK_ROUTINE)originalClassService)(originalClassDeviceObject,
                (PKEYBOARD_INPUT_DATA)passBuffer, (PKEYBOARD_INPUT_DATA)(passBuffer + passLengthBytes), &consumed);
    }

    if (heapBuffer != NULL) ExFreePoolWithTag(heapBuffer, INTERCEPTOR_POOL_TAG);
}

VOID InterceptorDeliverKeyboardStrokes(_In_ PINTERCEPTOR_SLOT Slot, _In_opt_ PINTERCEPTOR_OPEN StartAfter,
    _In_ PKEYBOARD_INPUT_DATA InputDataStart, _In_ PKEYBOARD_INPUT_DATA InputDataEnd)
{
    InterceptorDeliverGeneric(Slot, StartAfter, (PUCHAR)InputDataStart, (PUCHAR)InputDataEnd,
        sizeof(KEYBOARD_INPUT_DATA), InterceptorKeyboardMatches, FALSE);
}

VOID InterceptorDeliverMouseStrokes(_In_ PINTERCEPTOR_SLOT Slot, _In_opt_ PINTERCEPTOR_OPEN StartAfter,
    _In_ PMOUSE_INPUT_DATA InputDataStart, _In_ PMOUSE_INPUT_DATA InputDataEnd)
{
    InterceptorDeliverGeneric(Slot, StartAfter, (PUCHAR)InputDataStart, (PUCHAR)InputDataEnd,
        sizeof(MOUSE_INPUT_DATA), InterceptorMouseMatches, TRUE);
}

ULONG InterceptorReadStrokes(_In_ PINTERCEPTOR_OPEN Open, _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength, _In_ BOOLEAN IsMouse)
{
    ULONG strideBytes = IsMouse ? sizeof(MOUSE_INPUT_DATA) : sizeof(KEYBOARD_INPUT_DATA);
    ULONG capacity = OutputBufferLength / strideBytes;
    ULONG produced = 0;
    PUCHAR cursor = (PUCHAR)OutputBuffer;
    KIRQL irql;

    KeAcquireSpinLock(&Open->Lock, &irql);

    while (produced < capacity && !IsListEmpty(&Open->QueueHead))
    {
        PLIST_ENTRY entry = RemoveHeadList(&Open->QueueHead);
        PINTERCEPTOR_STROKE_ENTRY queueEntry = CONTAINING_RECORD(entry, INTERCEPTOR_STROKE_ENTRY, Link);

        RtlCopyMemory(cursor, queueEntry->Data, strideBytes);
        ExFreePoolWithTag(queueEntry, INTERCEPTOR_POOL_TAG);

        Open->QueuedCount -= 1;
        cursor += strideBytes;
        produced += 1;
    }

    if (IsListEmpty(&Open->QueueHead) && Open->Event != NULL)
        KeClearEvent(Open->Event);

    KeReleaseSpinLock(&Open->Lock, irql);

    return produced * strideBytes;
}
