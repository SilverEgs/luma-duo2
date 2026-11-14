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
| `duo2_ipa_dtfix.ko`, `duo2_ipa_netparent.ko` | `kernel/modem/`, `duo2-build-modem.sh` | stopgaps for kernel/DT bugs (below); `duo2_ipa_netparent` also gives the IPA the endpoint attributes ModemManager reads |
| `luma-duo2-ipa-wan` | `tools/duo2-ipa-wan/`, `duo2-build-modem.sh` | connects the IPA's WAN data pipes, the part of Android's netmgrd Luma needs (mobile data, below) |
| `rmnet.ko` | the kernel's own `drivers/net/ethernet/qualcomm/rmnet`, `duo2-build-modem.sh` | QMAP mux links on `rmnet_ipa0`, ModemManager's data interfaces (`CONFIG_RMNET` is off in Microsoft's configuration) |
| `80-luma-duo2-modem.rules`, `luma-duo2-modem-net@.service` | Duo 2 overlay | `/dev/qcom_rmtfs_uio1`; data pipes and parent for each new `rmnet_ipa0` |
| ModemManager, libqmi, geoclue | Fedora / Luma, unchanged | the modem (QRTR control port `qrtr0`, net port `rmnet_ipa0`) and location for apps |

Boot order: `luma-duo2-radio-firmware` (mounts the modem partition, shared
with Wi-Fi) → `luma-duo2-modem-setup` → `rmtfs`, `pd-mapper`, `tqftpserv` →
`luma-duo2-modem`. The MPSS is out of reset ~0.5 s after the open and its QMI
services are on QRTR node 0 about 2 s later; `luma-duo2-modem ready` then
connects the IPA's data pipes and parents `rmnet_ipa0`, and the unit is
started in ~3 s.
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
that arrives seconds before its control port. `rmnet_ipa` re-creates the
netdev, parentless, at every modem start; when `luma-duo2-modem.service`
starts the modem, `ready` re-applies it, and when the modem restarts by
itself (restart level RELATED) the udev rule starts
`luma-duo2-modem-net@rmnet_ipa0.service`, which runs the same steps.

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

## SIM and registration

Slot 1 (the physical SIM tray) was empty during bring-up and slot 2, the
eUICC, has no enabled profile, so ModemManager reports the modem as
`failed: sim-missing`; registration could not be tested.

## Mobile data

Ready for a SIM, untested with one: everything up to the bearer works and
is checked at every modem start, but no data call could be made.

The path is the one stock Android uses (`netmgr_config.xml`, profile
`lahaina`), with Luma's pieces in netmgrd's place:

```
ModemManager --QMI over QRTR--> modem: DPM Open Port, WDA Set Data Format,
                                       WDS Bind Mux Data Port, Start Network
qmapmuxN.M (rmnet.ko, one per bearer, QMAP mux id)
  -> rmnet_ipa0 (vendor rmnet_ipa) -> IPA pipes APPS_WAN_PROD (2) / APPS_WAN_CONS (20) -> modem
```

What was missing, in the order it bites:

1. **No data pipes.** The vendor IPA connects `rmnet_ipa0` to the IPA only
   when user space sends `RMNET_IOCTL_EXTENDED` requests on it
   (`SET_EGRESS_DATA_FORMAT`, `SET_INGRESS_DATA_FORMAT`, which also installs
   the IPA's default WAN route and QMAP header, and `ADD_MUX_CHANNEL`), as
   netmgrd does. Until then the netdev drops everything.
   `luma-duo2-ipa-wan setup` sends them, once per `rmnet_ipa0` instance
   (`luma-duo2-modem` records the ifindex it configured in
   `/run/luma-duo2/ipa-wan.rmnet_ipa0`; a second ingress setup on the same
   instance fails because the pipe is taken). Both directions are plain
   QMAP: no checksum offload, no uplink aggregation and no `AGG_DATA`, so
   the IPA splits its downlink aggregation frames and hands over one QMAP
   packet at a time. Stock Android uses MAPv5 checksum offload, which needs
   Qualcomm's `rmnet_core`; the kernel's rmnet in 5.4 does MAPv1/v4 only.
   With `qcom,rmnet-ctl-enable` in the device tree the IPA also connects
   its low-latency pipes (endpoints 1 and 17) and announces them to the
   modem, as on Android, where Qualcomm's `rmnet_ctl` carries QMAP flow
   control over them. Nothing reads them on Luma; nothing here turns QMAP
   flow control on (netmgrd does, through QMI DFS), so the modem should not
   use them -- to confirm with a SIM.
2. **No DPM port, so WDA said InvalidArgument.** The modem knows no data
   port for the embedded endpoint until QMI DPM Open Port maps it to the IPA
   endpoint pair; every WDA request naming `embedded`/1 failed before that,
   and ModemManager's "Couldn't setup data format: InvalidArgument" was
   that. ModemManager opens the port itself, with the numbers it reads from
   `<ipa>/modem/{rx,tx}_endpoint_id`; mainline's IPA has them, the vendor
   one not, so `duo2_ipa_netparent` adds them (from `ipa_get_ep_mapping()`,
   APPS_WAN_CONS = 20 and APPS_WAN_PROD = 2, what `RMNET_IOCTL_GET_EP_PAIR`
   gives netmgrd). ModemManager then logs "Opening DPM port with TX ID: 2
   and RX ID: 20", reads the format (`kernel data modes: mux-rmnet`) and
   "rmnet link management supported: 254 multiplexed bearers allowed".
   The DPM mapping lasts until the modem restarts; ModemManager re-probes
   and opens it again each time.
3. **No rmnet driver.** ModemManager (libqmi's rmnet backend) creates one
   rtnetlink `rmnet` link per bearer on `rmnet_ipa0`. The kernel's own
   driver builds as a module against Microsoft's tree and loads
   (`luma-duo2-modem` modprobes it; it has no rtnl alias to autoload).

At connect time ModemManager asks WDA for QMAPv5 first, then QMAP; the
modem accepts both (`qmicli --wda-set-data-format`, endpoint
`embedded`/1: downlink up to 32 datagrams, uplink 1 datagram, i.e. no
uplink aggregation, matching the egress pipe). The QMAP headers on the
AP's side of the IPA come from the AP's endpoint configuration above, not
from that negotiation, and ModemManager creates the links without
checksum flags because the IPA has no `feature/{rx,tx}_offload`
attributes.

Verified without a SIM: the ioctls succeed and the IPA's log shows the
WAN (2, 20) and low-latency (1, 17) pipes connected; ModemManager opens DPM, reads the data format and reports no data
errors (`ports: qrtr0 (qmi), rmnet_ipa0 (net)`); `qmicli --link-add` makes
`qmapmux0.0@rmnet_ipa0` (mux id 1) as ModemManager would; QMAP frames
sent on such a link leave through `rmnet_ipa0` (TX counted, not dropped;
the only drops are `rmnet_ipa0`'s own non-QMAP IPv6 autoconfiguration
packets, which `rmnet_ipa` filters). All of it again after
`systemctl restart luma-duo2-modem`, and through
`luma-duo2-modem-net@.service` when the modem comes up without that unit.
NetworkManager (with `NetworkManager-wwan`) lists `qrtr0` as a `gsm`
device, `unavailable` without a SIM.

Still to see with a SIM: registration, `mmcli -m any --simple-connect`,
downlink packets on the `qmapmux` link, and whether QMAPv5 or QMAP gets
negotiated in practice.

A kernel rebuild would replace the stopgaps: `SET_NETDEV_DEV()` in
`ipa3_wwan_probe()`, the two `modem/` attributes on the IPA device, and
`CONFIG_RMNET=m`. The pipe setup stays a user-space step, as on Android.

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
- Data path: `cat /sys/class/net/rmnet_ipa0/device/modem/*` gives 20 and 2;
  `/run/luma-duo2/ipa-wan.rmnet_ipa0` equals
  `/sys/class/net/rmnet_ipa0/ifindex`; `lsmod` lists `rmnet`;
  `qmicli -d qrtr://0 --wda-get-data-format="ep-type=embedded,ep-iface-number=1"`
  succeeds; ModemManager's journal has no "Couldn't setup data format".
