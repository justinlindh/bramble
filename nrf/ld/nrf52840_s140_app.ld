/* Flash layout for the SenseCAP T1000-E path: the card ships the Adafruit
 * UF2 bootloader, which expects the application at the ex-SoftDevice base
 * 0x26000. The Bramble build does not use the SoftDevice; the 0x1000-0x26000
 * region simply stays unused until a (P3) bootloader-settings decision
 * reclaims it. LENGTH stops short of the bootloader + settings region at the
 * top of flash. The dev kit uses the default nrfx script (app at 0x0, SWD).
 */
MEMORY
{
  FLASH (rx) : ORIGIN = 0x26000, LENGTH = 0xC2000
  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x40000
}

INCLUDE "nrf_common.ld"
