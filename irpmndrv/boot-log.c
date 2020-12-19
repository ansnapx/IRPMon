
#include <ntifs.h>
#include <fltKernel.h>
#include "allocator.h"
#include "preprocessor.h"
#include "general-types.h"
#include "request.h"
#include "driver-settings.h"
#include "pnp-driver-watch.h"
#include "boot-log.h"



static LIST_ENTRY _blRequestListHead;
static EX_PUSH_LOCK _blRequestListLock;
static LIST_ENTRY _blNPCache;
static KSPIN_LOCK _blNPLock;
static volatile BOOLEAN _blEnabled = FALSE;
static PETHREAD _blSavingThread = NULL;



static void _ListHeadMove(PLIST_ENTRY Original, PLIST_ENTRY New)
{
	New->Flink = Original->Flink;
	New->Blink = Original->Blink;
	New->Flink->Blink = New;
	New->Blink->Flink = New;
	InitializeListHead(Original);

	return;
}


static void _AddTailList(PLIST_ENTRY Head, PLIST_ENTRY List)
{
	if (!IsListEmpty(List)) {
		Head->Blink->Flink = List->Flink;
		List->Flink->Blink = Head->Blink;
		List->Blink->Flink = Head;
		Head->Blink = List->Blink;
		InitializeListHead(List);
	}

	return;
}

static void _NPCacheInsert(PREQUEST_HEADER Request)
{
	KIRQL irql;
	DEBUG_ENTER_FUNCTION("Request=0x%p", Request);

	KeAcquireSpinLock(&_blNPLock, &irql);
	InsertTailList(&_blNPCache, &Request->Entry);
	KeReleaseSpinLock(&_blNPLock, irql);

	DEBUG_EXIT_FUNCTION_VOID();
	return;
}


static void _NPCacheFlush(PLIST_ENTRY Head)
{
	KIRQL irql;
	DEBUG_ENTER_FUNCTION("Head=0x%p", Head);

	KeAcquireSpinLock(&_blNPLock, &irql);
	_ListHeadMove(&_blNPCache, Head);
	KeReleaseSpinLock(&_blNPLock, irql);

	DEBUG_EXIT_FUNCTION_VOID();
	return;
}


static NTSTATUS _FlushBootRequests(HANDLE FileHandle)
{
	IO_STATUS_BLOCK iosb;
	LIST_ENTRY reqsToSave;
	PREQUEST_HEADER tmp = NULL;
	PREQUEST_HEADER old = NULL;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION("FileHandle=0x%p", FileHandle);

	InitializeListHead(&reqsToSave);
	_NPCacheFlush(&reqsToSave);
	FltAcquirePushLockExclusive(&_blRequestListLock);
	_AddTailList(&_blRequestListHead, &reqsToSave);
	_ListHeadMove(&_blNPCache, &reqsToSave);
	FltReleasePushLock(&_blRequestListLock);
	tmp = CONTAINING_RECORD(&reqsToSave.Flink, REQUEST_HEADER, Entry);
	while (&tmp->Entry != &reqsToSave) {
		old = tmp;
		tmp = CONTAINING_RECORD(tmp->Entry.Flink, REQUEST_HEADER, Entry);
		status = ZwWriteFile(FileHandle, NULL, NULL, NULL, &iosb, old, (ULONG)RequestGetSize(old), NULL, NULL);
		RequestMemoryFree(old);
	}

	DEBUG_EXIT_FUNCTION("0x%x", status);
	return status;
}


static void _BLSaveThread(PVOID Context)
{
	IO_STATUS_BLOCK iosb;
	OBJECT_ATTRIBUTES oa;
	HANDLE hFile = NULL;
	LARGE_INTEGER timeout;
	UNICODE_STRING uFileName;
	BINARY_LOG_HEADER hdr;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION("Context=0x%p", Context);

	RtlSecureZeroMemory(&hdr, sizeof(hdr));
	hdr.Signature = LOGHEADER_SIGNATURE;
	hdr.Version = LOGHEADER_VERSION;
#ifdef _X86_
	hdr.Architecture = LOGHEADER_ARCHITECTURE_X86;
#elif defined(_AMD64_)
	hdr.Architecture = LOGHEADER_ARCHITECTURE_X64;
#else
#error Unsupported architecture
#endif
	timeout.QuadPart = -10000000;
	RtlInitUnicodeString(&uFileName, L"\\SystemRoot\\IRPMon-boot.bin");
	InitializeObjectAttributes(&oa, &uFileName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
	while (BLEnabled()) {
		status = ZwCreateFile(&hFile, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_SUPERSEDE, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
		if (NT_SUCCESS(status)) {
			status = ZwWriteFile(hFile, NULL, NULL, NULL, &iosb, &hdr, sizeof(hdr), NULL, NULL);
			if (NT_SUCCESS(status)) {
				while (BLEnabled()) {
					status = _FlushBootRequests(hFile);
					KeDelayExecutionThread(KernelMode, FALSE, &timeout);
				}
			}

			status = _FlushBootRequests(hFile);
			ZwClose(hFile);
		}

		KeDelayExecutionThread(KernelMode, FALSE, &timeout);
	}

	DEBUG_EXIT_FUNCTION_VOID();
	return;
}


static NTSTATUS _RegReadUInt32(HANDLE KeyHandle, PUNICODE_STRING ValueName, PULONG Value)
{
	ULONG retLength = 0;
	PKEY_VALUE_PARTIAL_INFORMATION kvpi = NULL;
	unsigned char buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION("KeyHandle=0x%p; ValueName=\"%wZ\"; Value=0x%p", KeyHandle, ValueName, Value);

	kvpi = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
	status = ZwQueryValueKey(KeyHandle, ValueName, KeyValuePartialInformation, kvpi, sizeof(buffer), &retLength);
	if (NT_SUCCESS(status)) {
		if (kvpi->Type == REG_DWORD && kvpi->DataLength == sizeof(ULONG))
			*Value = *(PULONG)kvpi->Data;
		else status = STATUS_OBJECT_TYPE_MISMATCH;
	}

	DEBUG_EXIT_FUNCTION("0x%x, *Value=%u", status, *Value);
	return status;
}


static NTSTATUS _RegReadString(HANDLE KeyHandle, PUNICODE_STRING ValueName, PUNICODE_STRING US)
{
	ULONG retLength = 0;
	wchar_t *data = NULL;
	PKEY_VALUE_PARTIAL_INFORMATION kvpi = NULL;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION("KeyHandle=0x%p; ValueName=\"%wZ\"; US=0x%p", KeyHandle, ValueName, US);

	RtlSecureZeroMemory(US, sizeof(UNICODE_STRING));
	status = ZwQueryValueKey(KeyHandle, ValueName, KeyValuePartialInformation, NULL, 0, &retLength);
	if (status == STATUS_BUFFER_TOO_SMALL) {
		kvpi = HeapMemoryAllocPaged(retLength);
		if (kvpi != NULL) {
			status = ZwQueryValueKey(KeyHandle, ValueName, KeyValuePartialInformation, kvpi, retLength, &retLength);
			if (NT_SUCCESS(status)) {
				if (kvpi->Type != REG_SZ && kvpi->Type != REG_EXPAND_SZ)
					status = STATUS_OBJECT_TYPE_MISMATCH;

				if (NT_SUCCESS(status) && kvpi->DataLength % 2 != 0)
					status = STATUS_INVALID_PARAMETER;

				if (NT_SUCCESS(status) && kvpi->DataLength > 0) {
					data = (wchar_t *)kvpi->Data;
					data += (kvpi->DataLength / sizeof(wchar_t)) - 1;
					if (*data == L'\0')
						kvpi->DataLength -= sizeof(wchar_t);

					US->Length = (USHORT)kvpi->DataLength;
					US->MaximumLength = US->Length;
					US->Buffer = HeapMemoryAllocPaged(US->Length);
					if (US->Buffer != NULL)
						memcpy(US->Buffer, kvpi->Data, US->Length);
					else status = STATUS_INSUFFICIENT_RESOURCES;
				}
			}

			HeapMemoryFree(kvpi);
		} else status = STATUS_INSUFFICIENT_RESOURCES;
	}

	DEBUG_EXIT_FUNCTION("0x%x, US=\"%wZ\"", status, US);
	return status;
}

static NTSTATUS _LoadDriverMonitoringSettings(HANDLE KeyHandle, PDRIVER_MONITOR_SETTINGS Settings)
{
	const wchar_t *valueNames[] = {
		L"NewDevices",
		L"IRP",
		L"FastIo",
		L"IRPCompletion",
		L"StartIo",
		L"AddDevice",
		L"Unload",
		L"Data",
	};
	PBOOLEAN values[] = {
		&Settings->MonitorNewDevices,
		&Settings->MonitorIRP,
		&Settings->MonitorFastIo,
		&Settings->MonitorIRPCompletion,
		&Settings->MonitorStartIo,
		&Settings->MonitorAddDevice,
		&Settings->MonitorUnload,
		&Settings->MonitorData,
	};
	ULONG value = 0;
	UNICODE_STRING uValueName;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION("KeyHandle=0x%p; Settings=0x%p", KeyHandle, Settings);

	status = STATUS_SUCCESS;
	RtlSecureZeroMemory(Settings, sizeof(DRIVER_MONITOR_SETTINGS));
	for (size_t i = 0; i < sizeof(Settings->IRPSettings) / sizeof(Settings->IRPSettings[0]); ++i)
		Settings->IRPSettings[i] = TRUE;

	for (size_t i = 0; i < sizeof(Settings->FastIoSettings) / sizeof(Settings->FastIoSettings[0]); ++i)
		Settings->FastIoSettings[i] = TRUE;

	for (size_t i = 0; i < sizeof(valueNames) / sizeof(valueNames[0]); ++i) {
		RtlInitUnicodeString(&uValueName, valueNames[i]);
		status = _RegReadUInt32(KeyHandle, &uValueName, &value);
		if (NT_SUCCESS(status))
			*(values[i]) = (value != 0);
	
		if (status == STATUS_OBJECT_NAME_NOT_FOUND)
			status = STATUS_SUCCESS;

		if (!NT_SUCCESS(status))
			break;
	}

	DEBUG_EXIT_FUNCTION("0x%x", status);
	return status;
}


static NTSTATUS _SaveDriverMonitoringSettings(HANDLE KeyHandle, const DRIVER_MONITOR_SETTINGS *Settings)
{
	const wchar_t* valueNames[] = {
		L"NewDevices",
		L"IRP",
		L"FastIo",
		L"IRPCompletion",
		L"StartIo",
		L"AddDevice",
		L"Unload",
		L"Data",
	};
	const BOOLEAN *values[] = {
		&Settings->MonitorNewDevices,
		&Settings->MonitorIRP,
		&Settings->MonitorFastIo,
		&Settings->MonitorIRPCompletion,
		&Settings->MonitorStartIo,
		&Settings->MonitorAddDevice,
		&Settings->MonitorUnload,
		&Settings->MonitorData,
	};
	ULONG value = 0;
	UNICODE_STRING uValueName;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION("KeyHandle=0x%p; Settings=0x%p", KeyHandle, Settings);

	status = STATUS_SUCCESS;
	for (size_t i = 0; i < sizeof(valueNames) / sizeof(valueNames[0]); ++i) {
		value = *(values[i]);
		RtlInitUnicodeString(&uValueName, valueNames[i]);
		status = ZwSetValueKey(KeyHandle, &uValueName, 0, REG_DWORD, &value, sizeof(value));
		if (!NT_SUCCESS(status))
			break;
	}

	DEBUG_EXIT_FUNCTION("0x%x", status);
	return status;
}


static NTSTATUS _LoadSettings(PUNICODE_STRING RegistryPath)
{
	HANDLE blKey = NULL;
	HANDLE servicesKey = NULL;
	OBJECT_ATTRIBUTES oa;
	UNICODE_STRING uName;
	UNICODE_STRING uValueName;
	DRIVER_MONITOR_SETTINGS monitorSettings;
	PIRPMNDRV_SETTINGS globalSettings = NULL;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION("RegistryPath=\"%wZ\"", RegistryPath);

	globalSettings = DriverSettingsGet();
	_blEnabled = globalSettings->LogBoot;
	InitializeObjectAttributes(&oa, RegistryPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
	status = ZwOpenKey(&servicesKey, KEY_READ, &oa);
	if (NT_SUCCESS(status)) {
		RtlInitUnicodeString(&uName, L"BootLogging");
		InitializeObjectAttributes(&oa, &uName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, servicesKey, NULL);
		status = ZwOpenKey(&blKey, KEY_READ, &oa);
		if (NT_SUCCESS(status)) {
			RtlInitUnicodeString(&uValueName, L"DriverObjectName");
			status = _RegReadString(blKey, &uValueName, &uName);
			if (NT_SUCCESS(status)) {
				status = _LoadDriverMonitoringSettings(blKey, &monitorSettings);
				if (NT_SUCCESS(status))
					status = PWDDriverNameRegister(&uName, &monitorSettings);
				
				HeapMemoryFree(uName.Buffer);
			}

			ZwClose(blKey);
		}

		ZwClose(servicesKey);
	}

	if (status == STATUS_OBJECT_NAME_NOT_FOUND)
		status = STATUS_SUCCESS;

	if (NT_SUCCESS(status) && _blEnabled)
		status = PDWMonitorFileSystems(TRUE);

	DEBUG_EXIT_FUNCTION("0x%x", status);
	return status;
}


void BLLogRequest(PREQUEST_HEADER Request)
{
	LIST_ENTRY cachedHead;
	DEBUG_ENTER_FUNCTION("Request=0x%p", Request);

	if (KeGetCurrentIrql() < DISPATCH_LEVEL) {
		InitializeListHead(&cachedHead);
		_NPCacheFlush(&cachedHead);
		FltAcquirePushLockExclusive(&_blRequestListLock);
		_AddTailList(&_blRequestListHead, &cachedHead);
		InsertTailList(&_blRequestListHead, &Request->Entry);
		FltReleasePushLock(&_blRequestListLock);
	} else _NPCacheInsert(Request);

	DEBUG_EXIT_FUNCTION_VOID();
	return;
}

BOOLEAN BLEnabled(void)
{
	return _blEnabled;
}


void BLDisable(void)
{
	DEBUG_ENTER_FUNCTION_NO_ARGS();

	_blEnabled = FALSE;
	KeWaitForSingleObject(_blSavingThread, Executive, KernelMode, FALSE, NULL);

	DEBUG_EXIT_FUNCTION_VOID();
	return;
}


NTSTATUS BLModuleInit(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath, PVOID Context)
{
	HANDLE hThread = NULL;
	CLIENT_ID clientId;
	OBJECT_ATTRIBUTES oa;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION("", DriverObject, RegistryPath, Context);

	InitializeListHead(&_blRequestListHead);
	FltInitializePushLock(&_blRequestListLock);
	InitializeListHead(&_blNPCache);
	KeInitializeSpinLock(&_blNPLock);
	status = _LoadSettings(RegistryPath);
	if (NT_SUCCESS(status)) {
		InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
		status = PsCreateSystemThread(&hThread, SYNCHRONIZE, &oa, NULL, &clientId, _BLSaveThread, NULL);
		if (NT_SUCCESS(status)) {
			status = ObReferenceObjectByHandle(hThread, SYNCHRONIZE, *PsThreadType, KernelMode, &_blSavingThread, NULL);
			ZwClose(hThread);
		}
	}

	DEBUG_EXIT_FUNCTION("0x%x", status);
	return status;
}


void BLModuleFinit(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath, PVOID Context)
{
	DEBUG_ENTER_FUNCTION("", DriverObject, RegistryPath, Context);

	BLDisable();
	ObDereferenceObject(_blSavingThread);

	DEBUG_EXIT_FUNCTION_VOID();
	return;
}