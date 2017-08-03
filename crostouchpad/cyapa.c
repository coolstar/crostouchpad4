#define DESCRIPTOR_DEF
#include <driver.h>

#define bool int

static ULONG CyapaDebugLevel = 100;
static ULONG CyapaDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

static bool deviceLoaded = false;

static int sqr(int num) {
	return num * num;
}

static int diffsig(int x, int y, int lastx, int lasty) {
	uint32_t distsq = sqr(x - lastx) + sqr(y - lasty);
	if (distsq < 4)
		return 0;
	return 1;
}

NTSTATUS
DriverEntry(
__in PDRIVER_OBJECT  DriverObject,
__in PUNICODE_STRING RegistryPath
)
{
	NTSTATUS               status = STATUS_SUCCESS;
	WDF_DRIVER_CONFIG      config;
	WDF_OBJECT_ATTRIBUTES  attributes;

	CyapaPrint(DEBUG_LEVEL_INFO, DBG_INIT,
		"Driver Entry\n");

	WDF_DRIVER_CONFIG_INIT(&config, CyapaEvtDeviceAdd);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

	//
	// Create a framework driver object to represent our driver.
	//

	status = WdfDriverCreate(DriverObject,
		RegistryPath,
		&attributes,
		&config,
		WDF_NO_HANDLE
		);

	if (!NT_SUCCESS(status))
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_INIT,
			"WdfDriverCreate failed with status 0x%x\n", status);
	}

	return status;
}

void cyapa_set_power_mode(_In_  PCYAPA_CONTEXT  pDevice, _In_ uint8_t power_mode)
{
	int ret;
	uint8_t power;

	SpbReadDataSynchronously(&pDevice->I2CContext, CMD_POWER_MODE, &ret, 1);
	if (ret < 0)
		return;

	power = (ret & ~0xFC);
	power |= power_mode & 0xFc;

	SpbWriteDataSynchronously(&pDevice->I2CContext, CMD_POWER_MODE, &power, 1);
}

VOID
CyapaBootWorkItem(
	IN WDFWORKITEM  WorkItem
)
{
	WDFDEVICE Device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
	PCYAPA_CONTEXT pDevice = GetDeviceContext(Device);

	struct cyapa_cap cap;
	SpbReadDataSynchronously(&pDevice->I2CContext, CMD_QUERY_CAPABILITIES, &cap, sizeof(cap));
	if (strncmp((const char *)cap.prod_ida, "CYTRA", 5) != 0) {
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "[cyapainit] Product ID \"%5.5s\" mismatch\n",
			cap.prod_ida);
		SpbReadDataSynchronously(&pDevice->I2CContext, CMD_QUERY_CAPABILITIES, &cap, sizeof(cap));
	}

	pDevice->max_x = ((cap.max_abs_xy_high << 4) & 0x0F00) |
		cap.max_abs_x_low;
	pDevice->max_y = ((cap.max_abs_xy_high << 8) & 0x0F00) |
		cap.max_abs_y_low;

	pDevice->phy_x = ((cap.phy_siz_xy_high << 4) & 0x0F00) |
		cap.phy_siz_x_low;
	pDevice->phy_y = ((cap.phy_siz_xy_high << 8) & 0x0F00) |
		cap.phy_siz_y_low;

	CyapaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "[cyapainit] %5.5s-%6.6s-%2.2s buttons=%c%c%c res=%dx%d\n",
		cap.prod_ida, cap.prod_idb, cap.prod_idc,
		((cap.buttons & CYAPA_FNGR_LEFT) ? 'L' : '-'),
		((cap.buttons & CYAPA_FNGR_MIDDLE) ? 'M' : '-'),
		((cap.buttons & CYAPA_FNGR_RIGHT) ? 'R' : '-'),
		pDevice->max_x,
		pDevice->max_y);

	uint16_t max_x[] = { pDevice->max_x };
	uint16_t max_y[] = { pDevice->max_y };

	uint8_t *max_x8bit = (uint8_t *)max_x;
	uint8_t *max_y8bit = (uint8_t *)max_y;

	pDevice->max_x_hid[0] = max_x8bit[0];
	pDevice->max_x_hid[1] = max_x8bit[1];

	pDevice->max_y_hid[0] = max_y8bit[0];
	pDevice->max_y_hid[1] = max_y8bit[1];


	uint16_t phy_x[] = { pDevice->phy_x * 10 };
	uint16_t phy_y[] = { pDevice->phy_y * 10 };

	uint8_t *phy_x8bit = (uint8_t *)phy_x;
	uint8_t *phy_y8bit = (uint8_t *)phy_y;

	pDevice->phy_x_hid[0] = phy_x8bit[0];
	pDevice->phy_x_hid[1] = phy_x8bit[1];

	pDevice->phy_y_hid[0] = phy_y8bit[0];
	pDevice->phy_y_hid[1] = phy_y8bit[1];

	cyapa_set_power_mode(pDevice, CMD_POWER_MODE_FULL);
	WdfObjectDelete(WorkItem);
}

void CyapaBootTimer(_In_ WDFTIMER hTimer) {
	WDFDEVICE Device = (WDFDEVICE)WdfTimerGetParentObject(hTimer);
	PCYAPA_CONTEXT pDevice = GetDeviceContext(Device);

	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_WORKITEM_CONFIG workitemConfig;
	WDFWORKITEM hWorkItem;

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&attributes, CYAPA_CONTEXT);
	attributes.ParentObject = Device;
	WDF_WORKITEM_CONFIG_INIT(&workitemConfig, CyapaBootWorkItem);

	WdfWorkItemCreate(&workitemConfig,
		&attributes,
		&hWorkItem);

	WdfWorkItemEnqueue(hWorkItem);
	WdfTimerStop(hTimer, FALSE);
}

NTSTATUS BOOTTRACKPAD(
	_In_  PCYAPA_CONTEXT  pDevice
	)
{
	NTSTATUS status = 0;

	static char bl_exit[] = {
		0x00, 0xff, 0xa5, 0x00, 0x01,
		0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };

	static char bl_deactivate[] = {
		0x00, 0xff, 0x3b, 0x00, 0x01,
		0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };

	struct cyapa_boot_regs boot;

	SpbReadDataSynchronously(&pDevice->I2CContext, CMD_BOOT_STATUS, &boot, sizeof(boot));

	if ((boot.stat & CYAPA_STAT_RUNNING) == 0) {
		if (boot.error & CYAPA_ERROR_BOOTLOADER)
			SpbWriteDataSynchronously(&pDevice->I2CContext, CMD_BOOT_STATUS, bl_deactivate, sizeof(bl_deactivate));
		else
			SpbWriteDataSynchronously(&pDevice->I2CContext, CMD_BOOT_STATUS, bl_exit, sizeof(bl_exit));
	}

	WDF_TIMER_CONFIG              timerConfig;
	WDFTIMER                      hTimer;
	WDF_OBJECT_ATTRIBUTES         attributes;

	WDF_TIMER_CONFIG_INIT(&timerConfig, CyapaBootTimer);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = pDevice->FxDevice;
	status = WdfTimerCreate(&timerConfig, &attributes, &hTimer);

	WdfTimerStart(hTimer, WDF_REL_TIMEOUT_IN_MS(75));

	return status;
}

NTSTATUS
OnPrepareHardware(
_In_  WDFDEVICE     FxDevice,
_In_  WDFCMRESLIST  FxResourcesRaw,
_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

This routine caches the SPB resource connection ID.

Arguments:

FxDevice - a handle to the framework device object
FxResourcesRaw - list of translated hardware resources that
the PnP manager has assigned to the device
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PCYAPA_CONTEXT pDevice = GetDeviceContext(FxDevice);
	BOOLEAN fSpbResourceFound = FALSE;
	NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;

	UNREFERENCED_PARAMETER(FxResourcesRaw);

	//
	// Parse the peripheral's resources.
	//

	ULONG resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);

	for (ULONG i = 0; i < resourceCount; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR pDescriptor;
		UCHAR Class;
		UCHAR Type;

		pDescriptor = WdfCmResourceListGetDescriptor(
			FxResourcesTranslated, i);

		switch (pDescriptor->Type)
		{
		case CmResourceTypeConnection:
			//
			// Look for I2C or SPI resource and save connection ID.
			//
			Class = pDescriptor->u.Connection.Class;
			Type = pDescriptor->u.Connection.Type;
			if (Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
				Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
			{
				if (fSpbResourceFound == FALSE)
				{
					status = STATUS_SUCCESS;
					pDevice->I2CContext.I2cResHubId.LowPart = pDescriptor->u.Connection.IdLowPart;
					pDevice->I2CContext.I2cResHubId.HighPart = pDescriptor->u.Connection.IdHighPart;
					fSpbResourceFound = TRUE;
				}
				else
				{
				}
			}
			break;
		default:
			//
			// Ignoring all other resource types.
			//
			break;
		}
	}

	//
	// An SPB resource is required.
	//

	if (fSpbResourceFound == FALSE)
	{
		status = STATUS_NOT_FOUND;
	}

	status = SpbTargetInitialize(FxDevice, &pDevice->I2CContext);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	return status;
}

NTSTATUS
OnReleaseHardware(
_In_  WDFDEVICE     FxDevice,
_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

Arguments:

FxDevice - a handle to the framework device object
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PCYAPA_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	UNREFERENCED_PARAMETER(FxResourcesTranslated);

	SpbTargetDeinitialize(FxDevice, &pDevice->I2CContext);

	return status;
}

NTSTATUS
OnD0Entry(
_In_  WDFDEVICE               FxDevice,
_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine allocates objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PCYAPA_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;
	
	cyapa_set_power_mode(pDevice, CMD_POWER_MODE_FULL);

	WdfTimerStart(pDevice->Timer, WDF_REL_TIMEOUT_IN_MS(10));

	for (int i = 0; i < 20; i++){
		pDevice->Flags[i] = 0;
	}

	pDevice->RegsSet = false;
	pDevice->ConnectInterrupt = true;

	BOOTTRACKPAD(pDevice);

	return status;
}

NTSTATUS
OnD0Exit(
_In_  WDFDEVICE               FxDevice,
_In_  WDF_POWER_DEVICE_STATE  FxTargetState
)
/*++

Routine Description:

This routine destroys objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxTargetState - the target power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxTargetState);

	PCYAPA_CONTEXT pDevice = GetDeviceContext(FxDevice);
	
	WdfTimerStop(pDevice->Timer, TRUE);

	pDevice->ConnectInterrupt = false;

	return STATUS_SUCCESS;
}

BOOLEAN OnInterruptIsr(
	WDFINTERRUPT Interrupt,
	ULONG MessageID){
	UNREFERENCED_PARAMETER(MessageID);

	WDFDEVICE Device = WdfInterruptGetDevice(Interrupt);
	PCYAPA_CONTEXT pDevice = GetDeviceContext(Device);

	if (!pDevice->ConnectInterrupt)
		return true;

	LARGE_INTEGER CurrentTime;

	KeQuerySystemTime(&CurrentTime);

	LARGE_INTEGER DIFF;

	DIFF.QuadPart = 0;

	if (pDevice->LastTime.QuadPart != 0)
		DIFF.QuadPart = (CurrentTime.QuadPart - pDevice->LastTime.QuadPart) / 1000;

	struct cyapa_regs rawregs;
	SpbReadDataSynchronously(&pDevice->I2CContext, 0, &rawregs, sizeof(rawregs));
	struct cyapa_regs *regs = &rawregs;

	struct _CYAPA_MULTITOUCH_REPORT report;
	report.ReportID = REPORTID_MTOUCH;

	int nfingers;

	nfingers = CYAPA_FNGR_NUMFINGERS(regs->fngr);
	
	int x[15];
	int y[15];
	int p[15];
	for (int i = 0; i < 15; i++) {
		x[i] = -1;
		y[i] = -1;
		p[i] = -1;
	}
	for (int i = 0; i < nfingers; i++) {
		int a = regs->touch[i].id;
		int rawx = CYAPA_TOUCH_X(regs, i);
		int rawy = CYAPA_TOUCH_Y(regs, i);
		int rawp = CYAPA_TOUCH_P(regs, i);
		x[a] = rawx;
		y[a] = rawy;
		p[a] = rawp;
	}
	for (int i = 0; i < 15; i++) {
		if (pDevice->Flags[i] == MXT_T9_DETECT && x[i] == -1) {
			pDevice->Flags[i] = MXT_T9_RELEASE;
		}
		if (x[i] != -1) {
			bool updateValues = false;

			if (pDevice->Flags[i] == 0 || pDevice->Flags[i] == MXT_T9_RELEASE)
				updateValues = true;

			pDevice->Flags[i] = MXT_T9_DETECT;
			
			if (diffsig(x[i], y[i], pDevice->XValue[i], pDevice->YValue[i]) == 1)
				updateValues = true;

			if (updateValues) {
				pDevice->XValue[i] = x[i];
				pDevice->YValue[i] = y[i];
				pDevice->PValue[i] = p[i];
			}
		}
	}

	pDevice->BUTTONPRESSED = ((regs->fngr & CYAPA_FNGR_LEFT) != 0);

	pDevice->TIMEINT += DIFF.QuadPart;

	pDevice->LastTime = CurrentTime;

	int count = 0, i = 0;
	while (count < 5 && i < 15) {
		if (pDevice->Flags[i] != 0) {
			report.Touch[count].ContactID = i;

			report.Touch[count].XValue = pDevice->XValue[i];
			report.Touch[count].YValue = pDevice->YValue[i];
			report.Touch[count].Pressure = pDevice->PValue[i];

			uint8_t flags = pDevice->Flags[i];
			if (flags & MXT_T9_DETECT) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_PRESS) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_RELEASE) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT;
				pDevice->Flags[i] = 0;
			}
			else
				report.Touch[count].Status = 0;

			count++;
		}
		i++;
	}

	report.ScanTime = pDevice->TIMEINT;
	report.IsDepressed = pDevice->BUTTONPRESSED;

	report.ContactCount = count;

	size_t bytesWritten;
	CyapaProcessVendorReport(pDevice, &report, sizeof(report), &bytesWritten);

	pDevice->RegsSet = true;
	return true;
}

VOID
CyapaReadWriteWorkItem(
	IN WDFWORKITEM  WorkItem
	)
{
	WDFDEVICE Device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
	PCYAPA_CONTEXT pDevice = GetDeviceContext(Device);

	WdfObjectDelete(WorkItem);

	if (!pDevice->ConnectInterrupt)
		return;

	struct _CYAPA_MULTITOUCH_REPORT report;
	report.ReportID = REPORTID_MTOUCH;

	LARGE_INTEGER CurrentTime;

	KeQuerySystemTimePrecise(&CurrentTime);

	LARGE_INTEGER DIFF;

	DIFF.QuadPart = 0;

	if (pDevice->LastTime.QuadPart != 0)
		DIFF.QuadPart = (CurrentTime.QuadPart - pDevice->LastTime.QuadPart) / 500;

	pDevice->TIMEINT += DIFF.QuadPart;

	pDevice->LastTime = CurrentTime;

	int count = 0, i = 0;
	while (count < 5 && i < 15) {
		if (pDevice->Flags[i] != 0) {
			report.Touch[count].ContactID = i;

			report.Touch[count].XValue = pDevice->XValue[i];
			report.Touch[count].YValue = pDevice->YValue[i];
			report.Touch[count].Pressure = pDevice->PValue[i];

			uint8_t flags = pDevice->Flags[i];
			if (flags & MXT_T9_DETECT) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_PRESS) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_RELEASE) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT;
				pDevice->Flags[i] = 0;
			}
			else
				report.Touch[count].Status = 0;

			count++;
		}
		i++;
	}

	report.ScanTime = pDevice->TIMEINT;
	report.IsDepressed = pDevice->BUTTONPRESSED;

	report.ContactCount = count;

	size_t bytesWritten;
	CyapaProcessVendorReport(pDevice, &report, sizeof(report), &bytesWritten);
}

void CyapaTimerFunc(_In_ WDFTIMER hTimer){
	return;
	WDFDEVICE Device = (WDFDEVICE)WdfTimerGetParentObject(hTimer);
	PCYAPA_CONTEXT pDevice = GetDeviceContext(Device);

	if (!pDevice->ConnectInterrupt)
		return;

	/*if (!pDevice->RegsSet)
		return;*/

	PCYAPA_CONTEXT context;
	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_WORKITEM_CONFIG workitemConfig;
	WDFWORKITEM hWorkItem;

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&attributes, CYAPA_CONTEXT);
	attributes.ParentObject = Device;
	WDF_WORKITEM_CONFIG_INIT(&workitemConfig, CyapaReadWriteWorkItem);

	WdfWorkItemCreate(&workitemConfig,
		&attributes,
		&hWorkItem);

	WdfWorkItemEnqueue(hWorkItem);

	return;
}

NTSTATUS
CyapaEvtDeviceAdd(
IN WDFDRIVER       Driver,
IN PWDFDEVICE_INIT DeviceInit
)
{
	NTSTATUS                      status = STATUS_SUCCESS;
	WDF_IO_QUEUE_CONFIG           queueConfig;
	WDF_OBJECT_ATTRIBUTES         attributes;
	WDFDEVICE                     device;
	WDF_INTERRUPT_CONFIG interruptConfig;
	WDFQUEUE                      queue;
	UCHAR                         minorFunction;
	PCYAPA_CONTEXT               devContext;

	UNREFERENCED_PARAMETER(Driver);

	PAGED_CODE();

	CyapaPrint(DEBUG_LEVEL_INFO, DBG_PNP,
		"CyapaEvtDeviceAdd called\n");

	//
	// Tell framework this is a filter driver. Filter drivers by default are  
	// not power policy owners. This works well for this driver because
	// HIDclass driver is the power policy owner for HID minidrivers.
	//

	WdfFdoInitSetFilter(DeviceInit);

	{
		WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
		WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

		pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
		pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
		pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
		pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;

		WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);
	}

	//
	// Because we are a virtual device the root enumerator would just put null values 
	// in response to IRP_MN_QUERY_ID. Lets override that.
	//

	minorFunction = IRP_MN_QUERY_ID;

	status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
		DeviceInit,
		CyapaEvtWdmPreprocessMnQueryId,
		IRP_MJ_PNP,
		&minorFunction,
		1
		);
	if (!NT_SUCCESS(status))
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfDeviceInitAssignWdmIrpPreprocessCallback failed Status 0x%x\n", status);

		return status;
	}

	//
	// Setup the device context
	//

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, CYAPA_CONTEXT);

	//
	// Create a framework device object.This call will in turn create
	// a WDM device object, attach to the lower stack, and set the
	// appropriate flags and attributes.
	//

	status = WdfDeviceCreate(&DeviceInit, &attributes, &device);

	if (!NT_SUCCESS(status))
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfDeviceCreate failed with status code 0x%x\n", status);

		return status;
	}

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);

	queueConfig.EvtIoInternalDeviceControl = CyapaEvtInternalDeviceControl;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&queue
		);

	if (!NT_SUCCESS(status))
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create manual I/O queue to take care of hid report read requests
	//

	devContext = GetDeviceContext(device);

	WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

	queueConfig.PowerManaged = WdfFalse;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->ReportQueue
		);

	if (!NT_SUCCESS(status))
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create an interrupt object for hardware notifications
	//
	WDF_INTERRUPT_CONFIG_INIT(
		&interruptConfig,
		OnInterruptIsr,
		NULL);
	interruptConfig.PassiveHandling = TRUE;

	status = WdfInterruptCreate(
		device,
		&interruptConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->Interrupt);

	if (!NT_SUCCESS(status))
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"Error creating WDF interrupt object - %!STATUS!",
			status);

		return status;
	}

	WDF_TIMER_CONFIG              timerConfig;
	WDFTIMER                      hTimer;

	WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, CyapaTimerFunc, 10);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = device;
	status = WdfTimerCreate(&timerConfig, &attributes, &hTimer);
	devContext->Timer = hTimer;
	if (!NT_SUCCESS(status))
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "(%!FUNC!) WdfTimerCreate failed status:%!STATUS!\n", status);
		return status;
	}

	//
	// Initialize DeviceMode
	//

	devContext->DeviceMode = DEVICE_MODE_MOUSE;
	devContext->FxDevice = device;

	return status;
}

NTSTATUS
CyapaEvtWdmPreprocessMnQueryId(
WDFDEVICE Device,
PIRP Irp
)
{
	NTSTATUS            status;
	PIO_STACK_LOCATION  IrpStack, previousSp;
	PDEVICE_OBJECT      DeviceObject;
	PWCHAR              buffer;

	PAGED_CODE();

	//
	// Get a pointer to the current location in the Irp
	//

	IrpStack = IoGetCurrentIrpStackLocation(Irp);

	//
	// Get the device object
	//
	DeviceObject = WdfDeviceWdmGetDeviceObject(Device);


	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_PNP,
		"CyapaEvtWdmPreprocessMnQueryId Entry\n");

	//
	// This check is required to filter out QUERY_IDs forwarded
	// by the HIDCLASS for the parent FDO. These IDs are sent
	// by PNP manager for the parent FDO if you root-enumerate this driver.
	//
	previousSp = ((PIO_STACK_LOCATION)((UCHAR *)(IrpStack)+
		sizeof(IO_STACK_LOCATION)));

	if (previousSp->DeviceObject == DeviceObject)
	{
		//
		// Filtering out this basically prevents the Found New Hardware
		// popup for the root-enumerated Cyapa on reboot.
		//
		status = Irp->IoStatus.Status;
	}
	else
	{
		switch (IrpStack->Parameters.QueryId.IdType)
		{
		case BusQueryDeviceID:
		case BusQueryHardwareIDs:
			//
			// HIDClass is asking for child deviceid & hardwareids.
			// Let us just make up some id for our child device.
			//
			buffer = (PWCHAR)ExAllocatePoolWithTag(
				NonPagedPool,
				CYAPA_HARDWARE_IDS_LENGTH,
				CYAPA_POOL_TAG
				);

			if (buffer)
			{
				//
				// Do the copy, store the buffer in the Irp
				//
				RtlCopyMemory(buffer,
					CYAPA_HARDWARE_IDS,
					CYAPA_HARDWARE_IDS_LENGTH
					);

				Irp->IoStatus.Information = (ULONG_PTR)buffer;
				status = STATUS_SUCCESS;
			}
			else
			{
				//
				//  No memory
				//
				status = STATUS_INSUFFICIENT_RESOURCES;
			}

			Irp->IoStatus.Status = status;
			//
			// We don't need to forward this to our bus. This query
			// is for our child so we should complete it right here.
			// fallthru.
			//
			IoCompleteRequest(Irp, IO_NO_INCREMENT);

			break;

		default:
			status = Irp->IoStatus.Status;
			IoCompleteRequest(Irp, IO_NO_INCREMENT);
			break;
		}
	}

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaEvtWdmPreprocessMnQueryId Exit = 0x%x\n", status);

	return status;
}

VOID
CyapaEvtInternalDeviceControl(
IN WDFQUEUE     Queue,
IN WDFREQUEST   Request,
IN size_t       OutputBufferLength,
IN size_t       InputBufferLength,
IN ULONG        IoControlCode
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	WDFDEVICE           device;
	PCYAPA_CONTEXT     devContext;
	BOOLEAN             completeRequest = TRUE;

	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	device = WdfIoQueueGetDevice(Queue);
	devContext = GetDeviceContext(device);

	CyapaPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
		"%s, Queue:0x%p, Request:0x%p\n",
		DbgHidInternalIoctlString(IoControlCode),
		Queue,
		Request
		);

	//
	// Please note that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl. So depending on the ioctl code, we will either
	// use retreive function or escape to WDM to get the UserBuffer.
	//

	switch (IoControlCode)
	{

	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
		//
		// Retrieves the device's HID descriptor.
		//
		status = CyapaGetHidDescriptor(device, Request);
		break;

	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
		//
		//Retrieves a device's attributes in a HID_DEVICE_ATTRIBUTES structure.
		//
		status = CyapaGetDeviceAttributes(Request);
		break;

	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		//
		//Obtains the report descriptor for the HID device.
		//
		status = CyapaGetReportDescriptor(device, Request, FALSE, &completeRequest);
		break;

	case IOCTL_HID_GET_STRING:
		//
		// Requests that the HID minidriver retrieve a human-readable string
		// for either the manufacturer ID, the product ID, or the serial number
		// from the string descriptor of the device. The minidriver must send
		// a Get String Descriptor request to the device, in order to retrieve
		// the string descriptor, then it must extract the string at the
		// appropriate index from the string descriptor and return it in the
		// output buffer indicated by the IRP. Before sending the Get String
		// Descriptor request, the minidriver must retrieve the appropriate
		// index for the manufacturer ID, the product ID or the serial number
		// from the device extension of a top level collection associated with
		// the device.
		//
		status = CyapaGetString(Request);
		break;

	case IOCTL_HID_WRITE_REPORT:
	case IOCTL_HID_SET_OUTPUT_REPORT:
		//
		//Transmits a class driver-supplied report to the device.
		//
		status = CyapaWriteReport(devContext, Request);
		break;

	case IOCTL_HID_READ_REPORT:
	case IOCTL_HID_GET_INPUT_REPORT:
		//
		// Returns a report from the device into a class driver-supplied buffer.
		// 
		status = CyapaReadReport(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_SET_FEATURE:
		//
		// This sends a HID class feature report to a top-level collection of
		// a HID class device.
		//
		status = CyapaSetFeature(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_GET_FEATURE:
		//
		// returns a feature report associated with a top-level collection
		//
		status = CyapaGetFeature(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_ACTIVATE_DEVICE:
		//
		// Makes the device ready for I/O operations.
		//
	case IOCTL_HID_DEACTIVATE_DEVICE:
		//
		// Causes the device to cease operations and terminate all outstanding
		// I/O requests.
		//
	default:
		status = STATUS_NOT_SUPPORTED;
		break;
	}

	if (completeRequest)
	{
		WdfRequestComplete(Request, status);

		CyapaPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"%s completed, Queue:0x%p, Request:0x%p\n",
			DbgHidInternalIoctlString(IoControlCode),
			Queue,
			Request
			);
	}
	else
	{
		CyapaPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"%s deferred, Queue:0x%p, Request:0x%p\n",
			DbgHidInternalIoctlString(IoControlCode),
			Queue,
			Request
			);
	}

	return;
}

NTSTATUS
CyapaGetHidDescriptor(
IN WDFDEVICE Device,
IN WDFREQUEST Request
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	size_t              bytesToCopy = 0;
	WDFMEMORY           memory;

	UNREFERENCED_PARAMETER(Device);

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaGetHidDescriptor Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputMemory(Request, &memory);

	if (!NT_SUCCESS(status))
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputMemory failed 0x%x\n", status);

		return status;
	}

	//
	// Use hardcoded "HID Descriptor" 
	//
	bytesToCopy = DefaultHidDescriptor.bLength;

	if (bytesToCopy == 0)
	{
		status = STATUS_INVALID_DEVICE_STATE;

		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"DefaultHidDescriptor is zero, 0x%x\n", status);

		return status;
	}

	status = WdfMemoryCopyFromBuffer(memory,
		0, // Offset
		(PVOID)&DefaultHidDescriptor,
		bytesToCopy);

	if (!NT_SUCCESS(status))
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfMemoryCopyFromBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, bytesToCopy);

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaGetHidDescriptor Exit = 0x%x\n", status);

	return status;
}

void CyapaDescriptorTimer(_In_ WDFTIMER hTimer) {
	WDFDEVICE Device = (WDFDEVICE)WdfTimerGetParentObject(hTimer);
	PCYAPA_CONTEXT pDevice = GetDeviceContext(Device);

	WdfRequestForwardToIoQueue(pDevice->lastRequest, pDevice->ReportQueue);

	pDevice->lastRequest = NULL;

	WdfTimerStop(hTimer, FALSE);
}

NTSTATUS
CyapaGetReportDescriptor(
	IN WDFDEVICE Device,
	IN WDFREQUEST Request,
	IN BOOLEAN Retried,
	OUT BOOLEAN* CompleteRequest
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	ULONG_PTR           bytesToCopy;
	WDFMEMORY           memory;

	PCYAPA_CONTEXT devContext = GetDeviceContext(Device);

	if (devContext->max_x == 0) {
		devContext->lastRequest = Request;
		WDF_TIMER_CONFIG              timerConfig;
		WDFTIMER                      hTimer;
		WDF_OBJECT_ATTRIBUTES         attributes;

		WDF_TIMER_CONFIG_INIT(&timerConfig, CyapaDescriptorTimer);

		WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
		attributes.ParentObject = Device;
		status = WdfTimerCreate(&timerConfig, &attributes, &hTimer);

		WdfTimerStart(hTimer, WDF_REL_TIMEOUT_IN_MS(75));
		*CompleteRequest = FALSE;
	}

	UNREFERENCED_PARAMETER(Device);

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaGetReportDescriptor Entry\n");

#define MT_TOUCH_COLLECTION												\
			MT_TOUCH_COLLECTION0 \
			0x26, devContext->max_x_hid[0], devContext->max_x_hid[1],                   /*       LOGICAL_MAXIMUM (WIDTH)    */ \
			0x46, devContext->phy_x_hid[0], devContext->phy_x_hid[1],                   /*       PHYSICAL_MAXIMUM (WIDTH)   */ \
			MT_TOUCH_COLLECTION1 \
			0x26, devContext->max_y_hid[0], devContext->max_y_hid[1],                   /*       LOGICAL_MAXIMUM (HEIGHT)    */ \
			0x46, devContext->phy_y_hid[0], devContext->phy_y_hid[1],                   /*       PHYSICAL_MAXIMUM (HEIGHT)   */ \
			MT_TOUCH_COLLECTION2

	HID_REPORT_DESCRIPTOR ReportDescriptor[] = {
		//
		// Multitouch report starts here
		//
		//TOUCH PAD input TLC
		0x05, 0x0d,                         // USAGE_PAGE (Digitizers)          
		0x09, 0x05,                         // USAGE (Touch Pad)             
		0xa1, 0x01,                         // COLLECTION (Application)         
		0x85, REPORTID_MTOUCH,            //   REPORT_ID (Touch pad)              
		0x09, 0x22,                         //   USAGE (Finger)                 
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		USAGE_PAGES
	};

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputMemory(Request, &memory);
	if (!NT_SUCCESS(status))
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputMemory failed 0x%x\n", status);

		return status;
	}

	//
	// Use hardcoded Report descriptor
	//
	bytesToCopy = DefaultHidDescriptor.DescriptorList[0].wReportLength;

	if (bytesToCopy == 0)
	{
		status = STATUS_INVALID_DEVICE_STATE;

		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"DefaultHidDescriptor's reportLength is zero, 0x%x\n", status);

		return status;
	}

	status = WdfMemoryCopyFromBuffer(memory,
		0,
		(PVOID)ReportDescriptor,
		bytesToCopy);
	if (!NT_SUCCESS(status))
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfMemoryCopyFromBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, bytesToCopy);

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaGetReportDescriptor Exit = 0x%x\n", status);

	if (Retried) {
		WdfRequestComplete(Request, status);
	}

	return status;
}


NTSTATUS
CyapaGetDeviceAttributes(
IN WDFREQUEST Request
)
{
	NTSTATUS                 status = STATUS_SUCCESS;
	PHID_DEVICE_ATTRIBUTES   deviceAttributes = NULL;

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaGetDeviceAttributes Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputBuffer(Request,
		sizeof(HID_DEVICE_ATTRIBUTES),
		&deviceAttributes,
		NULL);
	if (!NT_SUCCESS(status))
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Set USB device descriptor
	//

	deviceAttributes->Size = sizeof(HID_DEVICE_ATTRIBUTES);
	deviceAttributes->VendorID = CYAPA_VID;
	deviceAttributes->ProductID = CYAPA_PID;
	deviceAttributes->VersionNumber = CYAPA_VERSION;

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, sizeof(HID_DEVICE_ATTRIBUTES));

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaGetDeviceAttributes Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
CyapaGetString(
IN WDFREQUEST Request
)
{

	NTSTATUS status = STATUS_SUCCESS;
	PWSTR pwstrID;
	size_t lenID;
	WDF_REQUEST_PARAMETERS params;
	void *pStringBuffer = NULL;

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaGetString Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	switch ((ULONG_PTR)params.Parameters.DeviceIoControl.Type3InputBuffer & 0xFFFF)
	{
	case HID_STRING_ID_IMANUFACTURER:
		pwstrID = L"Cyapa.\0";
		break;

	case HID_STRING_ID_IPRODUCT:
		pwstrID = L"MaxTouch Touch Screen\0";
		break;

	case HID_STRING_ID_ISERIALNUMBER:
		pwstrID = L"123123123\0";
		break;

	default:
		pwstrID = NULL;
		break;
	}

	lenID = pwstrID ? wcslen(pwstrID)*sizeof(WCHAR) + sizeof(UNICODE_NULL) : 0;

	if (pwstrID == NULL)
	{

		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"CyapaGetString Invalid request type\n");

		status = STATUS_INVALID_PARAMETER;

		return status;
	}

	status = WdfRequestRetrieveOutputBuffer(Request,
		lenID,
		&pStringBuffer,
		&lenID);

	if (!NT_SUCCESS(status))
	{

		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"CyapaGetString WdfRequestRetrieveOutputBuffer failed Status 0x%x\n", status);

		return status;
	}

	RtlCopyMemory(pStringBuffer, pwstrID, lenID);

	WdfRequestSetInformation(Request, lenID);

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaGetString Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
CyapaWriteReport(
IN PCYAPA_CONTEXT DevContext,
IN WDFREQUEST Request
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;
	size_t bytesWritten = 0;

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaWriteReport Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"CyapaWriteReport Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"CyapaWriteReport No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			default:

				CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"CyapaWriteReport Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaWriteReport Exit = 0x%x\n", status);

	return status;

}

NTSTATUS
CyapaProcessVendorReport(
IN PCYAPA_CONTEXT DevContext,
IN PVOID ReportBuffer,
IN ULONG ReportBufferLen,
OUT size_t* BytesWritten
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDFREQUEST reqRead;
	PVOID pReadReport = NULL;
	size_t bytesReturned = 0;

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaProcessVendorReport Entry\n");

	status = WdfIoQueueRetrieveNextRequest(DevContext->ReportQueue,
		&reqRead);

	if (NT_SUCCESS(status))
	{
		status = WdfRequestRetrieveOutputBuffer(reqRead,
			ReportBufferLen,
			&pReadReport,
			&bytesReturned);

		if (NT_SUCCESS(status))
		{
			//
			// Copy ReportBuffer into read request
			//

			if (bytesReturned > ReportBufferLen)
			{
				bytesReturned = ReportBufferLen;
			}

			RtlCopyMemory(pReadReport,
				ReportBuffer,
				bytesReturned);

			//
			// Complete read with the number of bytes returned as info
			//

			WdfRequestCompleteWithInformation(reqRead,
				status,
				bytesReturned);

			CyapaPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
				"CyapaProcessVendorReport %d bytes returned\n", bytesReturned);

			//
			// Return the number of bytes written for the write request completion
			//

			*BytesWritten = bytesReturned;

			CyapaPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
				"%s completed, Queue:0x%p, Request:0x%p\n",
				DbgHidInternalIoctlString(IOCTL_HID_READ_REPORT),
				DevContext->ReportQueue,
				reqRead);
		}
		else
		{
			CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"WdfRequestRetrieveOutputBuffer failed Status 0x%x\n", status);
		}
	}
	else
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfIoQueueRetrieveNextRequest failed Status 0x%x\n", status);
	}

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaProcessVendorReport Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
CyapaReadReport(
IN PCYAPA_CONTEXT DevContext,
IN WDFREQUEST Request,
OUT BOOLEAN* CompleteRequest
)
{
	NTSTATUS status = STATUS_SUCCESS;

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaReadReport Entry\n");

	//
	// Forward this read request to our manual queue
	// (in other words, we are going to defer this request
	// until we have a corresponding write request to
	// match it with)
	//

	status = WdfRequestForwardToIoQueue(Request, DevContext->ReportQueue);

	if (!NT_SUCCESS(status))
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestForwardToIoQueue failed Status 0x%x\n", status);
	}
	else
	{
		*CompleteRequest = FALSE;
	}

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaReadReport Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
CyapaSetFeature(
IN PCYAPA_CONTEXT DevContext,
IN WDFREQUEST Request,
OUT BOOLEAN* CompleteRequest
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;
	CyapaFeatureReport* pReport = NULL;

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaSetFeature Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"CyapaSetFeature Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"CyapaWriteReport No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			case REPORTID_FEATURE:

				if (transferPacket->reportBufferLen == sizeof(CyapaFeatureReport))
				{
					pReport = (CyapaFeatureReport*)transferPacket->reportBuffer;

					DevContext->DeviceMode = pReport->DeviceMode;

					CyapaPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
						"CyapaSetFeature DeviceMode = 0x%x\n", DevContext->DeviceMode);
				}
				else
				{
					status = STATUS_INVALID_PARAMETER;

					CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
						"CyapaSetFeature Error transferPacket->reportBufferLen (%d) is different from sizeof(CyapaFeatureReport) (%d)\n",
						transferPacket->reportBufferLen,
						sizeof(CyapaFeatureReport));
				}

				break;

			default:

				CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"CyapaSetFeature Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaSetFeature Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
CyapaGetFeature(
IN PCYAPA_CONTEXT DevContext,
IN WDFREQUEST Request,
OUT BOOLEAN* CompleteRequest
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaGetFeature Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.OutputBufferLength < sizeof(HID_XFER_PACKET))
	{
		CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"CyapaGetFeature Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"CyapaGetFeature No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			case REPORTID_MTOUCH:
			{

				CyapaMaxCountReport* pReport = NULL;

				if (transferPacket->reportBufferLen == sizeof(CyapaMaxCountReport))
				{
					pReport = (CyapaMaxCountReport*)transferPacket->reportBuffer;

					pReport->MaximumCount = MULTI_MAX_COUNT;

					pReport->PadType = 0;

					CyapaPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
						"CyapaGetFeature MaximumCount = 0x%x\n", MULTI_MAX_COUNT);
				}
				else
				{
					status = STATUS_INVALID_PARAMETER;

					CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
						"CyapaGetFeature Error transferPacket->reportBufferLen (%d) is different from sizeof(CyapaMaxCountReport) (%d)\n",
						transferPacket->reportBufferLen,
						sizeof(CyapaMaxCountReport));
				}

				break;
			}

			case REPORTID_FEATURE:
			{

				CyapaFeatureReport* pReport = NULL;

				if (transferPacket->reportBufferLen == sizeof(CyapaFeatureReport))
				{
					pReport = (CyapaFeatureReport*)transferPacket->reportBuffer;

					pReport->DeviceMode = DevContext->DeviceMode;

					pReport->DeviceIdentifier = 0;

					CyapaPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
						"CyapaGetFeature DeviceMode = 0x%x\n", DevContext->DeviceMode);
				}
				else
				{
					status = STATUS_INVALID_PARAMETER;

					CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
						"CyapaGetFeature Error transferPacket->reportBufferLen (%d) is different from sizeof(CyapaFeatureReport) (%d)\n",
						transferPacket->reportBufferLen,
						sizeof(CyapaFeatureReport));
				}

				break;
			}

			default:

				CyapaPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"CyapaGetFeature Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	CyapaPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CyapaGetFeature Exit = 0x%x\n", status);

	return status;
}

PCHAR
DbgHidInternalIoctlString(
IN ULONG IoControlCode
)
{
	switch (IoControlCode)
	{
	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
		return "IOCTL_HID_GET_DEVICE_DESCRIPTOR";
	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		return "IOCTL_HID_GET_REPORT_DESCRIPTOR";
	case IOCTL_HID_READ_REPORT:
		return "IOCTL_HID_READ_REPORT";
	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
		return "IOCTL_HID_GET_DEVICE_ATTRIBUTES";
	case IOCTL_HID_WRITE_REPORT:
		return "IOCTL_HID_WRITE_REPORT";
	case IOCTL_HID_SET_FEATURE:
		return "IOCTL_HID_SET_FEATURE";
	case IOCTL_HID_GET_FEATURE:
		return "IOCTL_HID_GET_FEATURE";
	case IOCTL_HID_GET_STRING:
		return "IOCTL_HID_GET_STRING";
	case IOCTL_HID_ACTIVATE_DEVICE:
		return "IOCTL_HID_ACTIVATE_DEVICE";
	case IOCTL_HID_DEACTIVATE_DEVICE:
		return "IOCTL_HID_DEACTIVATE_DEVICE";
	case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
		return "IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST";
	case IOCTL_HID_SET_OUTPUT_REPORT:
		return "IOCTL_HID_SET_OUTPUT_REPORT";
	case IOCTL_HID_GET_INPUT_REPORT:
		return "IOCTL_HID_GET_INPUT_REPORT";
	default:
		return "Unknown IOCTL";
	}
}
