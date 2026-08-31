/*
 * connect.c -- the CONNECT_DATA substitution that lets this driver see every
 * real keystroke/mouse-event as it arrives from hardware, plus the service
 * callbacks substituted in its place.
 *
 * This driver registers as a LowerFilter of the keyboard/mouse device setup
 * classes (see interceptor-driver.inf), sitting between kbdclass/mouclass
 * and the underlying port/HID function driver -- not above kbdclass/
 * mouclass, which is where an upper filter would sit. That placement
 * matters here: during their own IRP_MN_START_DEVICE handling, kbdclass/
 * mouclass send IOCTL_INTERNAL_KEYBOARD_CONNECT (or _MOUSE_CONNECT) *down*
 * the stack to connect to the lower driver's data-ready callback -- a
 * CONNECT_DATA structure carrying a DeviceObject/ClassService pair the
 * lower driver is meant to invoke whenever new input arrives. Sitting below
 * kbdclass/mouclass puts that IRP on our own dispatch path on its way down,
 * which is what makes the substitution below possible; sitting above them
 * would never see it. This is the standard technique documented by
 * Microsoft's own kbfiltr.c/moufiltr.c WDK samples, reasoned here from the
 * public CONNECT_DATA mechanism -- not derived from oblitum's driver, whose
 * source has never been published (see the plan this was built from).
 */

#include "driver.h"

static VOID InterceptorKeyboardService(_In_ PDEVICE_OBJECT DeviceObject,
    _In_ PKEYBOARD_INPUT_DATA InputDataStart, _In_ PKEYBOARD_INPUT_DATA InputDataEnd,
    _Inout_ PULONG InputDataConsumed)
{
    PINTERCEPTOR_FIDO_EXTENSION extension = (PINTERCEPTOR_FIDO_EXTENSION)DeviceObject->DeviceExtension;

    /* This governs the port driver's own circular-buffer bookkeeping, not
       our filtering -- always claim the full batch. */
    *InputDataConsumed = (ULONG)(InputDataEnd - InputDataStart);

    InterceptorDeliverKeyboardStrokes(extension->Slot, NULL, InputDataStart, InputDataEnd);
}

static VOID InterceptorMouseService(_In_ PDEVICE_OBJECT DeviceObject,
    _In_ PMOUSE_INPUT_DATA InputDataStart, _In_ PMOUSE_INPUT_DATA InputDataEnd,
    _Inout_ PULONG InputDataConsumed)
{
    PINTERCEPTOR_FIDO_EXTENSION extension = (PINTERCEPTOR_FIDO_EXTENSION)DeviceObject->DeviceExtension;

    *InputDataConsumed = (ULONG)(InputDataEnd - InputDataStart);

    InterceptorDeliverMouseStrokes(extension->Slot, NULL, InputDataStart, InputDataEnd);
}

NTSTATUS InterceptorInternalDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    INTERCEPTOR_DEVICE_TYPE type = *(INTERCEPTOR_DEVICE_TYPE *)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PINTERCEPTOR_FIDO_EXTENSION extension;
    ULONG code;

    if (type != InterceptorFilterDevice)
    {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    extension = (PINTERCEPTOR_FIDO_EXTENSION)DeviceObject->DeviceExtension;
    code = stack->Parameters.DeviceIoControl.IoControlCode;

    if ((!extension->IsMouse && code == IOCTL_INTERNAL_KEYBOARD_CONNECT) ||
        (extension->IsMouse && code == IOCTL_INTERNAL_MOUSE_CONNECT))
    {
        PCONNECT_DATA connectData = (PCONNECT_DATA)stack->Parameters.DeviceIoControl.Type3InputBuffer;

        if (connectData == NULL || stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(CONNECT_DATA))
        {
            Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_INVALID_PARAMETER;
        }

        extension->Slot->OriginalClassDeviceObject = connectData->ClassDeviceObject;
        extension->Slot->OriginalClassService = connectData->ClassService;

        connectData->ClassDeviceObject = DeviceObject;
        connectData->ClassService = extension->IsMouse
            ? (PVOID)InterceptorMouseService
            : (PVOID)InterceptorKeyboardService;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(extension->LowerDeviceObject, Irp);
}
