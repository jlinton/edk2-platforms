/** I2cDxe.h
  Header defining the constant, base address amd function for I2C controller

  Copyright 2017-2020 NXP
  Sourced and reworked from edk2/NXP I2C stack
  Copyright 2022 Arm, Jeremy Linton

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef I2C_DXE_H_
#define I2C_DXE_H_

#include <Library/UefiLib.h>
#include <Uefi.h>

#include <Protocol/I2cMaster.h>
#include <Protocol/NonDiscoverableDevice.h>

#define BCM_I2C_SIGNATURE         SIGNATURE_32 ('B', 'I', '2', 'C')
#define BCM_I2C_FROM_THIS(a)      CR ((a), BCM_I2C_MASTER, \
                                    I2cMaster, BCM_I2C_SIGNATURE)

extern EFI_COMPONENT_NAME2_PROTOCOL gBcmI2cDriverComponentName2;

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH              Vendor;
  UINT64                          MmioBase;
  EFI_DEVICE_PATH_PROTOCOL        End;
} BCM_I2C_DEVICE_PATH;
#pragma pack()

typedef struct {
  UINT32                          Signature;
  EFI_I2C_MASTER_PROTOCOL         I2cMaster;
  BCM_I2C_DEVICE_PATH             DevicePath;
  NON_DISCOVERABLE_DEVICE         *Dev;
  UINTN                           ControllerBase;
} BCM_I2C_MASTER;

EFI_STATUS
BcmI2cInit (
  IN EFI_HANDLE  DriverBindingHandle,
  IN EFI_HANDLE  ControllerHandle
  );

EFI_STATUS
BcmI2cRelease (
  IN EFI_HANDLE  DriverBindingHandle,
  IN EFI_HANDLE  ControllerHandle
  );

#endif //I2C_DXE_H_
