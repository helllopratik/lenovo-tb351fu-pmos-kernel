# pmos_otg_vbus

Small helper that drives the SC8989x OTG boost through the vendor
`charger_class` API, giving the TB351FU a host-mode VBUS source.

## Behaviour

- Exposes `/sys/kernel/pmos_otg_vbus/enable` (0644).
- `get_charger_by_name("primary_chg")` and `charger_dev_enable_otg()` come from
  `charger_class.ko`; the module returns `-ENODEV` until `sc8989x_charger.ko`
  has registered `primary_chg`.
- `init_charger.sh` retries the load after the charger stack is up, then
  `pmos-usb-role` switches the role.

## Build

```sh
export PATH="/mnt/Nvme/evolutionx/sync/prebuilts/kernel-build-tools/linux-x86/bin:/mnt/Nvme/evolutionx/sync/prebuilts/clang/host/linux-x86/clang-r547379/bin:$PATH"
./deploy/modules/pmos_otg_vbus/build.sh
```

`update_modules_payload.sh` installs the result as
`/usr/lib/modules/extra/charger/pmos_otg_vbus.ko`. The `charger_dev_*` symbols
resolve at insmod time, so modpost "undefined" warnings are expected.
