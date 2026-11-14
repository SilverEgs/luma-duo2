# Cellular modem and GNSS on the Surface Duo 2

The Duo 2's modem is the SM8350's own MPSS (firmware
`MPSS.HI.4.0.c8-00035`), started by Microsoft's 5.4 kernel through Qualcomm's
subsystem/PIL framework (`/sys/bus/msm_subsys`, `/dev/subsys_modem`), not
remoteproc. The modem's location engine (QMI LOC) is the phone's GNSS.

## Pieces

| Piece | Source | Role |
|---|---|---|
| `modem.mdt`, `modem.bNN`, `modem_pr/` | the phone's own `modem_<slot>` partition, mounted read-only at `/run/luma-duo2/firmware/modem` | MPSS image (PIL loads `/usr/lib/firmware/modem.*`, links into the mount) and carrier configuration (MBNs, served over TFTP) |
| `ipa_fws.*` | stock vendor image, `/vendor/firmware` | IPA microcontroller firmware |
| `luma-duo2-modem-setup.service` | Duo 2 overlay | mounts the partition, sets the modem's restart level to RELATED, brings up the IPA |
| `luma-duo2-modem.service` | Duo 2 overlay | holds `/dev/subsys_modem` open (the modem runs while it is open), then `luma-duo2-modem ready` |
| `rmtfs` | Fedora, `rmtfs.service.d/50-luma-duo2.conf` | serves the modem's EFS from `modemst1`/`modemst2`/`fsg`/`fsc`, read-only (`-r`) |
| `tqftpserv` | upstream 1.3 + `packaging/tqftpserv/`, `duo2-build-modem.sh` | serves `modem_pr` (MBNs) to the modem |
| `pd-mapper` | upstream + `packaging/pd-mapper/`, `duo2-build-modem.sh` | service-registry locator for the modem (and ADSP/CDSP/SLPI) domains |
| `libqrtr` | upstream 1.2 (Fedora's version) + `packaging/qrtr/`, `duo2-build-modem.sh` | replaces Fedora's library so servers stay published (below) |
| `duo2_ipa_dtfix.ko`, `duo2_ipa_netparent.ko` | `kernel/modem/`, `duo2-build-modem.sh` | stopgaps for two kernel/DT bugs (below) |
| `80-luma-duo2-modem.rules` | Duo 2 overlay | `/dev/qcom_rmtfs_uio1`; re-parents `rmnet_ipa0` after a modem restart |
| ModemManager, libqmi, geoclue | Fedora / Luma, unchanged | the modem (QRTR control port `qrtr0`, net port `rmnet_ipa0`) and location for apps |

Boot order: `luma-duo2-radio-firmware` (mounts the modem partition, shared
with Wi-Fi) → `luma-duo2-modem-setup` → `rmtfs`, `pd-mapper`, `tqftpserv` →
`luma-duo2-modem`. The MPSS is out of reset ~0.5 s after the open and its QMI
services are on QRTR node 0 about 2 s later; the unit is started in ~3 s.
ModemManager exports the modem ~40 s after that (its GNSS assistance queries
time out first). Restarting `luma-duo2-modem.service` restarts the modem.

## What was wrong, and why each piece exists

**No remoteproc.** Fedora's rmtfs, tqftpserv and pd-mapper find the modem and
its firmware through `/sys/class/remoteproc`, which this kernel registers but
leaves empty. rmtfs runs without `-s`; tqftpserv falls back to the firmware
search path; pd-mapper is given the partition's `image/` directory.

**rmtfs memory is a UIO device.** The kernel shares the 2.5 MiB EFS buffer
through `msm_sharedmem` as `/dev/uioN` named `rmtfs`, not `qcom_rmtfs_mem1`.
rmtfs supports UIO as `/dev/qcom_rmtfs_uio1`, which the udev rule provides.

**rmtfs is read-only.** The EFS partitions hold the unit's calibration and
IMEI. With `-r` the modem reads them but its writes stay in rmtfs's memory, so
nothing it does reaches the partitions; settings it stores in EFS (e.g. the
GNSS NMEA selection, network caches) are lost when rmtfs
restarts, at the latest at the next boot. `luma-duo2-modem ready` re-applies
what Luma needs at every modem start. Before ever dropping `-r`,
back up all four partitions (they were backed up during bring-up, outside
git).

**Servers vanished from QRTR once the modem was up.** libqrtr's
`qrtr_publish()` sends NEW_SERVER with the node field zero. Qualcomm's
downstream qrtr (`qrtr_sendmsg`) looks that node up and silently drops the
announcement when it is a remote one, and node 0 is the modem. rmtfs, started
before the modem, was visible; pd-mapper and tqftpserv started after it were
not, and the modem waited 60 s for them before bringing up its services.
`packaging/qrtr/0001-…` names the publishing socket itself.

**The IPA never probed.** `duo2-mkdtb-usb.sh` round-trips the stock DTB
through dtc, which printed `qcom,scaling-exceptions = "wdi", "100", "600",
"1200"` as `"wdi\0100\0600\01200"` and read it back with `\010` and `\012` as
octal escapes. The IPA driver rejects the result (`ipa3_plat_drv_probe ...
-14`), so there was no IPA, no `rmnet_ipa0` and no modem data path. It is the
only property in the tree affected. `duo2-mkdtb-usb.sh` now writes the escape
as `\000`; until a DTB built that way is flashed, `duo2_ipa_dtfix` repairs
the live property and `luma-duo2-modem setup` binds the driver again.

**ModemManager would not take the modem.** Its `qcom-soc` plugin needs a net
port whose parent driver is `ipa` and refuses a QMI modem without one.
`rmnet_ipa.c` never calls `SET_NETDEV_DEV()`, so `rmnet_ipa0` is a virtual
device. `duo2_ipa_netparent` moves it under `1e00000.qcom,ipa`; the one-line
kernel fix (`SET_NETDEV_DEV(dev, &ipa3_ctx->master_pdev->dev)` after
`alloc_netdev()` in `ipa3_wwan_probe()`) would replace it. It is applied only
once the modem's QMI services are up: ModemManager gives up on a net port
that arrives seconds before its control port.

**Restart level.** Every subsystem defaults to restart level SYSTEM, so a
modem crash would restart the phone. Setup sets RELATED (modem only).

## GNSS and location

The engine runs standalone (no assistance data) and reports through QMI LOC.
It emits only Qualcomm's proprietary `$PQ…` sentences until the standard ones
are selected (`qmicli --loc-set-nmea-types='gga|rmc|gsv|gsa|vtg'`, done by
`luma-duo2-modem ready`); geoclue's ModemManager source needs GGA and RMC.

Verified: a fix with GPS and Galileo satellites (10 tracked, C/N0 up to
30 dB-Hz) next to a window; ModemManager's `gps-nmea`/`gps-raw` location; and
geoclue delivering 1 Hz positions described as "GPS GGA+RMC" to a client.
Apps get positions only when the user turns on Location Services in Settings
(`org.gnome.system.location enabled`, off by default).

## SIM, registration, data

Slot 1 (the physical SIM tray) was empty during bring-up and slot 2, the
eUICC, has no enabled profile, so ModemManager reports the modem as
`failed: sim-missing`; registration and data could not be tested.

For data, still to do and untested: ModemManager's DPM step needs the IPA
endpoint numbers in sysfs (`<ipa>/modem/{rx,tx}_endpoint_id`, mainline IPA's
names), which the vendor driver does not have (they are what `rmnet_ipa`'s
`RMNET_IOCTL_GET_EP_PAIR` returns); the vendor IPA's WAN pipes are only set
up by `RMNET_IOCTL_SET_{EGRESS,INGRESS}_DATA_FORMAT` and
`RMNET_IOCTL_ADD_MUX_CHANNEL` (Android's netmgrd), which nothing on Luma
issues; and ModemManager's rmnet links need the kernel's `rmnet` driver
(`CONFIG_RMNET`, off in Microsoft's config; the in-tree driver builds as a
module).

## Calls and SMS

Luma's call UI (`luma-calls`) is a VoIP call-activity surface, not a
cellular dialer, and Luma ships no ModemManager-based dialer or SMS app, so
nothing consumes the modem's voice or messaging interfaces yet. With a SIM,
ModemManager's `Voice` and `Messaging` interfaces are what such an app would
use.

## Checks

- `mmcli -L` lists `[QUALCOMM INCORPORATED] 0`; `mmcli -m any` shows ports
  `qrtr0 (qmi), rmnet_ipa0 (net)`, plugin `qcom-soc`.
- `qmicli -d qrtr://0 --dms-get-revision`, `--uim-get-card-status`,
  `--uim-get-slot-status`.
- `qrtr-lookup`: 14 (rmtfs), 64 (pd-mapper) and 4096 (tqftpserv) on node 1,
  the modem's services on node 0.
- `mmcli -m any --location-enable-gps-nmea --location-enable-gps-raw`, then
  `--location-get` (outdoors or near a window for a fix).
