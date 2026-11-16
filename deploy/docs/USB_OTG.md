# USB host / OTG

The MT6789 MUSB controller is now dual-role.  The stock DTB already contains
`dr_mode = "otg"`; `drivers/usb/musb/mediatek.c` honours it and registers a
standard `usb_role` switch instead of forcing peripheral-only mode.

## Manual role switch

```
sudo pmos-usb-role status
sudo pmos-usb-role host
sudo pmos-usb-role device
```

* `host` enables the SC8989x OTG boost (5 V VBUS), unbinds the configfs
  gadget and puts MUSB into host mode.  USB sticks, keyboards, serial
  adapters, webcams and USB audio devices enumerate normally; udisks2 mounts
  storage under `/run/media/user/`.
* `device` switches MUSB back to peripheral mode, re-binds the ACM+NCM
  gadget and disables VBUS.
* `none` leaves the controller idle.

VBUS is controlled by `pmos_otg_vbus.ko`, loaded by `init_charger.sh` after
the vendor charger stack:

```
/sys/kernel/pmos_otg_vbus/enable
```

The wrapper resolves `get_charger_by_name()` and `charger_dev_enable_otg()`
from `charger_class.ko` at load time and binds the charger named
`primary_chg` (SC8989x).

## Automatic role detection is not available yet

The MT6375 Type-C TCPC/charger driver loads but fails its device-ID check on
this board, so there is no working Type-C role/CC notification.  Role changes
are manual for now.  Do not expect cable insertion to switch modes by itself.

## Kernel support

Built in via `pmos_tb351fu.fragment`:

* USB storage + UAS, HID, video class, USB audio
* USB serial: FTDI, CP210x, CH341, PL2303, Option
* USB net: CDC Ethernet/NCM, RNDIS host, R8152, AX88179
* exFAT, f2fs, erofs, NTFS3
* MUSB dual role + `usb_role` class

## Type-C analog audio

Android's Type-C accessory stack reports `analog_audio` and `debug` accessory
modes.  pmOS does not implement the Type-C analog-audio accessory switch yet;
the 3.5 mm headset jack is handled by the MT6358/MT6366 codec and the UCM
profiles.  Treat Type-C analog audio as future work.
