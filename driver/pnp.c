/*
 * pnp.c -- PnP glue: AddDevice creates one Filter Device Object (FiDO) per
 * physical keyboard/mouse instance and binds it to the first free slot of
 * the matching kind (see driver.h); IRP_MJ_PNP/IRP_MJ_POWER pass everything
 * through unmodified except IRP_MN_START_DEVICE (cache the hardware ID once
 * the lower stack is up) and IRP_MN_REMOVE_DEVICE (unbind the slot and tear
 * the FiDO down).
 *
 * This driver is registered as a LowerFilter of the keyboard and mouse
 * device setup classes (see interceptor-driver.inf), so the PnP manager
 * calls AddDevice for every keyboard/mouse PDO the usual class-filter way --
 * see connect.c for why LowerFilters, not UpperFilters, is required here.
 */

#include "driver.h"

static PINTERCEPTOR_SLOT InterceptorAllocateSlot(_In_ BOOLEAN IsMouse)
{
    PINTERCEPTOR_SLOT table = IsMouse ? g_MouseSlot : g_KeyboardSlot;
    ULONG count = IsMouse ? INTERCEPTOR_MAX_MOUSE : INTERCEPTOR_MAX_KEYBOARD;
    ULONG i;
    PINTERCEPTOR_SLOT found = NULL;

    ExAcquireFastMutex(&g_SlotAllocationLock);
    for (i = 0; i < count; ++i)
    {
        if (table[i].FilterDeviceObject == NULL)
        {
            found = &table[i];
            break;
        }
    }
    /* Reserve immediately, still under the mutex, with a non-NULL sentinel;
       the real FiDO pointer is filled in right after by the caller. Nothing
       dereferences FilterDeviceObject as a device object until AddDevice
       finishes binding it below. */
    if (found != NULL) found->FilterDeviceObject = (PDEVICE_OBJECT)(ULONG_PTR)1;
    ExReleaseFastMutex(&g_SlotAllocationLock);

    return found;
}

static VOID InterceptorReleaseSlot(_In_ PINTERCEPTOR_SLOT Slot)
{
    KIRQL irql;

    ExAcquireFastMutex(&g_SlotAllocationLock);
    KeAcquireSpinLock(&Slot->Lock, &irql);
    Slot->FilterDeviceObject = NULL;
    Slot->OriginalClassDeviceObject = NULL;
    Slot->OriginalClassService = NULL;
    Slot->HardwareIdLength = 0;
    KeReleaseSpinLock(&Slot->Lock, irql);
    ExReleaseFastMutex(&g_SlotAllocationLock);
}

static NTSTATUS InterceptorStartCompletion(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static VOID InterceptorCacheHardwareId(_In_ PINTERCEPTOR_FIDO_EXTENSION Extension)
{
    NTSTATUS status;
    ULONG resultLength = 0;
    PINTERCEPTOR_SLOT slot = Extension->Slot;
    KIRQL irql;

    KeAcquireSpinLock(&slot->Lock, &irql);

    RtlZeroMemory(slot->HardwareId, sizeof(slot->HardwareId));
    status = IoGetDeviceProperty(Extension->PhysicalDeviceObject, DevicePropertyHardwareID,
        sizeof(slot->HardwareId), slot->HardwareId, &resultLength);

    /* IoGetDeviceProperty returns the PDO's full REG_MULTI_SZ (most specific
       hardware ID first, NUL-separated, double-NUL terminated); only the
       first string is reported through GET_HARDWARE_ID, matching
       samples/hardwareid.cpp's single-wchar_t-string usage. */
    if (NT_SUCCESS(status) && resultLength > 0 && slot->HardwareId[0] != L'\0')
        slot->HardwareIdLength = (USHORT)((wcslen(slot->HardwareId) + 1) * sizeof(WCHAR));
    else
        slot->HardwareIdLength = 0;

    KeReleaseSpinLock(&slot->Lock, irql);
}

NTSTATUS InterceptorAddDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    NTSTATUS status;
    PDEVICE_OBJECT filterDeviceObject = NULL;
    PDEVICE_OBJECT lowerDeviceObject;
    PINTERCEPTOR_FIDO_EXTENSION extension;
    BOOLEAN isMouse;
    PINTERCEPTOR_SLOT slot;

    status = IoCreateDevice(DriverObject, sizeof(INTERCEPTOR_FIDO_EXTENSION), NULL, FILE_DEVICE_UNKNOWN, 0, FALSE,
        &filterDeviceObject);
    if (!NT_SUCCESS(status)) return status;

    lowerDeviceObject = IoAttachDeviceToDeviceStack(filterDeviceObject, PhysicalDeviceObject);
    if (lowerDeviceObject == NULL)
    {
        IoDeleteDevice(filterDeviceObject);
        return STATUS_DEVICE_REMOVED;
    }

    /* Classify by the lower device's own DeviceType, which kbdclass/
       mouclass and the underlying port/HID stack always set correctly
       (FILE_DEVICE_KEYBOARD / FILE_DEVICE_MOUSE), rather than trying to
       infer which class GUID's LowerFilters entry triggered this call. */
    isMouse = (lowerDeviceObject->DeviceType == FILE_DEVICE_MOUSE);

    slot = InterceptorAllocateSlot(isMouse);
    if (slot == NULL)
    {
        /* All 10 slots of this kind are already bound: this hardware keeps
           working normally through kbdclass/mouclass, just outside the
           fixed 10+10 range the wire protocol supports. */
        IoDetachDevice(lowerDeviceObject);
        IoDeleteDevice(filterDeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    extension = (PINTERCEPTOR_FIDO_EXTENSION)filterDeviceObject->DeviceExtension;
    RtlZeroMemory(extension, sizeof(*extension));
    extension->Type = InterceptorFilterDevice;
    extension->LowerDeviceObject = lowerDeviceObject;
    extension->PhysicalDeviceObject = PhysicalDeviceObject;
    extension->Slot = slot;
    extension->IsMouse = isMouse;

    filterDeviceObject->Flags |= (lowerDeviceObject->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO));

    slot->FilterDeviceObject = filterDeviceObject;

    filterDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}

NTSTATUS InterceptorPnp(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    INTERCEPTOR_DEVICE_TYPE type = *(INTERCEPTOR_DEVICE_TYPE *)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PINTERCEPTOR_FIDO_EXTENSION extension;
    NTSTATUS status;

    if (type != InterceptorFilterDevice)
    {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    extension = (PINTERCEPTOR_FIDO_EXTENSION)DeviceObject->DeviceExtension;

    switch (stack->MinorFunction)
    {
    case IRP_MN_START_DEVICE:
        {
            KEVENT event;

            KeInitializeEvent(&event, NotificationEvent, FALSE);
            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(Irp, InterceptorStartCompletion, &event, TRUE, TRUE, TRUE);

            status = IoCallDriver(extension->LowerDeviceObject, Irp);
            if (status == STATUS_PENDING)
            {
                KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
                status = Irp->IoStatus.Status;
            }

            if (NT_SUCCESS(status))
            {
                InterceptorCacheHardwareId(extension);
                extension->Started = TRUE;
            }

            Irp->IoStatus.Status = status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return status;
        }

    case IRP_MN_REMOVE_DEVICE:
        {
            PINTERCEPTOR_SLOT slot = extension->Slot;
            PDEVICE_OBJECT lowerDeviceObject = extension->LowerDeviceObject;

            IoSkipCurrentIrpStackLocation(Irp);
            status = IoCallDriver(lowerDeviceObject, Irp);

            InterceptorReleaseSlot(slot);
            IoDetachDevice(lowerDeviceObject);
            IoDeleteDevice(DeviceObject);

            return status;
        }

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(extension->LowerDeviceObject, Irp);
    }
}

NTSTATUS InterceptorPower(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    INTERCEPTOR_DEVICE_TYPE type = *(INTERCEPTOR_DEVICE_TYPE *)DeviceObject->DeviceExtension;
    PINTERCEPTOR_FIDO_EXTENSION extension;

    if (type != InterceptorFilterDevice)
    {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    extension = (PINTERCEPTOR_FIDO_EXTENSION)DeviceObject->DeviceExtension;

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(extension->LowerDeviceObject, Irp);
}
