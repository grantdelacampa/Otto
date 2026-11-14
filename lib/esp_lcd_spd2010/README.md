# esp_lcd_spd2010

Espressif's SPD2010 panel driver, unmodified, from the component registry
(`espressif/esp_lcd_spd2010` v2.0.0~1, Apache-2.0, see `LICENSE`). Only the source and
header are kept; `library.json` lets PlatformIO build it and supplies the version macros
the component's CMake would normally define.

The touch half of the SPD2010 is read directly in `src/touch.cpp`.
