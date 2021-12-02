/** @file
 *
 *  Copyright (c) 2018, Andrei Warkentin <andrey.warkentin@gmail.com>
 *  Copyright (c) 2006-2014, Intel Corporation. All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Base.h>

#include <IndustryStandard/Bcm2836.h>
#include <IndustryStandard/Bcm2836Gpio.h>

#include <Protocol/DevicePath.h>
#include <Protocol/FirmwareVolumeBlock.h>
#include <Protocol/RpiFirmware.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/GpioLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Guid/VariableFormat.h>

#include "VarBlockService.h"

#define EFI_FVB2_STATUS \
          (EFI_FVB2_READ_STATUS | EFI_FVB2_WRITE_STATUS | EFI_FVB2_LOCK_STATUS)

EFI_FW_VOL_INSTANCE *mFvInstance;

FV_MEMMAP_DEVICE_PATH mFvMemmapDevicePathTemplate = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_MEMMAP_DP,
      {
        (UINT8)(sizeof (MEMMAP_DEVICE_PATH)),
        (UINT8)(sizeof (MEMMAP_DEVICE_PATH) >> 8)
      }
    },
    EfiMemoryMappedIO,
    (EFI_PHYSICAL_ADDRESS)0,
    (EFI_PHYSICAL_ADDRESS)0,
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      END_DEVICE_PATH_LENGTH,
      0
    }
  }
};

FV_PIWG_DEVICE_PATH mFvPIWGDevicePathTemplate = {
  {
    {
      MEDIA_DEVICE_PATH,
      MEDIA_PIWG_FW_VOL_DP,
      {
        (UINT8)(sizeof (MEDIA_FW_VOL_DEVICE_PATH)),
        (UINT8)(sizeof (MEDIA_FW_VOL_DEVICE_PATH) >> 8)
      }
    },
    { 0 }
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      END_DEVICE_PATH_LENGTH,
      0
    }
  }
};

EFI_FW_VOL_BLOCK_DEVICE mFvbDeviceTemplate = {
  NULL,
  {
    FvbProtocolGetAttributes,
    FvbProtocolSetAttributes,
    FvbProtocolGetPhysicalAddress,
    FvbProtocolGetBlockSize,
    FvbProtocolRead,
    FvbProtocolWrite,
    FvbProtocolEraseBlocks,
    NULL
  }
};

/*
 * This is a derived approximation for the number of BCM2835_SPI_CS
 * register reads that can be accomplished in 1US on a bcm2711.
 */
#define SPI_CS_READS_PER_US 25

STATIC
VOID
SpiDelay (
    IN UINTN micro_sec
    )
/*++

  Routine Description:
     Delay loop based on how fast we can read SPI controller
     state rather than a hardcoded delay since we are a runtime
     service.

  Arguments:
     micro_sec          - Approximate number of micro seconds to delay.

  Returns:
--*/
{
  UINT32 looping;
  for (looping = 0; looping < micro_sec; looping++) {
    UINT32 looping2;
    //
    // RPi4 does about 25 reg reads per micro second
    //
    for (looping2 = 0; looping2 < SPI_CS_READS_PER_US; looping2++) {
      MmioRead32 (mFvInstance->SpiBase+BCM2835_SPI_CS);
    }
  }
}

STATIC
INT32
DoSpiCommand (
    UINT8 *Buffer,
    UINTN in_len,
    UINTN out_len)
/*++

  Routine Description:
     Read and/or Write data on the SPI bus. A buffer with
     data to write/read is passed and this routine then writes
     out_len data on the SPI bus, and reads in_len data back
     into the buffer starting at the first location. Obviously
     this means that the buffer must be greater than the largest
     of in_len or out_len

  Arguments:
     in_len      - bytes to read from the SPI to buffer
     out_len     - bytes to write on the SPI from buffer

  Returns:
     number of bytes read into buffer.
--*/
{
  int cur_byte;
  UINT32 ret = 0;

  MmioWrite32 (mFvInstance->SpiBase + BCM2835_SPI_CS, BCM2835_SPI_CS_TA);

  for (cur_byte = 0 ;cur_byte < in_len + out_len; cur_byte++)
  {
    int loop = 10000*SPI_CS_READS_PER_US;

    while ((MmioRead32 (mFvInstance->SpiBase + BCM2835_SPI_CS) & BCM2835_SPI_CS_TXD) == 0) {
      loop--;
      if (loop == 0) {
        DEBUG ((DEBUG_ERROR, "Write timeout %X\n", MmioRead32 (mFvInstance->SpiBase+BCM2835_SPI_CS)));
        ret = -1;
        break;
      }
    }

    if (cur_byte < out_len) {
      MmioWrite32 (mFvInstance->SpiBase + BCM2835_SPI_FIFO, Buffer[cur_byte]);
    } else {
      MmioWrite32 (mFvInstance->SpiBase + BCM2835_SPI_FIFO, 0);
    }

    loop = 10000 * SPI_CS_READS_PER_US;
    while ((MmioRead32 (mFvInstance->SpiBase + BCM2835_SPI_CS) & BCM2835_SPI_CS_RXD) == 0) {
      loop--;
      if (loop == 0) {
        DEBUG ((DEBUG_ERROR, "Read timeout %X\n", MmioRead32 (mFvInstance->SpiBase+BCM2835_SPI_CS)));
        ret = -1;
        break;
      }
    }

    if (cur_byte < out_len) {
      MmioRead32 (mFvInstance->SpiBase + BCM2835_SPI_FIFO);
    } else {
      ret++;
      Buffer[cur_byte - out_len] = MmioRead32 (mFvInstance->SpiBase + BCM2835_SPI_FIFO);
    }
  }

  MmioWrite32 (mFvInstance->SpiBase + BCM2835_SPI_CS, 0);

  SpiDelay (1); //wait for /CS to settle

  return ret;
}

#if (RPI_MODEL == 4)
STATIC
INT32
ReadDeviceId (VOID)
/*++

  Routine Description:
     Sends the SPI device identification command then checks to see
     if its a winbond we recognize, and returns the expected capacity.

  Arguments:

  Returns:
     Capacity of attached device
--*/
{
  UINT8 Buffer[32];
  Buffer[0] = 0x9F;

  DoSpiCommand (Buffer, 3, 1);   //EF 30 31 is the winbond W25X40CL on the base rpi4
  if (Buffer[0] != 0xEF) {
    DEBUG ((DEBUG_INFO, "ReadDeviceId %02X %02X %02X\n",
            Buffer[0], Buffer[1], Buffer[2]));
  }
  // Lets assume we understand JEDEC type 0x30
  if (Buffer[1] == 0x30) {
    // it should be 512K
    return 1 << Buffer[2]; //not really standard...
  }

  return 0;
}


STATIC
INT32
ReadSpi (
    UINT32 Addr,
    UINT8 *Buffer,
    UINT32 Len)
/*++

  Routine Description:
     Reads Len bytes of data at flash Addr into Buffer

  Arguments:
     Addr    -  Flash device address
     Buffer  -  Buffer where data is returned, this
                buffer must be at least 5 bytes long to
                hold the command.
     Len     -  Number of bytes to read into Buffer

  Returns:
     Number of bytes read
--*/
{
  INT32 ret;
  Buffer[0] = 0x0B; //send read data
  Buffer[1] = (Addr >> 16) & 0xFF; // address MSB
  Buffer[2] = (Addr >> 8) & 0xFF;  //
  Buffer[3] = Addr & 0xFF;         // address LSB
  Buffer[4] = 0;

  ret = DoSpiCommand (Buffer, Len, 5);
  return ret;
}

STATIC
UINT32
WalkFlashVolume (VOID)
/*++

  Routine Description:
     Walk the RPi's SPI flash volume to determine if there is
     free space we may consume as the backing store for a UEFI
     variable store volume. This is fairly safe as the entire volume
     can be recovered using the Raspberry Pi OS image tool to create
     an EEPROM update disk. We aren't going to bother to
     attempt to contain it in their volume format, rather hiding in
     the free/unclaimed space. If this space is corrupted via an update
     done outside of our control, we will fallback to the original
     RPI_EFI.FD variables. AKA we should never really be worse off.


  Arguments:
     None

  Returns:
     A value that can be assigned to mFvInstance->FlashOffset
     as the location we may right, otherwise 0.
--*/
{
  UINT32 total_data;
  UINT32 device_size;
  UINT8 buffer[32];

  device_size = ReadDeviceId ();
  // newer write location 00051100

  for (total_data = 0; total_data < device_size; ) {
    UINT32 len;
    if (ReadSpi (total_data, buffer, 24) == 24) {
      len = 0;
      len += (*(UINT8 *)&buffer[5]) << 16;
      len += (*(UINT8 *)&buffer[6]) << 8;
      len += (*(UINT8 *)&buffer[7]);

      // round up to nearest 8 byte align
      len += 7;
      len &= 0xFFFFF8;

      buffer[24]=0;
      DEBUG ((DEBUG_INFO, "%X len=%d filename=%a \n", *(UINT32 *)buffer,
              len, (char *)&buffer[8]));
      if (*(UINT32 *)buffer == 0xFFFFFFFF) {
        break;
      }
      total_data += 8 + len;
    } else {
      DEBUG ((DEBUG_ERROR, "Didn't get correct amount of data from SPI, abort its use\n"));
      return 0;
    }
  }

  DEBUG ((DEBUG_INFO, "First free sector at %X free space remaining %dK \n", total_data,(device_size-total_data)/1024));
  if ((device_size - total_data) > SIZE_128KB) {
    //start at the next 4k page
    total_data = (total_data + SIZE_4KB) & 0xFFFFE000;
    DEBUG ((DEBUG_INFO, "Start of Fv at %X\n", total_data));
  } else {
    total_data = 0;
  }

  return total_data;
}


STATIC
INT32
FlashRead (
    UINT32 Addr,
    UINT8 *Buffer,
    UINT32 Len)
/*++

  Routine Description:
     Reads Len data from variable storage area into Buffer

  Arguments:
     Addr    -  Offset into variable store region
     Buffer  -  Buffer where data is returned, this
                buffer must be at least 5 bytes long to
                hold the command.
     Len     -  Number of bytes to read into Buffer

  Returns:
     Number of bytes read
--*/
{
  return ReadSpi (mFvInstance->FlashOffset + Addr, Buffer, Len);
}
#endif

STATIC
VOID
DisableSpiWp (VOID)
/*++

  Routine Description:
     Sends SPI flash command to disable write protection

  Arguments:

  Returns:

--*/
{
  UINT8 Buffer[32];
  Buffer[0] = 0x06;

  DoSpiCommand (Buffer, 0, 1);
}

STATIC
INT32
ReadSpiStatus (VOID)
/*++

  Routine Description:
     Sends SPI get status command
  Arguments:

  Returns:
     Status of flash device
--*/
{
  UINT8 Buffer[32];
  Buffer[0] = 0x05;

  DoSpiCommand (Buffer, 1, 1);

  return Buffer[0];
}


STATIC
VOID
WriteSpi (
    UINT32 Addr,
    UINT8 *SrcBuffer,
    UINT32 Len)
/*++

  Routine Description:
     Writes Len bytes of SrcBuffer to SPI flash. The max write len is a single
     256 byte block, but this routine deals with the case where its misaligned
     across two flash blocks.

  Arguments:
     Addr       -  Flash device address
     SrcBuffer  -  Buffer from which data is written to the SPI flash
     Len        -  Number of bytes to write

  Returns:
     Nothing
--*/
{
  UINT8 Buffer[280];
  UINTN loop;
  int additional = 0;

  if (Len > 256) Len = 256;

  // check if request crosses boundary
  if (((Addr + Len - 1) & 0xFFFFFF00) != (Addr & 0xFFFFFF00)) {
    additional = (Addr + Len) & 0xFF;
    Len -= additional;
  }

  do {

    DisableSpiWp ();

    while (ReadSpiStatus () != 2) {
      DEBUG ((DEBUG_INFO, "Spi status %X \n", ReadSpiStatus ()));
    }

    Buffer[0] = 0x02; //write len
    Buffer[1] = (Addr >> 16) & 0xFF;
    Buffer[2] = (Addr >> 8) & 0xFF;
    Buffer[3] = Addr & 0xFF;

    CopyMem (&Buffer[4], SrcBuffer, Len);

    DoSpiCommand (Buffer, 0, 4+Len);

    loop = Len * 30000 * SPI_CS_READS_PER_US;
    while (ReadSpiStatus () & 0x3) {
      loop--;
      if (loop == 0) {
        DEBUG ((DEBUG_ERROR, "Write still busy, continue\n"));
        break;
      }
    }

    // deal with second block
    if (additional) {
      Addr += Len;
      SrcBuffer += Len;
      Len = additional;
      additional = 0;
    } else {
      Len = 0;
    }

  } while (Len);
}


STATIC
VOID
Erase4kSpi (
    UINT32 Addr)
/*++

  Routine Description:
     Erases a complete SPI flash page, which in this
     case is 4k at the given address.

  Arguments:
     Addr       -  Flash device address

  Returns:
     Nothing
--*/
{
  UINT8 Buffer[32];
  int loop = 300000 * SPI_CS_READS_PER_US;

  DisableSpiWp ();

  Buffer[0] = 0x20; //erase 4k
  Buffer[1] = (Addr >> 16) & 0xFF;
  Buffer[2] = (Addr >> 8) & 0xFF;
  Buffer[3] = Addr & 0xFF;

  DoSpiCommand (Buffer, 0, 4);

  while (ReadSpiStatus () & 0x3) {
    loop--;
    if (loop == 0) {
      DEBUG ((DEBUG_ERROR, "Erase still busy \n"));
      break;
    }
  }
}

STATIC
VOID
FlashWrite (
  IN     UINTN Address,
  IN     UINT8 *Buffer,
  IN     UINTN NumBytes
  )
/*++

  Routine Description:
     Writes Len bytes of SrcBuffer to flash variable storage. This routine breaks
     the writes into blocks <= 256 bytes, which is the max that can be written with
     the SPI flash commands we are using.

  Arguments:
     Address  - Variable store offset
     Buffer   - data buffer to write
     NumBytes - bytes in buffer to write

  Returns:

--*/
{
  UINTN Off=Address;

  while (NumBytes>0) {
    int write_bytes = NumBytes;
    if (write_bytes > 256) {
      write_bytes = 256;
    }
    WriteSpi (mFvInstance->FlashOffset + Off, Buffer, write_bytes);

    Off += write_bytes;
    Buffer += write_bytes;
    NumBytes -= write_bytes;
  }
}


EFI_STATUS
VarStoreWrite (
  IN     UINTN Address,
  IN OUT UINTN *NumBytes,
  IN     UINT8 *Buffer
  )
{

  if (Address<mFvInstance->FvBase) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem ((VOID*)Address, Buffer, *NumBytes);

  if (mFvInstance->FlashOffset) {
    GpioPinFuncSet (40, GPIO_FSEL_ALT4);
    GpioPinFuncSet (41, GPIO_FSEL_ALT4);

    FlashWrite (Address-mFvInstance->FvBase, Buffer, *NumBytes);

    GpioPinFuncSet (40, GPIO_FSEL_ALT0);
    GpioPinFuncSet (41, GPIO_FSEL_ALT0);
  } else {
    mFvInstance->Dirty = TRUE;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
FlashErase (
  IN UINTN Address,
  IN UINTN LbaLength
  )

{
  UINTN Off=Address;

  while (LbaLength>0) {
    int erase_bytes = LbaLength;
    if (erase_bytes > 4096) {
      erase_bytes = 4096;
    }
    Erase4kSpi (mFvInstance->FlashOffset + Off);

    Off += erase_bytes;
    LbaLength -= erase_bytes;
  }

  return EFI_SUCCESS;
}


EFI_STATUS
VarStoreErase (
  IN UINTN Address,
  IN UINTN LbaLength
  )
{
  if (Address<mFvInstance->FvBase) {
    return EFI_INVALID_PARAMETER;
  }
  SetMem ((VOID*)Address, LbaLength, 0xff);

  if (mFvInstance->FlashOffset) {
    GpioPinFuncSet (40, GPIO_FSEL_ALT4);
    GpioPinFuncSet (41, GPIO_FSEL_ALT4);

    FlashErase (Address-mFvInstance->FvBase, LbaLength);

    GpioPinFuncSet (40, GPIO_FSEL_ALT0);
    GpioPinFuncSet (41, GPIO_FSEL_ALT0);
  } else {
    mFvInstance->Dirty = TRUE;
  }

  return EFI_SUCCESS;
}


EFI_STATUS
FvbGetVolumeAttributes (
  OUT EFI_FVB_ATTRIBUTES_2 *Attributes
  )
{
  *Attributes = mFvInstance->VolumeHeader->Attributes;
  return EFI_SUCCESS;
}


EFI_STATUS
FvbGetLbaAddress (
  IN  EFI_LBA Lba,
  OUT UINTN *LbaAddress,
  OUT UINTN *LbaLength,
  OUT UINTN *NumOfBlocks
  )
/*++

  Routine Description:
    Retrieves the starting address of an LBA in an FV

  Arguments:
    Lba                   - The logical block address
    LbaAddress            - On output, contains the physical starting address
                            of the Lba
    LbaLength             - On output, contains the length of the block
    NumOfBlocks           - A pointer to a caller allocated UINTN in which the
                            number of consecutive blocks starting with Lba is
                            returned. All blocks in this range have a size of
                            BlockSize

  Returns:
    EFI_SUCCESS
    EFI_INVALID_PARAMETER

--*/
{

  UINT32 NumBlocks;
  UINT32 BlockLength;
  UINTN Offset;
  EFI_LBA StartLba;
  EFI_LBA NextLba;
  EFI_FV_BLOCK_MAP_ENTRY *BlockMap;

  StartLba = 0;
  Offset = 0;
  BlockMap = &(mFvInstance->VolumeHeader->BlockMap[0]);

  //
  // Parse the blockmap of the FV to find which map entry the Lba belongs to.
  //
  while (TRUE) {
    if (BlockMap->NumBlocks==0xFFFFFFFF) {
      NumBlocks = mFvInstance->NumOfBlocks;
    } else {
      NumBlocks = BlockMap->NumBlocks;
    }
    if (BlockMap->Length==0xFFFFFFFF) {
      BlockLength = mFvInstance->BlockSize;
    } else {
      BlockLength = BlockMap->Length;
    }

    if (NumBlocks == 0 || BlockLength == 0) {
      return EFI_INVALID_PARAMETER;
    }

    NextLba = StartLba + NumBlocks;

    //
    // The map entry found.
    //
    if (Lba >= StartLba && Lba < NextLba) {
      Offset = Offset + (UINTN)MultU64x32 ((Lba - StartLba), BlockLength);
      if (LbaAddress != NULL) {
        *LbaAddress = mFvInstance->FvBase + Offset;
      }

      if (LbaLength != NULL) {
        *LbaLength = BlockLength;
      }

      if (NumOfBlocks != NULL) {
        *NumOfBlocks = (UINTN)(NextLba - Lba);
      }

      return EFI_SUCCESS;
    }

    StartLba = NextLba;
    Offset = Offset + NumBlocks * BlockLength;
    BlockMap++;
  }

}


EFI_STATUS
FvbEraseBlock (
  IN EFI_LBA Lba
  )
/*++

Routine Description:
  Erases and initializes a firmware volume block

Arguments:
  Lba                   - The logical block index to be erased

Returns:
  EFI_SUCCESS           - The erase request was successfully completed
  EFI_ACCESS_DENIED     - The firmware volume is in the WriteDisabled state
  EFI_DEVICE_ERROR      - The block device is not functioning correctly and
                          could not be written. Firmware device may have been
                          partially erased
  EFI_INVALID_PARAMETER

--*/
{
  EFI_FVB_ATTRIBUTES_2 Attributes;
  UINTN                LbaAddress;
  UINTN                LbaLength;
  EFI_STATUS           Status;

  //
  // Check if the FV is write enabled
  //
  FvbGetVolumeAttributes (&Attributes);

  if ((Attributes & EFI_FVB2_WRITE_STATUS) == 0) {
    return EFI_ACCESS_DENIED;
  }
  //
  // Get the starting address of the block for erase. For debug reasons,
  // LbaWriteAddress may not be the same as LbaAddress.
  //
  Status = FvbGetLbaAddress (Lba, &LbaAddress, &LbaLength, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return VarStoreErase (
           LbaAddress,
           LbaLength
         );
}


EFI_STATUS
FvbSetVolumeAttributes (
  IN OUT EFI_FVB_ATTRIBUTES_2 *Attributes
  )
/*++

  Routine Description:
    Modifies the current settings of the firmware volume according to the
    input parameter, and returns the new setting of the volume

  Arguments:
    Attributes            - On input, it is a pointer to EFI_FVB_ATTRIBUTES_2
                            containing the desired firmware volume settings.
                            On successful return, it contains the new setting.

  Returns:
    EFI_SUCCESS           - Successfully returns
    EFI_ACCESS_DENIED     - The volume setting is locked and cannot be modified
    EFI_INVALID_PARAMETER

--*/
{
  EFI_FVB_ATTRIBUTES_2 OldAttributes;
  EFI_FVB_ATTRIBUTES_2 *AttribPtr;
  UINT32 Capabilities;
  UINT32 OldStatus;
  UINT32 NewStatus;
  EFI_FVB_ATTRIBUTES_2 UnchangedAttributes;

  AttribPtr =
    (EFI_FVB_ATTRIBUTES_2*) &(mFvInstance->VolumeHeader->Attributes);
  OldAttributes = *AttribPtr;
  Capabilities = OldAttributes & (EFI_FVB2_READ_DISABLED_CAP | \
                                  EFI_FVB2_READ_ENABLED_CAP |    \
                                  EFI_FVB2_WRITE_DISABLED_CAP |  \
                                  EFI_FVB2_WRITE_ENABLED_CAP |   \
                                  EFI_FVB2_LOCK_CAP              \
                                  );
  OldStatus = OldAttributes & EFI_FVB2_STATUS;
  NewStatus = *Attributes & EFI_FVB2_STATUS;

  UnchangedAttributes = EFI_FVB2_READ_DISABLED_CAP  | \
                        EFI_FVB2_READ_ENABLED_CAP   | \
                        EFI_FVB2_WRITE_DISABLED_CAP | \
                        EFI_FVB2_WRITE_ENABLED_CAP  | \
                        EFI_FVB2_LOCK_CAP           | \
                        EFI_FVB2_STICKY_WRITE       | \
                        EFI_FVB2_MEMORY_MAPPED      | \
                        EFI_FVB2_ERASE_POLARITY     | \
                        EFI_FVB2_READ_LOCK_CAP      | \
                        EFI_FVB2_WRITE_LOCK_CAP     | \
                        EFI_FVB2_ALIGNMENT;

  //
  // Some attributes of FV is read only can *not* be set.
  //
  if ((OldAttributes & UnchangedAttributes) ^ (*Attributes & UnchangedAttributes)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // If firmware volume is locked, no status bit can be updated.
  //
  if (OldAttributes & EFI_FVB2_LOCK_STATUS) {
    if (OldStatus ^ NewStatus) {
      return EFI_ACCESS_DENIED;
    }
  }

  //
  // Test read disable.
  //
  if ((Capabilities & EFI_FVB2_READ_DISABLED_CAP) == 0) {
    if ((NewStatus & EFI_FVB2_READ_STATUS) == 0) {
      return EFI_INVALID_PARAMETER;
    }
  }

  //
  // Test read enable.
  //
  if ((Capabilities & EFI_FVB2_READ_ENABLED_CAP) == 0) {
    if (NewStatus & EFI_FVB2_READ_STATUS) {
      return EFI_INVALID_PARAMETER;
    }
  }

  //
  // Test write disable.
  //
  if ((Capabilities & EFI_FVB2_WRITE_DISABLED_CAP) == 0) {
    if ((NewStatus & EFI_FVB2_WRITE_STATUS) == 0) {
      return EFI_INVALID_PARAMETER;
    }
  }

  //
  // Test write enable.
  //
  if ((Capabilities & EFI_FVB2_WRITE_ENABLED_CAP) == 0) {
    if (NewStatus & EFI_FVB2_WRITE_STATUS) {
      return EFI_INVALID_PARAMETER;
    }
  }

  //
  // Test lock.
  //
  if ((Capabilities & EFI_FVB2_LOCK_CAP) == 0) {
    if (NewStatus & EFI_FVB2_LOCK_STATUS) {
      return EFI_INVALID_PARAMETER;
    }
  }

  *AttribPtr = (*AttribPtr) & (0xFFFFFFFF & (~EFI_FVB2_STATUS));
  *AttribPtr = (*AttribPtr) | NewStatus;
  *Attributes = *AttribPtr;

  if (mFvInstance->FlashOffset) {
    GpioPinFuncSet (40, GPIO_FSEL_ALT4);
    GpioPinFuncSet (41, GPIO_FSEL_ALT4);

    FlashErase (0, 0x1000);
    FlashWrite (0, (UINT8*)mFvInstance->VolumeHeader, 0x1000);

    GpioPinFuncSet (40, GPIO_FSEL_ALT0);
    GpioPinFuncSet (41, GPIO_FSEL_ALT0);

  }


  return EFI_SUCCESS;
}


EFI_STATUS
EFIAPI
FvbProtocolGetPhysicalAddress (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *This,
  OUT EFI_PHYSICAL_ADDRESS *Address
  )
{
  *Address = mFvInstance->FvBase;
  return EFI_SUCCESS;
}


EFI_STATUS
EFIAPI
FvbProtocolGetBlockSize (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *This,
  IN CONST EFI_LBA Lba,
  OUT UINTN *BlockSize,
  OUT UINTN *NumOfBlocks
  )
/*++

  Routine Description:
    Retrieve the size of a logical block

  Arguments:
    This                  - Calling context
    Lba                   - Indicates which block to return the size for.
    BlockSize             - A pointer to a caller allocated UINTN in which
                            the size of the block is returned
    NumOfBlocks           - a pointer to a caller allocated UINTN in which the
                            number of consecutive blocks starting with Lba is
                            returned. All blocks in this range have a size of
                            BlockSize

  Returns:
    EFI_SUCCESS           - The firmware volume was read successfully and
                            contents are in Buffer

--*/
{
  EFI_STATUS Status;

  Status = FvbGetLbaAddress (
      Lba,
      NULL,
      BlockSize,
      NumOfBlocks
      );

  return Status;
}


EFI_STATUS
EFIAPI
FvbProtocolGetAttributes (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *This,
  OUT EFI_FVB_ATTRIBUTES_2 *Attributes
  )
/*++

  Routine Description:
      Retrieves Volume attributes.  No polarity translations are done.

  Arguments:
      This                - Calling context
      Attributes          - output buffer which contains attributes

  Returns:
    EFI_SUCCESS           - Successfully returns

--*/
{
  return FvbGetVolumeAttributes (Attributes);
}


EFI_STATUS
EFIAPI
FvbProtocolSetAttributes (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *This,
  IN OUT EFI_FVB_ATTRIBUTES_2 *Attributes
  )
/*++

  Routine Description:
    Sets Volume attributes. No polarity translations are done.

  Arguments:
    This                  - Calling context
    Attributes            - output buffer which contains attributes

  Returns:
    EFI_SUCCESS           - Successfully returns

--*/
{
  return FvbSetVolumeAttributes (Attributes);
}


EFI_STATUS
EFIAPI
FvbProtocolEraseBlocks (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL*This,
  ...
  )
/*++

  Routine Description:

    The EraseBlock() function erases one or more blocks as denoted by the
    variable argument list. The entire parameter list of blocks must be
    verified prior to erasing any blocks.  If a block is requested that does
    not exist within the associated firmware volume (it has a larger index than
    the last block of the firmware volume), the EraseBlock() function must
    return EFI_INVALID_PARAMETER without modifying the contents of the firmware
    volume.

  Arguments:
    This                  - Calling context
    ...                   - Starting LBA followed by Number of Lba to erase.
                            a -1 to terminate the list.

  Returns:
    EFI_SUCCESS           - The erase request was successfully completed
    EFI_ACCESS_DENIED     - The firmware volume is in the WriteDisabled state
    EFI_DEVICE_ERROR      - The block device is not functioning correctly and
                            could not be written. Firmware device may have been
                            partially erased

--*/
{
  UINTN NumOfBlocks;
  VA_LIST args;
  EFI_LBA StartingLba;
  UINTN NumOfLba;
  EFI_STATUS Status;

  NumOfBlocks = mFvInstance->NumOfBlocks;
  VA_START (args, This);

  do {
    StartingLba = VA_ARG (args, EFI_LBA);
    if (StartingLba == EFI_LBA_LIST_TERMINATOR) {
      break;
    }

    NumOfLba = VA_ARG (args, UINTN);

    if ((NumOfLba == 0) || ((StartingLba + NumOfLba) > NumOfBlocks)) {
      VA_END (args);
      return EFI_INVALID_PARAMETER;
    }
  } while (1);

  VA_END (args);

  VA_START (args, This);
  do {
    StartingLba = VA_ARG (args, EFI_LBA);
    if (StartingLba == EFI_LBA_LIST_TERMINATOR) {
      break;
    }

    NumOfLba = VA_ARG (args, UINTN);

    while (NumOfLba > 0) {
      Status = FvbEraseBlock (StartingLba);
      if (EFI_ERROR (Status)) {
        VA_END (args);
        return Status;
      }

      StartingLba++;
      NumOfLba--;
    }

  } while (1);

  VA_END (args);

  return EFI_SUCCESS;
}


EFI_STATUS
EFIAPI
FvbProtocolWrite (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *This,
  IN       EFI_LBA Lba,
  IN       UINTN Offset,
  IN OUT   UINTN *NumBytes,
  IN       UINT8 *Buffer
  )
/*++

  Routine Description:

    Writes data beginning at Lba:Offset from FV. The write terminates either
    when *NumBytes of data have been written, or when a block boundary is
    reached.  *NumBytes is updated to reflect the actual number of bytes
    written. The write opertion does not include erase. This routine will
    attempt to write only the specified bytes. If the writes do not stick,
    it will return an error.

  Arguments:
    This                  - Calling context
    Lba                   - Block in which to begin write
    Offset                - Offset in the block at which to begin write
    NumBytes              - On input, indicates the requested write size. On
                            output, indicates the actual number of bytes
                            written
    Buffer                - Buffer containing source data for the write.

  Returns:
    EFI_SUCCESS           - The firmware volume was written successfully
    EFI_BAD_BUFFER_SIZE   - Write attempted across a LBA boundary. On output,
                            NumBytes contains the total number of bytes
                            actually written
    EFI_ACCESS_DENIED     - The firmware volume is in the WriteDisabled state
    EFI_DEVICE_ERROR      - The block device is not functioning correctly and
                            could not be written
    EFI_INVALID_PARAMETER - NumBytes or Buffer are NULL

--*/
{
  EFI_FVB_ATTRIBUTES_2 Attributes;
  UINTN LbaAddress;
  UINTN LbaLength;
  EFI_STATUS Status = EFI_SUCCESS;
  EFI_STATUS ReturnStatus;

  //
  // Check for invalid conditions.
  //
  if ((NumBytes == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (*NumBytes == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Status = FvbGetLbaAddress (Lba, &LbaAddress, &LbaLength, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Check if the FV is write enabled.
  //
  FvbGetVolumeAttributes (&Attributes);

  if ((Attributes & EFI_FVB2_WRITE_STATUS) == 0) {
    return EFI_ACCESS_DENIED;
  }

  //
  // Perform boundary checks and adjust NumBytes.
  //
  if (Offset > LbaLength) {
    return EFI_INVALID_PARAMETER;
  }

  // forces this write to split
  if (LbaLength < (*NumBytes + Offset)) {
    *NumBytes = (UINT32)(LbaLength - Offset);
    Status = EFI_BAD_BUFFER_SIZE;
  }

  ReturnStatus = VarStoreWrite (
                   LbaAddress + Offset,
                   NumBytes,
                   Buffer
                 );
  if (EFI_ERROR (ReturnStatus)) {
    return ReturnStatus;
  }

  return Status;
}


EFI_STATUS
EFIAPI
FvbProtocolRead (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *This,
  IN CONST EFI_LBA Lba,
  IN CONST UINTN Offset,
  IN OUT UINTN *NumBytes,
  IN UINT8 *Buffer
  )
/*++

  Routine Description:

    Reads data beginning at Lba:Offset from FV. The Read terminates either
    when *NumBytes of data have been read, or when a block boundary is
    reached.  *NumBytes is updated to reflect the actual number of bytes
    written. The write opertion does not include erase. This routine will
    attempt to write only the specified bytes. If the writes do not stick,
    it will return an error.

  Arguments:
    This                  - Calling context
    Lba                   - Block in which to begin Read
    Offset                - Offset in the block at which to begin Read
    NumBytes              - On input, indicates the requested write size. On
                            output, indicates the actual number of bytes Read
    Buffer                - Buffer containing source data for the Read.

  Returns:
    EFI_SUCCESS           - The firmware volume was read successfully and
                            contents are in Buffer
    EFI_BAD_BUFFER_SIZE   - Read attempted across a LBA boundary. On output,
                            NumBytes contains the total number of bytes
                            returned in Buffer
    EFI_ACCESS_DENIED     - The firmware volume is in the ReadDisabled state
    EFI_DEVICE_ERROR      - The block device is not functioning correctly and
                            could not be read
    EFI_INVALID_PARAMETER - NumBytes or Buffer are NULL

--*/
{
  EFI_FVB_ATTRIBUTES_2 Attributes;
  UINTN LbaAddress;
  UINTN LbaLength;
  EFI_STATUS Status;

  //
  // Check for invalid conditions.
  //
  if ((NumBytes == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (*NumBytes == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Status = FvbGetLbaAddress (Lba, &LbaAddress, &LbaLength, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Check if the FV is read enabled.
  //
  FvbGetVolumeAttributes (&Attributes);

  if ((Attributes & EFI_FVB2_READ_STATUS) == 0) {
    return EFI_ACCESS_DENIED;
  }

  //
  // Perform boundary checks and adjust NumBytes.
  //
  if (Offset > LbaLength) {
    return EFI_INVALID_PARAMETER;
  }

  if (LbaLength < (*NumBytes + Offset)) {
    *NumBytes = (UINT32)(LbaLength - Offset);
    Status = EFI_BAD_BUFFER_SIZE;
  }

  CopyMem (Buffer, (VOID*)(LbaAddress + Offset), (UINTN)*NumBytes);

  return Status;
}


EFI_STATUS
ValidateFvHeader (
  IN EFI_FIRMWARE_VOLUME_HEADER *FwVolHeader
  )
/*++

  Routine Description:
    Check the integrity of firmware volume header

  Arguments:
    FwVolHeader           - A pointer to a firmware volume header

  Returns:
    EFI_SUCCESS           - The firmware volume is consistent
    EFI_NOT_FOUND         - The firmware volume has corrupted. So it is not an
                            FV

--*/
{
  UINT16 Checksum;

  //
  // Verify the header revision, header signature, length
  // Length of FvBlock cannot be 2**64-1
  // HeaderLength cannot be an odd number.
  //
  if ((FwVolHeader->Revision != EFI_FVH_REVISION) ||
      (FwVolHeader->Signature != EFI_FVH_SIGNATURE) ||
      (FwVolHeader->FvLength == ((UINTN)-1)) ||
      ((FwVolHeader->HeaderLength & 0x01) != 0)
      ) {
    return EFI_NOT_FOUND;
  }

  //
  // Verify the header checksum.
  //

  Checksum = CalculateSum16 ((UINT16*)FwVolHeader, FwVolHeader->HeaderLength);
  if (Checksum != 0) {
    UINT16 Expected;

    Expected =
      (UINT16)(((UINTN)FwVolHeader->Checksum + 0x10000 - Checksum) & 0xffff);

    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}




EFI_STATUS
EFIAPI
FvbInitialize (
  IN EFI_HANDLE         ImageHandle,
  IN EFI_SYSTEM_TABLE   *SystemTable
  )
/*++

  Routine Description:
    This function does common initialization for FVB services

  Arguments:

  Returns:

--*/
{
  EFI_STATUS Status;
  UINT32 BufferSize;
  EFI_FV_BLOCK_MAP_ENTRY *PtrBlockMapEntry;
  EFI_FW_VOL_BLOCK_DEVICE *FvbDevice;
  UINT32 MaxLbaSize;
  EFI_PHYSICAL_ADDRESS BaseAddress;
  UINTN Length;
  UINTN NumOfBlocks;
  RETURN_STATUS PcdStatus;
  UINTN StartOffset;
  EFI_FIRMWARE_VOLUME_HEADER SpiBuffer[2];

  BaseAddress = PcdGet32 (PcdNvStorageVariableBase);
  Length = (FixedPcdGet32 (PcdFlashNvStorageVariableSize) +
    FixedPcdGet32 (PcdFlashNvStorageFtwWorkingSize) +
    FixedPcdGet32 (PcdFlashNvStorageFtwSpareSize) +
    FixedPcdGet32 (PcdNvStorageEventLogSize));
  StartOffset = BaseAddress - FixedPcdGet64 (PcdFdBaseAddress);


  BufferSize = sizeof (EFI_FW_VOL_INSTANCE);

  mFvInstance = AllocateRuntimeZeroPool (BufferSize);
  if (mFvInstance == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  mFvInstance->FvBase = (UINTN)BaseAddress;
  mFvInstance->FvLength = (UINTN)Length;
  mFvInstance->BlockSize = FixedPcdGet32 (PcdFirmwareBlockSize);
  mFvInstance->Offset = StartOffset;  // Start offset of RPI_EFI.FD file
  /*
   * Should I parse config.txt instead and find the real name?
   */
  mFvInstance->MappedFile = L"RPI_EFI.FD";
  /*
   * SPI Control
   */
  mFvInstance->SpiBase = BCM2836_SPI0_BASE_ADDRESS;
  mFvInstance->FlashOffset = 0;
  mFvInstance->DisableRuntime = 0;

  ZeroMem (SpiBuffer, sizeof (EFI_FIRMWARE_VOLUME_HEADER) * 2);
#if (RPI_MODEL == 4)
  /*
   * On the RPI4 there is a 512KB flash chip
   * used to store the low level bcm2711 bootstrap code
   * and the XHCI firmware on newer devices. It has between
   * ~330K and ~180K available depending on model/version
   * Possibly less as its consumed for other purposes.
   * It shares a GPIO pin with the 3.5mm audio port (pwm)
   * so accessing it causes pops, shzzzz and bzzz
   * noices that are sometimes audible even without us.
   * During reboot for example. Newer TFA's will presumably
   * help us out and disable the audio amp at shutdown
   * and leave it disabled during startup. That means
   * that this code can bzzzz if TFA hasn't assured the
   * audio is off for us.
   *
   * Runtime audio pops are audible although barely noticable
   * in Windows and DT based linux boots. Although the larger
   * problem is configuring the GPIO pins. As such we disable
   * runtime persistance if either DT boot mode, or ACPI GPIO
   * is enabled.
   */

  GpioPinFuncSet (40, GPIO_FSEL_ALT4);
  GpioPinFuncSet (41, GPIO_FSEL_ALT4);
  GpioPinFuncSet (42, GPIO_FSEL_ALT4);
  GpioPinFuncSet (43, GPIO_FSEL_ALT4);
  GpioPinFuncSet (44, GPIO_FSEL_ALT4);
  GpioPinFuncSet (45, GPIO_FSEL_ALT4);

  GpioSetPull (43, GPIO_PULL_DOWN);
  GpioSetPull (44, GPIO_PULL_DOWN);
  GpioSetPull (45, GPIO_PULL_DOWN);

  mFvInstance->FlashOffset = WalkFlashVolume ();

  if (mFvInstance->FlashOffset) {
    if (FlashRead (0, (UINT8*)SpiBuffer, sizeof (EFI_FIRMWARE_VOLUME_HEADER)*2)
        != sizeof (EFI_FIRMWARE_VOLUME_HEADER) * 2) {
      DEBUG ((DEBUG_ERROR, "Unable to read data from SPI\n"));
      mFvInstance->FlashOffset = 0;
    } else  {
      Status = ValidateFvHeader (SpiBuffer);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_INFO, "Invalid header on SPI, recreate volume\n"));
        FlashErase (0, Length);
        FlashWrite (0, (UINT8*)BaseAddress, Length);
      }

      // read the entire varstore...
      if (FlashRead (0, (UINT8*)BaseAddress, Length) != Length) {
        DEBUG ((DEBUG_ERROR, "Failed to read entire flash region\n"));
      }
    }
  }
  GpioPinFuncSet (40, GPIO_FSEL_ALT0);
  GpioPinFuncSet (41, GPIO_FSEL_ALT0);
#endif

  Status = ValidateFvHeader (mFvInstance->VolumeHeader);
  if (!EFI_ERROR (Status)) {
    if (mFvInstance->VolumeHeader->FvLength != Length ||
        mFvInstance->VolumeHeader->BlockMap[0].Length !=
        PcdGet32 (PcdFirmwareBlockSize)) {
      Status = EFI_VOLUME_CORRUPTED;
    }
  }
  if (EFI_ERROR (Status)) {
    EFI_FIRMWARE_VOLUME_HEADER *GoodFwVolHeader;
    UINTN WriteLength;

    DEBUG ((DEBUG_INFO,
      "Variable FV header is not valid. It will be reinitialized.\n"));

    //
    // Get FvbInfo
    //
    Status = GetFvbInfo (Length, &GoodFwVolHeader);
    ASSERT_EFI_ERROR (Status);

    //
    // Erase all the blocks
    //
    Status = VarStoreErase ((UINTN)mFvInstance->FvBase, mFvInstance->FvLength);
    ASSERT_EFI_ERROR (Status);
    //
    // Write good FV header
    //
    WriteLength = GoodFwVolHeader->HeaderLength;
    Status = VarStoreWrite ((UINTN)mFvInstance->FvBase, &WriteLength,
               (UINT8*)GoodFwVolHeader);
    ASSERT_EFI_ERROR (Status);
    ASSERT (WriteLength == GoodFwVolHeader->HeaderLength);

    Status = ValidateFvHeader (mFvInstance->VolumeHeader);
    ASSERT_EFI_ERROR (Status);
  }

  MaxLbaSize = 0;
  NumOfBlocks = 0;
  for (PtrBlockMapEntry = mFvInstance->VolumeHeader->BlockMap;
       PtrBlockMapEntry->NumBlocks != 0;
       PtrBlockMapEntry++) {
    //
    // Get the maximum size of a block.
    //
    if (MaxLbaSize < PtrBlockMapEntry->Length) {
      MaxLbaSize = PtrBlockMapEntry->Length;
    }

    NumOfBlocks = NumOfBlocks + PtrBlockMapEntry->NumBlocks;
  }

  //
  // The total number of blocks in the FV. This should match:
  // PcdFlashNvStorageVariableSize + PcdFlashNvStorageFtwWorkingSize +
  // PcdFlashNvStorageFtwSpareSize + PcdNvStorageEventLogSize / PcdFirmwareBlockSize
  //
  mFvInstance->NumOfBlocks = NumOfBlocks;

  //
  // Add a FVB Protocol Instance
  //
  FvbDevice = AllocateRuntimePool (sizeof (EFI_FW_VOL_BLOCK_DEVICE));
  ASSERT (FvbDevice != NULL);
  CopyMem (FvbDevice, &mFvbDeviceTemplate, sizeof (EFI_FW_VOL_BLOCK_DEVICE));

  //
  // Set up the devicepath
  //
  if (mFvInstance->VolumeHeader->ExtHeaderOffset == 0) {
    FV_MEMMAP_DEVICE_PATH *FvMemmapDevicePath;

    //
    // FV does not contains extension header, then produce MEMMAP_DEVICE_PATH
    //
    FvMemmapDevicePath = AllocateCopyPool (sizeof (FV_MEMMAP_DEVICE_PATH),
                           &mFvMemmapDevicePathTemplate);
    FvMemmapDevicePath->MemMapDevPath.StartingAddress = mFvInstance->FvBase;
    FvMemmapDevicePath->MemMapDevPath.EndingAddress = mFvInstance->FvBase +
      mFvInstance->FvLength - 1;
    FvbDevice->DevicePath = (EFI_DEVICE_PATH_PROTOCOL*)FvMemmapDevicePath;
  } else {
    FV_PIWG_DEVICE_PATH *FvPiwgDevicePath;

    FvPiwgDevicePath = AllocateCopyPool (sizeof (FV_PIWG_DEVICE_PATH),
                         &mFvPIWGDevicePathTemplate);
    CopyGuid (&FvPiwgDevicePath->FvDevPath.FvName,
      (GUID*)(UINTN)(mFvInstance->FvBase + mFvInstance->VolumeHeader->ExtHeaderOffset));
    FvbDevice->DevicePath = (EFI_DEVICE_PATH_PROTOCOL*)FvPiwgDevicePath;
  }

  //
  // Module type specific hook.
  //
  InstallProtocolInterfaces (FvbDevice);

  //
  // Set several PCD values to point to flash.
  //
  PcdStatus = PcdSet64S (PcdFlashNvStorageVariableBase64,
                (UINTN)PcdGet32 (PcdNvStorageVariableBase));
  ASSERT_RETURN_ERROR (PcdStatus);
  PcdStatus = PcdSet32S (PcdFlashNvStorageFtwWorkingBase,
                PcdGet32 (PcdNvStorageFtwWorkingBase));
  ASSERT_RETURN_ERROR (PcdStatus);
  PcdStatus = PcdSet32S (PcdFlashNvStorageFtwSpareBase,
                PcdGet32 (PcdNvStorageFtwSpareBase));
  ASSERT_RETURN_ERROR (PcdStatus);

  InstallFSNotifyHandler ();
  InstallDumpVarEventHandlers ();
  InstallVirtualAddressChangeHandler ();

  return EFI_SUCCESS;
}
