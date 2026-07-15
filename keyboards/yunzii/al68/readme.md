# MK AL68


![AL68](https://i.imgur.com/mhiYDh3.png)

A customizable 68keys keyboard.

* Keyboard Maintainer: [Jeff](https://github.com/yunziikeyboard)
* Hardware Supported: [yunzii](https://www.yunzii.com)
* Hardware Availability: [yunzii](https://www.yunzii.com)

Make example for this keyboard (after setting up your build environment):

    make yunzii/al68:default

Flashing example for this keyboard:

    make yunzii/al68:default:flash

See the build environment setup and the make instructions for more information. Brand new to QMK? Start with our Complete Newbs Guide.

## Bootloader ESC the bootloader in 3 ways:
* **Bootmagic reset: Hold down Enter in the keyboard then replug
* **Physical reset button: Briefly press the button on the back of the PCB
* **Keycode in layout: Press the key mapped to QK_BOOT

## Credits

This port builds on the work of several people:

* [yunziikeyboard/qmk_firmware](https://github.com/yunziikeyboard/qmk_firmware) — Yunzii's official QMK fork (maintainer Jeff): the original AL68 port that everything below is based on.
* [karamanliev/qmk_firmware](https://github.com/karamanliev/qmk_firmware) — the bulk of this tree: the BT/2.4GHz wireless port (`al68.c`, `common/smart_ble.c`) and the battery ADC driver (`adc.c`), ported from the mk637 ODM reference code to upstream QMK.
* [djcastaldo/qmk_yunzii](https://github.com/djcastaldo/qmk_yunzii) — the DFU bootloader fix (clearing the `BKP->DR10` flag so the board doesn't get stuck in DFU after replugging).

Local additions on top: fixed mouse-over-BT (swapped arguments to `has_mouse_report_changed()` inherited from the ODM code), wireless channel persistence across power cycles, sleep/wake reliability fixes, and the BT status indicator.