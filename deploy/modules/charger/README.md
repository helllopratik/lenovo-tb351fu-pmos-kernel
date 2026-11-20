# pmos_charge_cap

User-selectable battery charge cap for the Lenovo TB351FU running pmOS,
built on the vendor `charger_class` API used by `sc8989x_charger.ko`.

## Behaviour

- `limit` is `full` (100 %) or a percentage in 50–100. The default is 80 %.
- At `limit`, `charger_dev_enable(false)` stops battery charging. The charger
  power path stays on, so the tablet runs from VBUS and the battery sits idle
  (verified: battery current drops to 0).
- On load the module enables the charger once for `CAP_SETTLE_MS` (3 s) so the
  VBUS power path comes up even when the cap is already reached at boot; the
  first monitor pass then re-applies the limit.
- Charging resumes at `limit - 5` (hysteresis) to avoid toggling at the limit.
  This matches stock Android, which stops at 80 % and recharges at 75 %
  (`recharge_setting=75` in `init.battery_protection.rc`).
- `input_current` defaults to 900000 µA.  The SC8989x input current limit is
  what makes charging possible on a 500 mA SDP port: under normal load the
  tablet draws ~650 mA, so 500 mA leaves the battery discharging.
- On unload the module re-enables charging.

## Build

The canonical build is the repository helper:

```sh
export PATH="/mnt/Nvme/evolutionx/sync/prebuilts/kernel-build-tools/linux-x86/bin:/mnt/Nvme/evolutionx/sync/prebuilts/clang/host/linux-x86/clang-r547379/bin:$PATH"
./deploy/modules/charger/build.sh
```

It builds against `out/` and produces `pmos_charge_cap.ko` next to the source.
`./deploy/update_modules_payload.sh` then refreshes `deploy/modules.tar.xz`
with the module under `charger/`. The vendor `.ko` files
(`charger_class.ko`, `sc8989x_charger.ko`, `pmos_chg_stub.ko`, …) are staged
locally from the stock vendor ramdisk and are git-ignored; only the tracked
payload ships them.

`charger_dev_*` symbols resolve at load time from `charger_class.ko`, so the
modpost "undefined" warnings are expected.

## Sysfs

`/sys/kernel/pmos_charge_cap/`

| File | Mode | Meaning |
|---|---|---|
| `limit` | 0644 | `full` or 50–100 |
| `capacity` | 0444 | current battery percentage |
| `charging` | 0444 | 1 while charging is enabled |
| `input_current` | 0644 | charger input current limit in µA |

Use `pmos-charge-cap full|80` (installed in `/usr/local/bin`) to change the
limit and persist it in `/etc/pmos-charge-cap.conf`.

## Boot integration

`init_charger.sh` loads the vendor stack and this module; it is started by
`pmos-charger.service` on every boot.  Do **not** unbind `mt6359-auxadc`
while the charger stack is loaded (see `pmos_kernel.md` §7.9).
