/** @file
 *
 *  Secondary System Description Table (SSDT) for active (fan) cooling
 *
 *  Copyright (c) 2020, Arm Ltd. All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <IndustryStandard/Bcm2711.h>
#include <IndustryStandard/Bcm2836.h>
#include <IndustryStandard/Bcm2836Gpio.h>

#include <IndustryStandard/Acpi.h>

DefinitionBlock (__FILE__, "SSDT", 5, "RPIFDN", "RPITHFAN", 2)
{
  External (\_SB_.EC00, DeviceObj)
  External (\_SB_.EC00.TZ00, DeviceObj)

  Scope (\_SB_.EC00)
  {
    // Define a NameOp we will modify during InstallTable
    Name (GIOP, 0x2) //08 47 49 4f 50 0a 02 (value must be >1)
    Name (FTMP, 0x2)
    Name (FANX, 0)   // current fan speed

    // opregion for EL3 trap to rpi mailbox
    OperationRegion (FDBL, SystemMemory, 0xFF800080, 0x4)
    Field (FDBL, DWordAcc, NoLock, Preserve) {
      FSPD, 32
    }

    // opregion for direct GPIO control of fan
    OperationRegion (GPIO, SystemMemory, GPIO_BASE_ADDRESS, 0x1000)
    Field (GPIO, DWordAcc, NoLock, Preserve) {
      Offset (0x1C),
      GPS0, 32,
      GPS1, 32,
      RES1, 32,
      GPC0, 32,
      GPC1, 32,
      RES2, 32,
      GPL1, 32,
      GPL2, 32
    }


    // Describe a fan
    Device (FAN0) {
      // Note, an ACPIv4 fan
      // This is a variable speed fan definition, but in the case of
      // the GPIO based fan control all the "speeds" are the same.
      // For simplicity we are only defining 4 speeds, rather
      // than fine grained control.
      //
      // In the case of a GPIO fan we are
      // assuming that UEFI has programmed the
      // direction as OUT. Given the current limitations
      // on the GPIO pins, its recommended to use
      // the GPIO to switch a larger voltage/current
      // for the fan rather than driving it directly.

      Name (_HID, EISAID ("PNP0C0B"))
      Name (_UID, 0x0)

      Method (_STA) {
        Return (0xf)
      }

      Method (_FIF) {
         Return (
           Package() {  0, 0, 0, 0  } // No fine grain control, no step size, no notification
         )
      }
      
      Method (_FPS) {
        Return ( Package() {
          0,
          Package() {0,   0xFFFFFFF,   0, 0xFFFFFFF, 0xFFFFFFF }, // off
          Package() {50,  2, 200, 0xFFFFFFF, 0xFFFFFFF }, // single trip point  tied to AC2
          Package() {80, 1, 400, 0xFFFFFFF, 0xFFFFFFF }, // single trip point  tied to AC1
          Package() {100, 0, 800, 0xFFFFFFF, 0xFFFFFFF }  // single trip point  tied to AC0
        })
      }
      Method (_FSL, 1, Serialized) {
        Store (Arg0, FANX)
        if ( LEqual(GIOP, 0xFF)) {
          Store (0x40000000+Arg0, FSPD) //change the fan state
        } else {
          if (Arg0 == 0) {
            Store (1 << GIOP, GPC0) // off
          } else {
            Store (1 << GIOP, GPS0) // on
          }
        }       
      }
      Method (_FST) {
        Return ( 
          Package() {
            0, FANX, 0xFFFFFFFF
          }
        )
      }

    }
  }

  // merge in an active cooling point.
  Scope (\_SB_.EC00.TZ00)
  {

    Method (_AC2) { Return ( ((FTMP) * 10) + 2732) }    // (60C) active cooling trip point,
                                                        // if this is lower than PSV then we
                                                        // prefer active cooling
                                                        // This is either low
                                                        // speed (for variable speed fans)
                                                        // or on for fixed speed fans
    Name (_AL2, Package () { \_SB_.EC00.FAN0 })         // the fan used for AC2 above

    Method (_AC1) { Return ( ((FTMP+5) * 10) + 2732) }  // (+5C) active cooling trip point
    Name (_AL1, Package () { \_SB_.EC00.FAN0 })         // Middle fan speed
    Method (_AC0) { Return ( ((FTMP+10) * 10) + 2732) } // (+10C) active cooling trip point
    Name (_AL0, Package () { \_SB_.EC00.FAN0 })         // Max fan speed
  }
}
