# Legacy / manual fallback units

These units are **not** installed by `install_pmos_rootfs.sh` (it only copies
`deploy/services/*.service`).

- `pmos-refresh-90.service` + `deploy/scripts/pmos-refresh-90.sh`
  Old boot-time "force 90 Hz" unit. Superseded by
  `pmos-display-mode.service`, which applies the user's Mobile Settings
  choice (60 / 90 / adaptive). Keep it for manual recovery or a temporary
  boot-time 90 Hz test:
  `cp deploy/legacy/pmos-refresh-90.service /etc/systemd/system/`
  then `systemctl enable --now pmos-refresh-90.service`.
