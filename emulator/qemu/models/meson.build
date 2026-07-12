# Bramble Phase 2 emulator device models (esp32s3 QEMU machine).
#
# Injected into the espressif/qemu tree at hw/xtensa/bramble/ by
# emulator/qemu/bootstrap-qemu.sh; hw/xtensa/meson.build reaches it with one
# `subdir('bramble')` line. This file (and the sources beside it) is owned by
# the bramble repo, so adding a model is one line HERE, never a patch to stock
# QEMU meson. `xtensa_ss` is the source set defined by the parent meson.build.
xtensa_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files(
  'bramble_scaffold.c',
  'bramble_attach.c',
  'bramble_gpio.c',
  'bramble_adc.c',
  'bramble_gpspi2.c',
  'bramble_sx1262.c',
  'bramble_ssd1680.c',
  'bramble_indicators.c',
  'bramble_emulink.c',
))
