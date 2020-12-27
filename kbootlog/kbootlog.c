
#include <ntifs.h>
#include "preprocessor.h"
#include "allocator.h"
#include "pnp-driver-watch.h"
#include "boot-log.h"




NTSTATUS DllInitialize(_In_ PUNICODE_STRING RegistryPath)
{
	UNICODE_STRING uRegistryPath;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION("RegistryPath=\"%wZ\"", RegistryPath);

	RtlInitUnicodeString(&uRegistryPath, L"\\Registry\\Machine\\System\\CurrentControlSet\\services\\irpmndrv");
	status = PWDModuleInit(NULL, &uRegistryPath, NULL);
	if (NT_SUCCESS(status)) {
		status = BLModuleInit(NULL, &uRegistryPath, NULL);
		if (!NT_SUCCESS(status))
			PWDModuleFinit(NULL, &uRegistryPath, NULL);
	}

	DEBUG_EXIT_FUNCTION("0x%x", status);
	return status;
}


NTSTATUS DllUnload(void)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION_NO_ARGS();

	BLModuleFinit(NULL, NULL, NULL);
	PWDModuleFinit(NULL, NULL, NULL);

	DEBUG_EXIT_FUNCTION("0x%x", status);
	return status;
}


NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION("DriverObject=0x%p; RegistryPath=%wZ", DriverObject, RegistryPath);

	status = DllInitialize(RegistryPath);

	DEBUG_EXIT_FUNCTION("0x%x", status);
	return status;
}