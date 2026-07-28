/* Flash layout for the SenseCAP T1000-E path: the card ships the Adafruit
 * UF2 bootloader with SoftDevice S140 v7.3.0, whose app base is 0x27000
 * (evidence: Meshtastic's shipped tracker-t1000-e board definition and its
 * nrf52840_s140_v7.ld; to be confirmed against the physical card at the
 * post-P2 hardware test). The Bramble build does not use the SoftDevice; the
 * 0x1000-0x27000 region simply stays unused until a (P3) bootloader-settings
 * decision reclaims it. LENGTH stops at 0xED000, where the bootloader's
 * InternalFileSystem region begins (0xED000-0xF4000), followed by the
 * bootloader itself. The dev kit uses the default nrfx script (app at 0x0,
 * SWD).
 */
MEMORY
{
  /* Ends at 0xC0000 where the settings filesystem begins
     (BRAMBLE_LFS_BASE in nrf/CMakeLists.txt). */
  FLASH (rx) : ORIGIN = 0x27000, LENGTH = 0x99000
  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x40000
}

INCLUDE "nrf_common.ld"
