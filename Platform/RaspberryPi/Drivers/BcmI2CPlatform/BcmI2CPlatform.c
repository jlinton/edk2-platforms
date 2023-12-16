/** @file
  Brcm/Rpi I2C DXE platform driver.

  Copyright 2018-2020 NXP
  Sourced and reworked from edk2/NXP I2C stack
  Copyright 2022 Arm, Jeremy Linton

  SPDX-License-Identifier: BSD-2-Clause-Patent

  This thing binds a I2C driver to a RTC..

**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <IndustryStandard/Bcm2836.h>

#include <Protocol/NonDiscoverableDevice.h>

typedef struct {
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR StartDesc;
  UINT8 EndDesc;
} ADDRESS_SPACE_DESCRIPTOR;

#define BCM_I2C_NUM_CONTROLLERS 1 //actually 6 on the bcm2711, hack for now

STATIC ADDRESS_SPACE_DESCRIPTOR mI2cDesc[BCM_I2C_NUM_CONTROLLERS];

STATIC
EFI_STATUS
RegisterDevice (
  IN  EFI_GUID                        *TypeGuid,
  IN  ADDRESS_SPACE_DESCRIPTOR        *Desc,
  OUT EFI_HANDLE                      *Handle
  )
{
  NON_DISCOVERABLE_DEVICE             *Device;
  EFI_STATUS                          Status;

  Device = (NON_DISCOVERABLE_DEVICE *)AllocateZeroPool (sizeof (*Device));
  if (Device == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Device->Type = TypeGuid;
  Device->DmaType = NonDiscoverableDeviceDmaTypeNonCoherent;
  Device->Resources = (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)Desc;

  Status = gBS->InstallMultipleProtocolInterfaces (Handle,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid, Device,
                  NULL);
  if (EFI_ERROR (Status)) {
    goto FreeDevice;
  }
  return EFI_SUCCESS;

FreeDevice:
  FreePool (Device);

  return Status;
}

VOID
PopulateI2cInformation (
  IN VOID
  )
{
  UINT32 Index;

  for (Index = 0; Index < ARRAY_SIZE (mI2cDesc); Index++) {
    mI2cDesc[Index].StartDesc.Desc = ACPI_ADDRESS_SPACE_DESCRIPTOR;
    mI2cDesc[Index].StartDesc.Len = sizeof (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR) - 3;
    mI2cDesc[Index].StartDesc.ResType = ACPI_ADDRESS_SPACE_TYPE_MEM;
    mI2cDesc[Index].StartDesc.GenFlag = 0;
    mI2cDesc[Index].StartDesc.SpecificFlag = 0;
    mI2cDesc[Index].StartDesc.AddrSpaceGranularity = 32;
    mI2cDesc[Index].StartDesc.AddrRangeMin = BCM2836_I2C1_BASE_ADDRESS;
    mI2cDesc[Index].StartDesc.AddrRangeMax = mI2cDesc[Index].StartDesc.AddrRangeMin + BCM2836_I2C1_LENGTH;
    mI2cDesc[Index].StartDesc.AddrTranslationOffset = 0;
    mI2cDesc[Index].StartDesc.AddrLen = BCM2836_I2C1_LENGTH;

    mI2cDesc[Index].EndDesc = ACPI_END_TAG_DESCRIPTOR;
  }
}

EFI_STATUS
EFIAPI
BcmI2CPlatformDxeEntryPoint (
  IN EFI_HANDLE         ImageHandle,
  IN EFI_SYSTEM_TABLE   *SystemTable
  )
{
  EFI_STATUS                      Status;
  EFI_HANDLE                      Handle;

  Handle = NULL;

  PopulateI2cInformation ();

  if (PcdGet32 (PcdHwRtc))
  {
    DEBUG ((DEBUG_ERROR, "RTC I2C enable\n"));
    // If we don't register this, the second rtc won't get enabled
    // leaving the emulator in place.
    Status = RegisterDevice (&gBcmNonDiscoverableI2cMasterGuid,
               &mI2cDesc[0], &Handle);
    ASSERT_EFI_ERROR (Status);

    //
    // Install the DS1307 I2C Master protocol on this handle so the RTC driver
    // can identify it as the I2C master it can invoke directly.
    //
    Status = gBS->InstallProtocolInterface (&Handle,
                    &gDs1307RealTimeClockLibI2cMasterProtocolGuid,
                    EFI_NATIVE_INTERFACE, NULL);
    ASSERT_EFI_ERROR (Status);
  }  else  {
    DEBUG ((DEBUG_ERROR, "RTC I2C disabled\n"));
  }

  return EFI_SUCCESS;
}
