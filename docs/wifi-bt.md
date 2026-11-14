# Wi-Fi and Bluetooth: the QCA6490

The Duo 2's Wi-Fi and Bluetooth are one Qualcomm QCA6490 (HSP). Wi-Fi sits on
PCIe (`1c00000.qcom,pcie`) behind the `cnss2` platform driver; Bluetooth is on
the high-speed UART `/dev/ttyHS0` (`a94000.qcom,qup_uart`), powered through
`btpower`. Both work on Microsoft's 5.4 kernel with two modules we build
ourselves, firmware read from the phone's own partitions, and two small
services.

## Why it did not work out of the box

- **No Wi-Fi driver.** Microsoft's kernel tree has `cnss2`, but not the
  qcacld-3.0 host driver (`wlan.ko`); that lives in a separate vendor tree.
  The stock `wlan.ko` cannot be loaded: its vermagic, signature and symbol
  CRCs belong to Microsoft's build, and this kernel enforces all three.
  `scripts/duo2-build-radio-modules.sh` builds it from CodeLinaro at the
  release the stock module reports (2.0.8.31N, `LA.UM.9.14.r1-21000`) and
  signs it with the vendor build's module key. The `qca-wifi-host-cmn` tag of
  that name on CodeLinaro points at a qcacld commit, so the script pins the
  matching host-cmn merge by hash instead.
- **cnss2 gates the driver on calibration.** Loading `wlan` before user space
  signals `fs_ready` gets "Reject WLAN Driver insmod before CBC". The order is
  set `wlan` subsystem restart level to RELATED (the default, SYSTEM, panics
  the phone on a firmware crash), enable `recovery`, write `fs_ready`, then
  `modprobe wlan`, retrying while cold-boot calibration starts. udev must not
  load `wlan` by itself, hence the `blacklist wlan`.
- **A missing firmware file stalls the chip.** The kernel has the firmware
  user-helper fallback, so a file that does not exist waits 60 s. cnss2 asks
  for a QDSS trace config that the phone does not have, and that wait was
  long enough to time out FW_READY and trigger recovery. A udev rule fails
  the fallback at once for the radio firmware names.
- **udev renamed an interface.** qcacld creates `wlan0`, `p2p0` and
  `wifi-aware0`, and the default naming policy turned `p2p0` into `wlp1s0`.
  A `.link` file keeps the driver's names.
- **No Bluetooth driver for this setup.** The kernel's `hci_qca` only knows
  serdev, which this kernel does not build, and does not know the HSP's
  firmware names. The Android stack does the bring-up in user space. So
  does `luma-duo2-bluetooth`: it votes the UART clock (`TIOCPMGET`), reads the
  version, switches to 3 Mbaud, downloads the rampatch (`hpbtfw10.tlv`) and
  NVM (`hpnv10.bin`, with the unit's address), then hands the tty to the
  kernel's QCA line discipline. That discipline is needed because the chip
  speaks in-band sleep (IBS) whatever the NVM says, which H4 cannot parse.
  `kernel/radio/hci_qca-ldisc.patch` lets `hci_qca` run as a plain line
  discipline: no serdev, skip its own setup, IBS on.
- **`btpower` has no modalias**, so nothing loads it; the service does. It
  exposes rfkill `bt_power`, which is the controller's power switch:
  blocking it (the GNOME toggle, `rfkill block bluetooth`) detaches and
  powers off, unblocking brings the controller up again.

## Per-unit data

Nothing unit-specific is in this repository or in the image. At boot,
`luma-duo2-radio-firmware.service` (`luma-duo2-radio-setup firmware`):

- mounts the active slot's `modem` and `bluetooth` partitions read-only
  under `/run/luma-duo2/firmware/` (Wi-Fi firmware, board data and
  regulatory database; Bluetooth rampatch and NVM);
- reads the Wi-Fi and Bluetooth addresses from the `persist` partition with
  `debugfs` (read-only, never mounted) into `/run/luma-duo2/radio/`.

The image only carries symlinks into those directories
(`/usr/lib/firmware/qca6490/*`, `/usr/lib/firmware/wlan/qca_cld/wlan_mac.bin`)
and our own `WCNSS_qcom_cfg.ini`.

NetworkManager randomizes the MAC while scanning and per network
(`stable-ssid`); `ethtool -P wlan0` shows the unit's own address.

## Files

| Path | What |
|---|---|
| `scripts/duo2-build-radio-modules.sh` | builds and signs `wlan.ko` and `hci_uart.ko` into the staged module tree (run after `duo2-stage-modules.sh`) |
| `kernel/radio/hci_qca-ldisc.patch` | `hci_qca` without serdev |
| `overlay/usr/libexec/luma-duo2-radio-setup` | `firmware`: partitions and addresses; `wlan`: cnss2 sequence and `modprobe wlan` |
| `overlay/usr/libexec/luma-duo2-bluetooth` | Bluetooth bring-up daemon, follows rfkill `bt_power` |
| `overlay/usr/lib/systemd/system/luma-duo2-{radio-firmware,wlan,bluetooth}.service` | the three services |
| `overlay/usr/lib/udev/rules.d/60-luma-duo2-radio-firmware.rules` | fail missing radio firmware at once |
| `overlay/usr/lib/systemd/network/10-luma-duo2-wlan.link` | keep `wlan0`/`p2p0` |
| `overlay/usr/lib/modprobe.d/luma-duo2-radio.conf` | `blacklist wlan` |
| `overlay/usr/lib/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini` | the qcacld configuration (ours, not the stock one) |

The firmware symlinks are made by the image compose, not kept here: the
repository ignores `*.bin` and `*.elf` so that firmware can never be
committed by accident.

(`overlay/` is `config/mobile/duo2-physical/overlay/`.)

## Checking it

```
systemctl status luma-duo2-radio-firmware luma-duo2-wlan luma-duo2-bluetooth
nmcli dev wifi list
bluetoothctl power on; bluetoothctl --timeout 10 scan on
journalctl -u luma-duo2-bluetooth -b
```

The first Bluetooth attempt after a warm restart of the service can fail
the version request, because the chip is still at 3 Mbaud. The daemon
power-cycles it through rfkill and retries, up to three times.
