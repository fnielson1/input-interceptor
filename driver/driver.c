/*
 * driver.c -- DriverEntry/DriverUnload and the 20 permanent control device
 * objects (\Device\interception00..19, symlinked as \??\interception00..19).
 *
 * These control devices are NOT part of any PnP stack: they are created
 * once here and live for the driver's whole lifetime, independent of which
 * physical keyboards/mice are currently present. That is what lets
 * interceptor_create_context() open all 20 \\.\interceptionNN paths
 * unconditionally (see src/interceptor.c), even with fewer than 10
 * keyboards or mice attached -- a slot with no bound filter device object
 * (see pnp.c) simply behaves inertly (see dispatch.c / queue.c).
 */

#include "driver.h"

INTERCEPTOR_SLOT g_KeyboardSlot[INTERCEPTOR_MAX_KEYBOARD];
INTERCEPTOR_SLOT g_MouseSlot[INTERCEPTOR_MAX_MOUSE];
FAST_MUTEX g_SlotAllocationLock;
PDRIVER_OBJECT g_DriverObject;

/*
 * Every sample in this repo opens \\.\interceptionNN unelevated with plain
 * GENERIC_READ (see src/interceptor.c, samples/*). IoCreateDevice's default
 * security descriptor does not allow that for non-admin callers, so these
 * are created with an explicit, permissive SDDL instead: authenticated
 * users get read/write, consistent with the README's existing integrity-
 * level caveat (only the same-or-lower integrity level distinction applies,
 * not an elevation requirement).
 */
static const UNICODE_STRING g_ControlDeviceSddl =
    RTL_CONSTANT_STRING(L"D:P(A;;GRGW;;;AU)");

static VOID InterceptorUnload(_In_ PDRIVER_OBJECT DriverObject);

static VOID InterceptorBuildDeviceName(_In_ ULONG DeviceOrdinal, _Out_writes_z_(BufferChars) PWCHAR Buffer,
    _In_ SIZE_T BufferChars, _In_ BOOLEAN Symlink)
{
    if (Symlink)
        RtlStringCchPrintfW(Buffer, BufferChars, L"\\??\\interception%02u", DeviceOrdinal);
    else
        RtlStringCchPrintfW(Buffer, BufferChars, L"\\Device\\interception%02u", DeviceOrdinal);
}

static NTSTATUS InterceptorCreateControlDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ ULONG Index,
    _In_ BOOLEAN IsMouse, _Out_ PINTERCEPTOR_SLOT Slot)
{
    NTSTATUS status;
    WCHAR deviceNameBuffer[64];
    WCHAR symlinkBuffer[64];
    UNICODE_STRING deviceName;
    UNICODE_STRING symlinkName;
    PDEVICE_OBJECT deviceObject;
    PINTERCEPTOR_CONTROL_EXTENSION extension;

    /* interception00..09 = keyboards, interception10..19 = mice -- fixed by
       src/interceptor.c/.h, not renumberable independently of it. */
    ULONG deviceOrdinal = IsMouse ? (INTERCEPTOR_MAX_KEYBOARD + Index) : Index;

    InterceptorBuildDeviceName(deviceOrdinal, deviceNameBuffer, RTL_NUMBER_OF(deviceNameBuffer), FALSE);
    InterceptorBuildDeviceName(deviceOrdinal, symlinkBuffer, RTL_NUMBER_OF(symlinkBuffer), TRUE);

    RtlInitUnicodeString(&deviceName, deviceNameBuffer);
    RtlInitUnicodeString(&symlinkName, symlinkBuffer);

    status = IoCreateDeviceSecure(DriverObject, sizeof(INTERCEPTOR_CONTROL_EXTENSION), &deviceName,
        FILE_DEVICE_UNKNOWN, 0, FALSE, &g_ControlDeviceSddl, NULL, &deviceObject);
    if (!NT_SUCCESS(status)) return status;

    status = IoCreateSymbolicLink(&symlinkName, &deviceName);
    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(deviceObject);
        return status;
    }

    RtlZeroMemory(Slot, sizeof(*Slot));
    Slot->Index = Index;
    Slot->IsMouse = IsMouse;
    Slot->ControlDeviceObject = deviceObject;
    KeInitializeSpinLock(&Slot->Lock);
    InitializeListHead(&Slot->OpenListHead);

    extension = (PINTERCEPTOR_CONTROL_EXTENSION)deviceObject->DeviceExtension;
    extension->Type = InterceptorControlDevice;
    extension->Slot = Slot;

    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}

static VOID InterceptorDestroyControlDevice(_Inout_ PINTERCEPTOR_SLOT Slot, _In_ ULONG DeviceOrdinal)
{
    WCHAR symlinkBuffer[64];
    UNICODE_STRING symlinkName;

    if (Slot->ControlDeviceObject == NULL) return;

    InterceptorBuildDeviceName(DeviceOrdinal, symlinkBuffer, RTL_NUMBER_OF(symlinkBuffer), TRUE);
    RtlInitUnicodeString(&symlinkName, symlinkBuffer);
    IoDeleteSymbolicLink(&symlinkName);

    IoDeleteDevice(Slot->ControlDeviceObject);
    Slot->ControlDeviceObject = NULL;
}

static VOID InterceptorDestroyControlDevices(VOID)
{
    ULONG i;

    for (i = 0; i < INTERCEPTOR_MAX_KEYBOARD; ++i)
        InterceptorDestroyControlDevice(&g_KeyboardSlot[i], i);

    for (i = 0; i < INTERCEPTOR_MAX_MOUSE; ++i)
        InterceptorDestroyControlDevice(&g_MouseSlot[i], INTERCEPTOR_MAX_KEYBOARD + i);
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    g_DriverObject = DriverObject;
    ExInitializeFastMutex(&g_SlotAllocationLock);

    for (i = 0; i < INTERCEPTOR_MAX_KEYBOARD; ++i)
    {
        status = InterceptorCreateControlDevice(DriverObject, i, FALSE, &g_KeyboardSlot[i]);
        if (!NT_SUCCESS(status)) goto failure;
    }

    for (i = 0; i < INTERCEPTOR_MAX_MOUSE; ++i)
    {
        status = InterceptorCreateControlDevice(DriverObject, i, TRUE, &g_MouseSlot[i]);
        if (!NT_SUCCESS(status)) goto failure;
    }

    DriverObject->DriverExtension->AddDevice = InterceptorAddDevice;
    DriverObject->DriverUnload = InterceptorUnload;

    DriverObject->MajorFunction[IRP_MJ_CREATE] = InterceptorCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = InterceptorCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = InterceptorDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = InterceptorInternalDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = InterceptorPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = InterceptorPower;

    return STATUS_SUCCESS;

failure:
    InterceptorDestroyControlDevices();
    return status;
}

static VOID InterceptorUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    /* Any still-attached FiDOs are torn down by Windows sending
       IRP_MN_REMOVE_DEVICE through the stack as part of driver removal
       (handled in pnp.c) before Unload is invoked; only the control devices
       are ours to tear down directly here. */
    InterceptorDestroyControlDevices();
}
