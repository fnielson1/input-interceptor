/*
 * dispatch.c -- IRP_MJ_CREATE/CLOSE and IRP_MJ_DEVICE_CONTROL for the 20
 * control devices (the user-mode-visible \\.\interceptionNN handles), plus
 * plain passthrough for the same major functions when they land on a
 * Filter Device Object instead (kbdclass/mouclass or the port driver below
 * can exchange ordinary device-control IOCTLs, e.g.
 * IOCTL_KEYBOARD_QUERY_ATTRIBUTES, that must still reach real hardware).
 *
 * The 8 IOCTLs implemented here are exactly the ones src/interceptor.c
 * sends -- see driver/protocol.h.
 */

#include "driver.h"

NTSTATUS InterceptorCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    INTERCEPTOR_DEVICE_TYPE type = *(INTERCEPTOR_DEVICE_TYPE *)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;

    if (type == InterceptorFilterDevice)
    {
        PINTERCEPTOR_FIDO_EXTENSION extension = (PINTERCEPTOR_FIDO_EXTENSION)DeviceObject->DeviceExtension;
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(extension->LowerDeviceObject, Irp);
    }

    {
        PINTERCEPTOR_CONTROL_EXTENSION extension = (PINTERCEPTOR_CONTROL_EXTENSION)DeviceObject->DeviceExtension;

        if (stack->MajorFunction == IRP_MJ_CREATE)
        {
            PINTERCEPTOR_OPEN open = InterceptorCreateOpen(extension->Slot);

            if (open == NULL)
                status = STATUS_INSUFFICIENT_RESOURCES;
            else
                stack->FileObject->FsContext = open;
        }
        else /* IRP_MJ_CLOSE */
        {
            PINTERCEPTOR_OPEN open = (PINTERCEPTOR_OPEN)stack->FileObject->FsContext;

            if (open != NULL)
            {
                InterceptorCloseOpen(open);
                stack->FileObject->FsContext = NULL;
            }
        }
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS InterceptorDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    INTERCEPTOR_DEVICE_TYPE type;
    PIO_STACK_LOCATION stack;
    PINTERCEPTOR_CONTROL_EXTENSION extension;
    PINTERCEPTOR_SLOT slot;
    PINTERCEPTOR_OPEN open;
    PVOID buffer;
    ULONG inLength, outLength;
    NTSTATUS status;
    ULONG_PTR information;

    type = *(INTERCEPTOR_DEVICE_TYPE *)DeviceObject->DeviceExtension;
    stack = IoGetCurrentIrpStackLocation(Irp);

    if (type == InterceptorFilterDevice)
    {
        PINTERCEPTOR_FIDO_EXTENSION fidoExtension = (PINTERCEPTOR_FIDO_EXTENSION)DeviceObject->DeviceExtension;
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(fidoExtension->LowerDeviceObject, Irp);
    }

    extension = (PINTERCEPTOR_CONTROL_EXTENSION)DeviceObject->DeviceExtension;
    slot = extension->Slot;
    open = (PINTERCEPTOR_OPEN)stack->FileObject->FsContext;
    buffer = Irp->AssociatedIrp.SystemBuffer;
    inLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    outLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    status = STATUS_SUCCESS;
    information = 0;

    if (open == NULL)
    {
        status = STATUS_INVALID_DEVICE_STATE;
    }
    else switch (stack->Parameters.DeviceIoControl.IoControlCode)
    {
    case IOCTL_INTERCEPTOR_SET_PRECEDENCE:
        if (inLength < sizeof(LONG)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        InterceptorSetOpenPrecedence(open, *(PLONG)buffer);
        break;

    case IOCTL_INTERCEPTOR_GET_PRECEDENCE:
        if (outLength < sizeof(LONG)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        {
            KIRQL irql;
            KeAcquireSpinLock(&open->Lock, &irql);
            *(PLONG)buffer = open->Precedence;
            KeReleaseSpinLock(&open->Lock, irql);
        }
        information = sizeof(LONG);
        break;

    case IOCTL_INTERCEPTOR_SET_FILTER:
        if (inLength < sizeof(USHORT)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        {
            KIRQL irql;
            KeAcquireSpinLock(&open->Lock, &irql);
            open->Filter = *(PUSHORT)buffer;
            KeReleaseSpinLock(&open->Lock, irql);
        }
        break;

    case IOCTL_INTERCEPTOR_GET_FILTER:
        if (outLength < sizeof(USHORT)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        {
            KIRQL irql;
            KeAcquireSpinLock(&open->Lock, &irql);
            *(PUSHORT)buffer = open->Filter;
            KeReleaseSpinLock(&open->Lock, irql);
        }
        information = sizeof(USHORT);
        break;

    case IOCTL_INTERCEPTOR_SET_EVENT:
        /* interceptor.c zero-pads to HANDLE[2] but only slot 0 is used. */
        if (inLength < sizeof(HANDLE) * 2) { status = STATUS_BUFFER_TOO_SMALL; break; }
        {
            HANDLE eventHandle = ((PHANDLE)buffer)[0];
            PKEVENT eventObject = NULL;
            KIRQL irql;

            if (eventHandle != NULL)
            {
                status = ObReferenceObjectByHandle(eventHandle, EVENT_MODIFY_STATE, *ExEventObjectType,
                    Irp->RequestorMode, (PVOID *)&eventObject, NULL);
                if (!NT_SUCCESS(status)) break;
            }

            KeAcquireSpinLock(&open->Lock, &irql);
            if (open->Event != NULL) ObDereferenceObject(open->Event);
            open->Event = eventObject;
            if (open->Event != NULL)
            {
                /* Reflect current queue state immediately: a SET_EVENT that
                   arrives after strokes are already queued must not miss
                   them. */
                if (open->QueuedCount > 0) KeSetEvent(open->Event, IO_NO_INCREMENT, FALSE);
                else KeClearEvent(open->Event);
            }
            KeReleaseSpinLock(&open->Lock, irql);

            status = STATUS_SUCCESS;
        }
        break;

    case IOCTL_INTERCEPTOR_WRITE:
        {
            BOOLEAN isMouse = slot->IsMouse;
            ULONG entrySize = isMouse ? sizeof(MOUSE_INPUT_DATA) : sizeof(KEYBOARD_INPUT_DATA);
            ULONG count = inLength / entrySize;
            PDEVICE_OBJECT filterDeviceObject = slot->FilterDeviceObject;

            if (count == 0 || filterDeviceObject == NULL || (ULONG_PTR)filterDeviceObject == 1)
            {
                /* No physical device bound to this slot (inert), or nothing
                   to write: nothing accepted. */
                information = 0;
            }
            else if (isMouse)
            {
                InterceptorDeliverMouseStrokes(slot, open, (PMOUSE_INPUT_DATA)buffer,
                    (PMOUSE_INPUT_DATA)buffer + count);
                information = (ULONG_PTR)count * entrySize;
            }
            else
            {
                InterceptorDeliverKeyboardStrokes(slot, open, (PKEYBOARD_INPUT_DATA)buffer,
                    (PKEYBOARD_INPUT_DATA)buffer + count);
                information = (ULONG_PTR)count * entrySize;
            }
        }
        break;

    case IOCTL_INTERCEPTOR_READ:
        information = InterceptorReadStrokes(open, buffer, outLength, slot->IsMouse);
        break;

    case IOCTL_INTERCEPTOR_GET_HARDWARE_ID:
        {
            KIRQL irql;
            USHORT length;

            KeAcquireSpinLock(&slot->Lock, &irql);
            length = slot->HardwareIdLength;
            /* Caller's buffer too small: report nothing rather than a
               partial string, matching samples/hardwareid.cpp's
               "0 < length < sizeof(buffer)" trust contract. */
            if (length > outLength) length = 0;
            if (length > 0) RtlCopyMemory(buffer, slot->HardwareId, length);
            KeReleaseSpinLock(&slot->Lock, irql);

            information = length;
        }
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}
