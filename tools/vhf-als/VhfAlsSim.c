#include <ntddk.h>
#include <wdf.h>
#include <hidclass.h>
#include <vhf.h>

#include "AlsReportDescriptor.h"

#define REPORT_ID_INPUT 1
#define REPORT_ID_FEATURE 1
#define FEATURE_PAYLOAD_LENGTH 18

#define SENSOR_EVENT_DATA_UPDATED 4
#define SENSOR_STATE_READY 2
#define SENSOR_REPORTING_ALL_EVENTS 2
#define SENSOR_POWER_D0_FULL 2
#define SENSOR_CONNECTION_PC_ATTACHED 2
#define DEFAULT_REPORT_INTERVAL_MS 250
#define MINIMUM_REPORT_INTERVAL_MS 100

typedef struct _DEVICE_CONTEXT {
    VHFHANDLE VhfHandle;
    WDFTIMER Timer;
    WDFSPINLOCK StateLock;
    ULONG ReportIntervalMs;
    ULONG SampleCount;
    USHORT IlluminanceLux;
    USHORT ChangeSensitivityPercentX100;
    UCHAR ReportingState;
    UCHAR PowerState;
    UCHAR InputReport[5];
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD VhfAlsEvtDeviceAdd;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT VhfAlsEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP VhfAlsEvtSelfManagedIoCleanup;
EVT_WDF_TIMER VhfAlsEvtTimer;
EVT_VHF_ASYNC_OPERATION VhfAlsEvtGetFeature;
EVT_VHF_ASYNC_OPERATION VhfAlsEvtSetFeature;
EVT_VHF_ASYNC_OPERATION VhfAlsEvtGetInputReport;

static VOID
CompleteVhfOperation(
    _In_ VHFOPERATIONHANDLE Operation,
    _In_ NTSTATUS Status
    )
{
    NTSTATUS completeStatus = VhfAsyncOperationComplete(Operation, Status);
    UNREFERENCED_PARAMETER(completeStatus);
}

static NTSTATUS
GetReportPayload(
    _In_ PHID_XFER_PACKET Packet,
    _In_ UCHAR ReportId,
    _In_ ULONG PayloadLength,
    _Outptr_result_bytebuffer_(PayloadLength) PUCHAR* Payload
    )
{
    if (Packet == NULL || Packet->reportBuffer == NULL || Packet->reportId != ReportId) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Packet->reportBufferLen >= PayloadLength + 1) {
        Packet->reportBuffer[0] = ReportId;
        *Payload = Packet->reportBuffer + 1;
        return STATUS_SUCCESS;
    }

    return STATUS_BUFFER_TOO_SMALL;
}

static VOID
BuildInputReport(
    _Inout_ PDEVICE_CONTEXT Context,
    _Out_writes_bytes_(5) PUCHAR ReportBuffer
    )
{
    USHORT lux;
    WdfSpinLockAcquire(Context->StateLock);
    lux = Context->IlluminanceLux;
    WdfSpinLockRelease(Context->StateLock);

    // HID_XFER_PACKET input buffers include the Report ID as byte zero.
    ReportBuffer[0] = REPORT_ID_INPUT;
    ReportBuffer[1] = SENSOR_STATE_READY;
    ReportBuffer[2] = SENSOR_EVENT_DATA_UPDATED;
    ReportBuffer[3] = (UCHAR)(lux & 0xFF);
    ReportBuffer[4] = (UCHAR)(lux >> 8);
}

static VOID
SubmitInputReport(
    _Inout_ PDEVICE_CONTEXT Context
    )
{
    HID_XFER_PACKET packet;

    BuildInputReport(Context, Context->InputReport);
    packet.reportBuffer = Context->InputReport;
    packet.reportBufferLen = sizeof(Context->InputReport);
    packet.reportId = REPORT_ID_INPUT;

    NTSTATUS status = VhfReadReportSubmit(Context->VhfHandle, &packet);
    UNREFERENCED_PARAMETER(status);
}

// Descriptor byte order: ID, reporting, power, interval, connection,
// status, relative sensitivity, minimum interval, sensor state.
VOID VhfAlsEvtGetFeature(PVOID VhfClientContext, VHFOPERATIONHANDLE Operation,
    PVOID OperationContext, PHID_XFER_PACKET Packet)
{
    PDEVICE_CONTEXT context = (PDEVICE_CONTEXT)VhfClientContext;
    PUCHAR payload;
    const ULONG zero = 0, minimum = MINIMUM_REPORT_INTERVAL_MS;
    NTSTATUS status = GetReportPayload(Packet, REPORT_ID_FEATURE, FEATURE_PAYLOAD_LENGTH, &payload);
    UNREFERENCED_PARAMETER(OperationContext);
    if (NT_SUCCESS(status)) {
        RtlZeroMemory(payload, Packet->reportBufferLen - 1);
        WdfSpinLockAcquire(context->StateLock);
        payload[0] = context->ReportingState;
        payload[1] = context->PowerState;
        RtlCopyMemory(payload + 2, &context->ReportIntervalMs, 4);
        payload[6] = SENSOR_CONNECTION_PC_ATTACHED;
        RtlCopyMemory(payload + 7, &zero, 4);
        RtlCopyMemory(payload + 11, &context->ChangeSensitivityPercentX100, 2);
        RtlCopyMemory(payload + 13, &minimum, 4);
        payload[17] = SENSOR_STATE_READY;
        WdfSpinLockRelease(context->StateLock);
    }
    CompleteVhfOperation(Operation, status);
}

VOID VhfAlsEvtSetFeature(PVOID VhfClientContext, VHFOPERATIONHANDLE Operation,
    PVOID OperationContext, PHID_XFER_PACKET Packet)
{
    PDEVICE_CONTEXT context = (PDEVICE_CONTEXT)VhfClientContext;
    PUCHAR payload;
    ULONG interval;
    USHORT sensitivity;
    NTSTATUS status = GetReportPayload(Packet, REPORT_ID_FEATURE, FEATURE_PAYLOAD_LENGTH, &payload);
    UNREFERENCED_PARAMETER(OperationContext);
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(&interval, payload + 2, 4);
        RtlCopyMemory(&sensitivity, payload + 11, 2);
        if (payload[0] < 1 || payload[0] > 6 || payload[1] < 1 || payload[1] > 6 || sensitivity > 10000) {
            status = STATUS_INVALID_PARAMETER;
        } else {
            WdfSpinLockAcquire(context->StateLock);
            context->ReportingState = payload[0];
            context->PowerState = payload[1];
            context->ReportIntervalMs = max(interval, MINIMUM_REPORT_INTERVAL_MS);
            context->ChangeSensitivityPercentX100 = sensitivity;
            WdfSpinLockRelease(context->StateLock);
        }
    }
    CompleteVhfOperation(Operation, status);
}

VOID
VhfAlsEvtGetInputReport(
    _In_ PVOID VhfClientContext,
    _In_ VHFOPERATIONHANDLE VhfOperationHandle,
    _In_opt_ PVOID VhfOperationContext,
    _In_ PHID_XFER_PACKET HidTransferPacket
    )
{
    PDEVICE_CONTEXT context = (PDEVICE_CONTEXT)VhfClientContext;
    PUCHAR payload = NULL;
    NTSTATUS status;
    UCHAR report[5];

    UNREFERENCED_PARAMETER(VhfOperationContext);
    status = GetReportPayload(HidTransferPacket, REPORT_ID_INPUT, 4, &payload);
    if (NT_SUCCESS(status)) {
        BuildInputReport(context, report);
        if (HidTransferPacket->reportBufferLen == sizeof(report)) {
            RtlCopyMemory(HidTransferPacket->reportBuffer, report, sizeof(report));
        } else {
            RtlCopyMemory(payload, report + 1, 4);
        }
    }

    CompleteVhfOperation(VhfOperationHandle, status);
}

VOID
VhfAlsEvtTimer(
    _In_ WDFTIMER Timer
    )
{
    PDEVICE_CONTEXT context = GetDeviceContext(WdfTimerGetParentObject(Timer));
    ULONG interval;
    UCHAR reporting;
    UCHAR power;

    WdfSpinLockAcquire(context->StateLock);
    context->SampleCount++;
    context->IlluminanceLux = ((context->SampleCount / 10) & 1) ? 500 : 50;
    interval = context->ReportIntervalMs;
    reporting = context->ReportingState;
    power = context->PowerState;
    WdfSpinLockRelease(context->StateLock);

    if ((reporting == 2 || reporting == 3 || reporting == 5 || reporting == 6) && power == SENSOR_POWER_D0_FULL) {
        SubmitInputReport(context);
    }

    WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(interval));
}

NTSTATUS
VhfAlsEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_OBJECT_ATTRIBUTES lockAttributes;
    WDF_OBJECT_ATTRIBUTES timerAttributes;
    WDF_TIMER_CONFIG timerConfig;
    PDEVICE_CONTEXT context;
    VHF_CONFIG vhfConfig;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDeviceSelfManagedIoInit = VhfAlsEvtSelfManagedIoInit;
    pnpCallbacks.EvtDeviceSelfManagedIoCleanup = VhfAlsEvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);
    WdfDeviceInitSetCharacteristics(DeviceInit, FILE_DEVICE_SECURE_OPEN, FALSE);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    context = GetDeviceContext(device);
    RtlZeroMemory(context, sizeof(*context));
    context->ReportIntervalMs = DEFAULT_REPORT_INTERVAL_MS;
    context->IlluminanceLux = 50;
    context->ChangeSensitivityPercentX100 = 2500;
    context->ReportingState = SENSOR_REPORTING_ALL_EVENTS;
    context->PowerState = SENSOR_POWER_D0_FULL;

    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
    lockAttributes.ParentObject = device;
    status = WdfSpinLockCreate(&lockAttributes, &context->StateLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_TIMER_CONFIG_INIT(&timerConfig, VhfAlsEvtTimer);
    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    timerAttributes.ParentObject = device;
    status = WdfTimerCreate(&timerConfig, &timerAttributes, &context->Timer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    VHF_CONFIG_INIT(
        &vhfConfig,
        WdfDeviceWdmGetDeviceObject(device),
        g_AlsReportDescriptorLength,
        (PUCHAR)g_AlsReportDescriptor
        );
    vhfConfig.VhfClientContext = context;
    vhfConfig.VendorID = 0x1209;
    vhfConfig.ProductID = 0xA105;
    vhfConfig.VersionNumber = 0x0001;
    vhfConfig.EvtVhfAsyncOperationGetFeature = VhfAlsEvtGetFeature;
    vhfConfig.EvtVhfAsyncOperationSetFeature = VhfAlsEvtSetFeature;
    vhfConfig.EvtVhfAsyncOperationGetInputReport = VhfAlsEvtGetInputReport;

    status = VhfCreate(&vhfConfig, &context->VhfHandle);
    return status;
}

NTSTATUS
VhfAlsEvtSelfManagedIoInit(
    _In_ WDFDEVICE Device
    )
{
    PDEVICE_CONTEXT context = GetDeviceContext(Device);
    NTSTATUS status = VhfStart(context->VhfHandle);
    if (NT_SUCCESS(status)) {
        WdfTimerStart(context->Timer, WDF_REL_TIMEOUT_IN_MS(1));
    }
    return status;
}

VOID
VhfAlsEvtSelfManagedIoCleanup(
    _In_ WDFDEVICE Device
    )
{
    PDEVICE_CONTEXT context = GetDeviceContext(Device);
    WdfTimerStop(context->Timer, TRUE);
    if (context->VhfHandle != NULL) {
        VhfDelete(context->VhfHandle, TRUE);
        context->VhfHandle = NULL;
    }
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, VhfAlsEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
        &config, WDF_NO_HANDLE);
}
