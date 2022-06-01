/** I2cDxe.c
  I2c driver APIs for read, write, initialize, set speed and reset

  Sourced and reworked from edk2/NXP I2C stack
  Copyright 2022 Arm, Jeremy Linton

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeLib.h>

#include <IndustryStandard/Bcm2836.h>

#include "I2cDxe.h"

STATIC CONST EFI_I2C_CONTROLLER_CAPABILITIES mI2cControllerCapabilities = {
  0,
  0,
  0,
  0
};

/**
  Function to set i2c bus frequency

  @param   This            Pointer to I2c master protocol
  @param   BusClockHertz   value to be set

  @retval EFI_SUCCESS      Operation successfull
**/
STATIC
EFI_STATUS
EFIAPI
SetBusFrequency (
  IN CONST EFI_I2C_MASTER_PROTOCOL   *This,
  IN OUT UINTN                       *BusClockHertz
 )
{
  UINTN                    I2cBase;
  UINT64                   I2cClock;
  BCM_I2C_MASTER           *I2c;

  I2c = BCM_I2C_FROM_THIS (This);

  I2cBase = I2c->ControllerBase;

  // depend on ConfigDxe? On the NXP this only sets the clock and resets the bus
  // Here we are hardcoding the I2C clock until we have a need not to.
  I2cClock = 50000;

  return EFI_SUCCESS;
}

/**
  Function to reset I2c Controller

  @param  This             Pointer to I2c master protocol

  @return EFI_SUCCESS      Operation successfull
**/
STATIC
EFI_STATUS
EFIAPI
Reset (
  IN CONST EFI_I2C_MASTER_PROTOCOL *This
  )
{
  return EFI_SUCCESS;
}

STATIC
void
StatusPoll(
  UINTN              I2cBase,
  UINTN              Mask
    )
{
  UINTN              Retry;

  Retry = 0;
  while ((MmioRead32 (I2cBase + BCM2835_I2C_S) & Mask) != Mask) {
    Retry++;
  }
}



STATIC
EFI_STATUS
SingleTransfer(
  UINTN              I2cBase,
  UINTN              SlaveAddress,
  EFI_I2C_OPERATION  *Operation
)
{
  EFI_STATUS               Status;
  UINTN                    Index;
  UINTN                    FifoState;

  // clear all the status
  MmioWrite32 (I2cBase + BCM2835_I2C_C, 0x10 ); // fifo clear
  MmioWrite32 (I2cBase + BCM2835_I2C_S, 0x302);

  // don't support 10 bit addr for now. (see 3.3 in 2711 manual)
  // the problem with arm is that you never know if there are undocumented
  // acces restrictions (aka 8 bit reg, but it needs to be read with a 32-bit instr)
  MmioWrite8 (I2cBase + BCM2835_I2C_A, (UINT8)SlaveAddress);
  MmioWrite32 (I2cBase + BCM2835_I2C_DLEN, Operation->LengthInBytes );

  if (Operation->Flags & I2C_FLAG_READ) {
    FifoState = 0x20; // fifo has data
    MmioWrite32 (I2cBase + BCM2835_I2C_C, 0x8081 ); // Enable, start, fifo clear, read
  } else {
    FifoState = 0x10; // fifo can accept data
    MmioWrite32 (I2cBase + BCM2835_I2C_C, 0x8080 ); // Enable, start, fifo clear, write
  }

  for (Index = 0; Index < Operation->LengthInBytes; Index++) {

    StatusPoll (I2cBase, FifoState);

    if (Operation->Flags & I2C_FLAG_READ) {
      Operation->Buffer[Index] = MmioRead8 (I2cBase + BCM2835_I2C_FIFO);
    } else {
      MmioWrite8 (I2cBase + BCM2835_I2C_FIFO, Operation->Buffer[Index]);
    }
  }

  StatusPoll (I2cBase, 0x02);

  Status = 0;
  return Status;
}

volatile UINTN forcewrite;

STATIC
EFI_STATUS
EFIAPI
StartRequest (
  IN CONST EFI_I2C_MASTER_PROTOCOL *This,
  IN UINTN                         SlaveAddress,
  IN EFI_I2C_REQUEST_PACKET        *RequestPacket,
  IN EFI_EVENT                     Event            OPTIONAL,
  OUT EFI_STATUS                   *I2cStatus       OPTIONAL
  )
{
  BCM_I2C_MASTER           *I2c;
  UINTN                    I2cBase;
  EFI_STATUS               Status;
  EFI_TPL                  Tpl;
  BOOLEAN                  AtRuntime;
  UINTN                    Index;

  AtRuntime = EfiAtRuntime ();
  if (!AtRuntime) {
    Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  }

  I2c = BCM_I2C_FROM_THIS (This);

  I2cBase = I2c->ControllerBase;

  for (Index = 0; Index < RequestPacket->OperationCount; Index++ ) {

    Status = SingleTransfer (I2cBase, SlaveAddress,  &RequestPacket->Operation[Index]);

    if (EFI_ERROR (Status)) {
      break;
    }
  }

  if (!AtRuntime) {
    gBS->RestoreTPL (Tpl);
  }

  return Status;
}

STATIC
VOID
EFIAPI
BcmI2cVirtualAddressChangeEvent (
  IN EFI_EVENT Event,
  IN VOID *Context
  )
{
  BCM_I2C_MASTER            *I2c;

  I2c = (BCM_I2C_MASTER *)Context;

  EfiConvertPointer (0x0, (VOID**)&I2c->ControllerBase);
  EfiConvertPointer (0x0, (VOID**)&I2c);
}


EFI_STATUS
BcmI2cInit (
  IN EFI_HANDLE             DriverBindingHandle,
  IN EFI_HANDLE             ControllerHandle
  )
{
  EFI_STATUS                RetVal;
  NON_DISCOVERABLE_DEVICE   *Dev;
  BCM_I2C_MASTER            *I2c;
  EFI_EVENT                 VirtualAddressChangeEvent;

  RetVal = gBS->OpenProtocol (ControllerHandle,
                              &gEdkiiNonDiscoverableDeviceProtocolGuid,
                              (VOID **)&Dev, DriverBindingHandle,
                              ControllerHandle, EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (EFI_ERROR (RetVal)) {
    return RetVal;
  }

  I2c = AllocateRuntimeZeroPool (sizeof (BCM_I2C_MASTER));

  I2c->Signature                            = BCM_I2C_SIGNATURE;
  I2c->I2cMaster.SetBusFrequency            = SetBusFrequency;
  I2c->I2cMaster.Reset                      = Reset;
  I2c->I2cMaster.StartRequest               = StartRequest;
  I2c->I2cMaster.I2cControllerCapabilities  = &mI2cControllerCapabilities;
  I2c->Dev                                  = Dev;

  CopyGuid (&I2c->DevicePath.Vendor.Guid, &gEfiCallerIdGuid);
  I2c->DevicePath.MmioBase = I2c->Dev->Resources[0].AddrRangeMin;
  I2c->ControllerBase = I2c->Dev->Resources[0].AddrRangeMin;
  SetDevicePathNodeLength (&I2c->DevicePath.Vendor,
    sizeof (I2c->DevicePath) - sizeof (I2c->DevicePath.End));
  SetDevicePathEndNode (&I2c->DevicePath.End);

  RetVal = gBS->InstallMultipleProtocolInterfaces (&ControllerHandle,
                  &gEfiI2cMasterProtocolGuid, (VOID**)&I2c->I2cMaster,
                  &gEfiDevicePathProtocolGuid, &I2c->DevicePath,
                  NULL);

  if (EFI_ERROR (RetVal)) {
    FreePool (I2c);
    gBS->CloseProtocol (ControllerHandle,
                        &gEdkiiNonDiscoverableDeviceProtocolGuid,
                        DriverBindingHandle,
                        ControllerHandle);
  } else {
    RetVal = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  BcmI2cVirtualAddressChangeEvent,
                  (VOID *)I2c,
                  &gEfiEventVirtualAddressChangeGuid,
                  &VirtualAddressChangeEvent
                );

    ASSERT_EFI_ERROR (RetVal);

  }

  return RetVal;
}

EFI_STATUS
BcmI2cRelease (
  IN EFI_HANDLE                 DriverBindingHandle,
  IN EFI_HANDLE                 ControllerHandle
  )
{
  EFI_I2C_MASTER_PROTOCOL       *I2cMaster;
  EFI_STATUS                    RetVal;
  BCM_I2C_MASTER                *I2c;

  RetVal = gBS->HandleProtocol (ControllerHandle,
                                &gEfiI2cMasterProtocolGuid,
                                (VOID **)&I2cMaster);
  ASSERT_EFI_ERROR (RetVal);
  if (EFI_ERROR (RetVal)) {
    return RetVal;
  }

  I2c = BCM_I2C_FROM_THIS (I2cMaster);

  RetVal = gBS->UninstallMultipleProtocolInterfaces (ControllerHandle,
                  &gEfiI2cMasterProtocolGuid, I2cMaster,
                  &gEfiDevicePathProtocolGuid, &I2c->DevicePath,
                  NULL);
  if (EFI_ERROR (RetVal)) {
    return RetVal;
  }

  RetVal = gBS->CloseProtocol (ControllerHandle,
                               &gEdkiiNonDiscoverableDeviceProtocolGuid,
                               DriverBindingHandle,
                               ControllerHandle);
  ASSERT_EFI_ERROR (RetVal);
  if (EFI_ERROR (RetVal)) {
    return RetVal;
  }

  gBS->FreePool (I2c);

  return EFI_SUCCESS;
}
