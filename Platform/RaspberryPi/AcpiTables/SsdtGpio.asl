/** @file
 *
 *  Secondary System Description Table (SSDT) for the GPIO port
 *
 *  Copyright (c) 2021, Arm Ltd. All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/Bcm2711.h>
#include <IndustryStandard/Bcm2836.h>
#include <IndustryStandard/Bcm2836Gpio.h>

#include "AcpiTables.h"

#define BCM_ALT0 0x4
#define BCM_ALT1 0x5
#define BCM_ALT2 0x6
#define BCM_ALT3 0x7
#define BCM_ALT4 0x3
#define BCM_ALT5 0x2

DefinitionBlock (__FILE__, "SSDT", 5, "RPIFDN", "RPI3GPIO", 2)
{
  External (\_SB_.GDV0, DeviceObj)
  External (\_SB_.GDV0.RPIQ, DeviceObj)
  Scope (\_SB_.GDV0)
  {
    include ("Rhpx.asl")

    // Description: GPIO
    Device (GPI0)
    {
      Name (_HID, "BCM2845")
      Name (_CID, "BCM2845")
      Name (_UID, 0x0)
      Name (_CCA, 0x0)
      Method (_STA)
      {
        Return(0xf)
      }
      Name (RBUF, ResourceTemplate ()
      {
        MEMORY32FIXED (ReadWrite, 0, GPIO_LENGTH, RMEM)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared)
        {
          BCM2386_GPIO_INTERRUPT0, BCM2386_GPIO_INTERRUPT1,
          BCM2386_GPIO_INTERRUPT2, BCM2386_GPIO_INTERRUPT3
        }
      })
      Method (_CRS, 0x0, Serialized)
      {
        MEMORY32SETBASE (RBUF, RMEM, RBAS, GPIO_OFFSET)
        Return (^RBUF)
      }
    }

    // SPI
    Device (SPI0)
    {
      Name (_HID, "BCM2838")
      Name (_CID, "BCM2838")
      Name (_UID, 0x0)
      Name (_CCA, 0x0)
      Method (_STA)
      {
        Return (0xf)
      }
      Name (RBUF, ResourceTemplate ()
      {
        MEMORY32FIXED (ReadWrite, 0, BCM2836_SPI0_LENGTH, RMEM)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { BCM2836_SPI0_INTERRUPT }
        PinFunction (Exclusive, PullDown, BCM_ALT0, "\\_SB.GDV0.GPI0", 0, ResourceConsumer, , ) { 9, 10, 11 } // MISO, MOSI, SCLK
        PinFunction (Exclusive, PullUp, BCM_ALT0, "\\_SB.GDV0.GPI0", 0, ResourceConsumer, , ) { 8 } // CE0
        PinFunction (Exclusive, PullUp, BCM_ALT0, "\\_SB.GDV0.GPI0", 0, ResourceConsumer, , ) { 7 } // CE1
      })

      Method (_CRS, 0x0, Serialized)
      {
        MEMORY32SETBASE (RBUF, RMEM, RBAS, BCM2836_SPI0_OFFSET)
        Return (^RBUF)
      }
    }

    Device (SPI1)
    {
      Name (_HID, "BCM2839")
      Name (_CID, "BCM2839")
      Name (_UID, 0x1)
      Name (_CCA, 0x0)
      Name (_DEP, Package() { \_SB.GDV0.RPIQ })
      Method (_STA)
      {
        Return (0xf)
      }
      Name (RBUF, ResourceTemplate ()
      {
        MEMORY32FIXED (ReadWrite, 0, BCM2836_SPI1_LENGTH, RMEM)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared,) { BCM2836_SPI1_INTERRUPT }
        PinFunction (Exclusive, PullDown, BCM_ALT4, "\\_SB_.GDV0.GPI0", 0, ResourceConsumer, , ) { 19, 20, 21 } // MISO, MOSI, SCLK
        PinFunction (Exclusive, PullDown, BCM_ALT4, "\\_SB_.GDV0.GPI0", 0, ResourceConsumer, , ) { 16 } // CE2
      })

      Method (_CRS, 0x0, Serialized)
      {
        MEMORY32SETBASE (RBUF, RMEM, RBAS, BCM2836_SPI1_OFFSET)
        Return (^RBUF)
      }
    }

  // SPI2 has no pins on GPIO header
  // Device (SPI2)
  // {
  //   Name (_HID, "BCM2839")
  //   Name (_CID, "BCM2839")
  //   Name (_UID, 0x2)
  //   Name (_CCA, 0x0)
  //   Name (_DEP, Package() { \_SB.GDV0.RPIQ })
  //   Method (_STA)
  //   {
  //     Return (0xf)     // Disabled
  //   }
  //   Method (_CRS, 0x0, Serialized)
  //   {
  //     Name (RBUF, ResourceTemplate ()
  //     {
  //       MEMORY32FIXED (ReadWrite, BCM2836_SPI2_BASE_ADDRESS, BCM2836_SPI2_LENGTH, RMEM)
  //       Interrupt (ResourceConsumer, Level, ActiveHigh, Shared,) { BCM2836_SPI2_INTERRUPT }
  //     })
  //     Return (RBUF)
  //   }
  // }

    Device (I2C1)
    {
      Name (_HID, "BCM2841")
      Name (_CID, "BCM2841")
      Name (_UID, 0x1)
      Name (_CCA, 0x0)
      Method (_STA)
      {
        Return(0xf)
      }
      Name (RBUF, ResourceTemplate ()
      {
        MEMORY32FIXED (ReadWrite, 0, BCM2836_I2C1_LENGTH, RMEM)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { BCM2836_I2C1_INTERRUPT }
        PinFunction (Exclusive, PullUp, BCM_ALT0, "\\_SB_.GDV0.GPI0", 0, ResourceConsumer, , ) { 2, 3 }
      })
      Method (_CRS, 0x0, Serialized)
      {
        MEMORY32SETBASE (RBUF, RMEM, RBAS, BCM2836_I2C1_OFFSET)
        Return (^RBUF)
      }
    }
  }
}
