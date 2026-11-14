#!/usr/bin/env bash
# Luma's published ARM nightly, made to boot on the Surface Duo 2.
#
# Luma Mobile is no longer a separate phone rootfs: every ARM phone runs the
# one generic ARM nightly (an OSTree commit, GNOME Shell with the Mobile
# presentation, GDM), written to userdata as a GPT holding LumaBoot (the
# device kit) and LumaRoot (ext4: /ostree, /boot), and adds its own hardware
# on top. The Fairphone 6 does that with a signed device kit that
# luma-device-layer layers with rpm-ostree on the first boot; a kit has to be
# signed with Luma's release key, so the Duo 2 cannot use that path.
#
# Instead this script derives a new commit on the workstation: the nightly's
# commit plus a Duo 2 layer (Microsoft's 5.4 kernel modules, the Adreno 660
# firmware, the native touch daemon duo2-touch, Turnip for KGSL, the bring-up
# services, and the handheld profile the FP6's luma-fp6-platform carries),
# labelled with the base commit's own SELinux policy. It deploys that commit
# as the default beside the nightly's own deployment and writes the whole
# disk back out as an Android sparse image for userdata.
#
# Usage: duo2-compose-ostree.sh NIGHTLY.sparse
# Output: build/duo2-ostree/userdata.sparse
#
# Nothing from the phone, the stock firmware or this workstation enters git.

set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/duo2-env.sh"

SPARSE="$(realpath -e "${1:?usage: duo2-compose-ostree.sh NIGHTLY.sparse}")"
BUILD="$LUMA_DUO2_ROOT/../build"
OUT="${LUMA_DUO2_OSTREE_OUT:-$BUILD/duo2-ostree}"
LUMA="${LUMA_SOURCE:-$LUMA_DUO2_ROOT/../luma}"
IMAGE="registry.fedoraproject.org/fedora:44"
OVERLAY="$LUMA_DUO2_ROOT/config/mobile/duo2-physical/overlay"

MODULES="$BUILD/duo2-modules"
# The stock heat processor, read by duo2-touch for its tuning only.
TOUCHLIB="$BUILD/stock-vendor/lib64/c1/libsurfacetouch_c1.so"
TURNIP="$BUILD/mesa-kgsl/opt/mesa-kgsl"
GPUFW="$BUILD/stock-vendor/firmware"
TOOLS="$BUILD/tools"
SSHPUB="$BUILD/ssh/duo2_ed25519.pub"

[ -d "$MODULES/lib/modules" ] || die "no staged modules -- run duo2-stage-modules.sh"
[ -f "$TOUCHLIB" ] || die "no $TOUCHLIB -- extract the stock vendor image (docs/touch.md)"
[ -x "$TOOLS/duo2-touch" ] || die "no $TOOLS/duo2-touch -- run duo2-build-touch.sh"
[ -d "$TURNIP" ] || die "no Turnip-KGSL in $TURNIP -- run duo2-build-turnip.sh"
ls "$GPUFW"/a660_sqe.fw >/dev/null 2>&1 || die "no Adreno 660 firmware in $GPUFW"
[ -x "$TOOLS/duo2-kmstest" ] || die "no $TOOLS/duo2-kmstest"
[ -f "$SSHPUB" ] || die "no $SSHPUB"
for f in config/mobile/luma-shell-handheld.dconf config/mobile/luma-greeter-handheld.dconf \
         config/mobile/environment.d/90-luma-handheld.conf; do
    [ -f "$LUMA/$f" ] || die "no $LUMA/$f -- is $LUMA a current Luma checkout?"
done

# The luma account's passcode, from the gitignored .duo2.local; only its hash
# enters the image.
duo2_load_config
passcode="$(sed -n 's/^DUO2_LUMA_PASSCODE=//p' "$DUO2_LOCAL_CONFIG" 2>/dev/null | tail -1)"
[ -n "$passcode" ] || die "no DUO2_LUMA_PASSCODE= in $DUO2_LOCAL_CONFIG"
LUMA_PASSWORD_HASH="$(printf '%s' "$passcode" | openssl passwd -6 -stdin)"
unset passcode
export LUMA_PASSWORD_HASH

mkdir -p "$OUT"
RAW="$OUT/disk.raw"
info "expanding $(basename "$SPARSE")"
rm -f "$RAW"
simg2img "$SPARSE" "$RAW"

# The layer is assembled on the workstation, then committed inside a
# privileged Fedora container that owns the loop device.
LAYER="$OUT/layer"
rm -rf "$LAYER"
install -d "$LAYER"

L="$LAYER/usr/lib/luma-duo2"
install -d "$L/bin" "$L/ssh"
# Microsoft's kernel modules.
install -d "$LAYER/usr/lib/modules"
cp -a --no-preserve=ownership "$MODULES/lib/modules/." "$LAYER/usr/lib/modules/"
# Adreno 660 microcode, GMU firmware and zap shader, loaded by KGSL from the
# firmware root (docs/gpu.md).
install -d "$LAYER/usr/lib/firmware"
install -m 0644 "$GPUFW"/a660_sqe.fw "$GPUFW"/a660_gmu.bin "$GPUFW"/a660_zap.* "$LAYER/usr/lib/firmware/"
# Turnip for KGSL. /opt is not part of an OSTree commit (it is /var/opt), so
# it moves under /usr/lib/luma-duo2, and its ICD manifest with it.
cp -a --no-preserve=ownership "$TURNIP" "$L/mesa-kgsl"
for icd in "$L"/mesa-kgsl/share/vulkan/icd.d/*.json; do
    sed -i 's#/opt/mesa-kgsl/#/usr/lib/luma-duo2/mesa-kgsl/#g' "$icd"
done
# duo2-touch, and the stock library whose PS_CFG_DATA_Zeta tuning it reads
# (docs/touch.md). The library is data here; nothing runs it.
install -m 0755 "$TOOLS/duo2-touch" "$L/bin/duo2-touch"
install -D -m 0644 "$TOUCHLIB" "$L/touch/lib64/libsurfacetouch_c1.so"
install -m 0755 "$TOOLS/duo2-kmstest" "$L/bin/duo2-kmstest"
install -m 0644 "$SSHPUB" "$L/ssh/root_authorized_keys"

# Luma packages newer than the nightly, built here with duo2-luma-builder.sh:
# Mutter with the handheld inexact scales (luma-fixes/0019; the panels' 2.5),
# and the Shell from Luma's current tree, whose search no longer swallows the
# on-screen keyboard's taps. Their files go over the nightly's in the layer;
# the rpmdb still names the nightly's versions.
UIRPMS="mutter/aarch64/RPMS/mutter-50.4-1.luma.20.duo2.3.fc44.aarch64.rpm
gnome-shell/RPMS/aarch64/gnome-shell-50.3-1.luma.176.fc44.aarch64.rpm"
for rpm in $UIRPMS; do
    [ -f "$LUMA/build/packages/$rpm" ] || die "no luma/build/packages/$rpm -- build it with duo2-luma-builder.sh"
    info "layering $(basename "$rpm")"
done
for rpm in $UIRPMS; do
    # rpm2cpio from the Fedora image (the workstation has no rpm); cpio here.
    docker run --rm -v "$LUMA/build/packages:/pkgs:ro" "$IMAGE" rpm2cpio "/pkgs/$rpm" |
        (cd "$LAYER" && cpio -idm --quiet --no-absolute-filenames './usr/*')
done

# Duo 2 overlay pieces that still apply under GNOME Shell.
install -D -m 0644 "$OVERLAY/usr/lib/udev/rules.d/70-luma-duo2-gpu.rules" \
    "$LAYER/usr/lib/udev/rules.d/70-luma-duo2-gpu.rules"
install -D -m 0644 "$OVERLAY/usr/lib/udev/rules.d/90-luma-duo2-touch.rules" \
    "$LAYER/usr/lib/udev/rules.d/90-luma-duo2-touch.rules"
# Which panel each backlight drives, for Luma's Mutter (mutter patch 0032).
install -D -m 0644 "$OVERLAY/usr/lib/udev/rules.d/71-luma-duo2-backlight.rules" \
    "$LAYER/usr/lib/udev/rules.d/71-luma-duo2-backlight.rules"
# The unit's own two-panel alignment, copied from its sfpd partition at each
# start (the file is per unit and never part of the image).
install -D -m 0755 "$OVERLAY/usr/libexec/luma-duo2-touch-calibration" \
    "$LAYER/usr/libexec/luma-duo2-touch-calibration"

# Wi-Fi and Bluetooth (QCA6490; docs/wifi-bt.md). wlan.ko and hci_uart.ko
# come from duo2-build-radio-modules.sh, which adds them to $MODULES after
# duo2-stage-modules.sh; the module copy above carries them into the layer.
ls "$MODULES"/lib/modules/*/extra/wlan.ko "$MODULES"/lib/modules/*/extra/hci_uart.ko >/dev/null 2>&1 ||
    die "no radio modules in $MODULES -- run duo2-build-radio-modules.sh after duo2-stage-modules.sh"
for f in usr/lib/systemd/system/luma-duo2-radio-firmware.service \
         usr/lib/systemd/system/luma-duo2-wlan.service \
         usr/lib/systemd/system/luma-duo2-bluetooth.service \
         usr/lib/systemd/network/10-luma-duo2-wlan.link \
         usr/lib/modprobe.d/luma-duo2-radio.conf \
         usr/lib/udev/rules.d/60-luma-duo2-radio-firmware.rules \
         usr/lib/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini; do
    install -D -m 0644 "$OVERLAY/$f" "$LAYER/$f"
done
install -D -m 0755 "$OVERLAY/usr/libexec/luma-duo2-radio-setup" "$LAYER/usr/libexec/luma-duo2-radio-setup"
install -D -m 0755 "$OVERLAY/usr/libexec/luma-duo2-bluetooth" "$LAYER/usr/libexec/luma-duo2-bluetooth"
# The firmware itself stays on the phone: cnss2 and qcacld find it through
# links into the partitions and addresses luma-duo2-radio-setup provides at
# boot under /run/luma-duo2.
install -d "$LAYER/usr/lib/firmware/qca6490"
for n in amss.bin amss20.bin m3.bin bdwlan.elf regdb.bin Data20.msc; do
    ln -sfn "/run/luma-duo2/firmware/modem/image/$n" "$LAYER/usr/lib/firmware/qca6490/$n"
done
ln -sfn /run/luma-duo2/radio/wlan_mac.bin "$LAYER/usr/lib/firmware/wlan/qca_cld/wlan_mac.bin"
install -d "$LAYER/usr/lib/systemd/system/multi-user.target.wants"
for unit in luma-duo2-radio-firmware luma-duo2-wlan luma-duo2-bluetooth; do
    ln -sf "../$unit.service" "$LAYER/usr/lib/systemd/system/multi-user.target.wants/$unit.service"
done

# Cellular modem and GNSS (docs/modem.md). libqrtr, tqftpserv and pd-mapper
# come from duo2-build-modem.sh, which also adds duo2_ipa_dtfix and
# duo2_ipa_netparent to $MODULES after duo2-stage-modules.sh. The layer is
# committed over the base tree, so the patched libqrtr replaces Fedora's.
MODEM="$BUILD/duo2-modem"
[ -x "$MODEM/usr/bin/pd-mapper" ] || die "no $MODEM -- run duo2-build-modem.sh"
ls "$MODULES"/lib/modules/*/extra/luma-duo2-modem/duo2_ipa_netparent.ko >/dev/null 2>&1 ||
    die "no modem modules in $MODULES -- run duo2-build-modem.sh after duo2-stage-modules.sh"
ls "$GPUFW"/ipa_fws.mdt >/dev/null 2>&1 || die "no IPA firmware in $GPUFW"
cp -a --no-preserve=ownership "$MODEM/usr/." "$LAYER/usr/"
install -m 0644 "$GPUFW"/ipa_fws.* "$LAYER/usr/lib/firmware/"
# MPSS loads through the subsystem/PIL framework from the firmware root; the
# images stay on the phone's modem partition, mounted at boot.
for n in modem.mdt $(seq -f 'modem.b%02g' 0 31) modem_pr; do
    ln -sfn "/run/luma-duo2/firmware/modem/image/$n" "$LAYER/usr/lib/firmware/$n"
done
for f in usr/lib/systemd/system/luma-duo2-modem-setup.service \
         usr/lib/systemd/system/luma-duo2-modem.service \
         usr/lib/systemd/system/pd-mapper.service \
         usr/lib/systemd/system/rmtfs.service.d/50-luma-duo2.conf \
         usr/lib/systemd/system/tqftpserv.service.d/50-luma-duo2.conf \
         usr/lib/udev/rules.d/80-luma-duo2-modem.rules; do
    install -D -m 0644 "$OVERLAY/$f" "$LAYER/$f"
done
install -D -m 0755 "$OVERLAY/usr/libexec/luma-duo2-modem" "$LAYER/usr/libexec/luma-duo2-modem"
ln -sf ../luma-duo2-modem.service "$LAYER/usr/lib/systemd/system/multi-user.target.wants/luma-duo2-modem.service"

U="$LAYER/usr/lib/systemd/system"
install -d "$U/multi-user.target.wants" "$U/gdm.service.d"
cat >"$U/luma-duo2-touch.service" <<'EOF'
[Unit]
Description=Luma Duo 2: the touchscreens (duo2-touch)
Documentation=https://github.com/none24smu/luma-duo2/blob/main/docs/touch.md
# The touchscreens exist before the display manager starts.
Before=gdm.service display-manager.service

[Service]
# Keep the digitizer powered, as the stock service does on start.
ExecStartPre=-/bin/sh -c 'echo on > /sys/bus/spi/devices/spi0.0/power/control'
# The unit's factory panel alignment, from sfpd (docs/touch.md).
ExecStartPre=-/usr/libexec/luma-duo2-touch-calibration /var/lib/luma-duo2/touch
ExecStart=/usr/lib/luma-duo2/bin/duo2-touch --align /var/lib/luma-duo2/touch/TwoPanelAlignment.bin
StateDirectory=luma-duo2/touch
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
# Release the display pipe the ABL splash keeps reserved before the first
# compositor starts: until a mode set frees it, SDE refuses a commit that
# lands on that pipe (the counterpart of the FP6's Plymouth DRM release).
cat >"$U/gdm.service.d/20-luma-duo2-display.conf" <<'EOF'
[Service]
ExecStartPre=-/usr/bin/timeout 10 /usr/lib/luma-duo2/bin/duo2-kmstest 1
EOF
# The marker duo2-home waits for: without it the phone goes back to fastboot
# by itself (duo2.booted= seconds into boot).
cat >"$U/luma-duo2-booted.service" <<'EOF'
[Unit]
Description=Luma Duo 2 bring-up: mark the boot as reaching multi-user
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/usr/bin/touch /run/luma-duo2-booted

[Install]
WantedBy=multi-user.target
EOF
# DHCP for the USB network link (the ECM half of g_cdc): the phone is
# 172.16.42.1 and hands the workstation 172.16.42.2, with no router or DNS on
# offer so the workstation never routes its own traffic this way.
cat >"$U/luma-duo2-usb-dhcp.service" <<'EOF'
[Unit]
Description=Luma Duo 2 bring-up: DHCP for the USB network link
After=NetworkManager.service sys-subsystem-net-devices-usb0.device
BindsTo=sys-subsystem-net-devices-usb0.device

[Service]
ExecStartPre=/bin/sh -c "until ip -4 addr show usb0 | grep -q 172.16.42.1; do sleep 1; done"
ExecStart=/usr/bin/dnsmasq --keep-in-foreground --port=0 --interface=usb0 --bind-interfaces --dhcp-range=172.16.42.2,172.16.42.2,255.255.255.0,1h --dhcp-option=3 --dhcp-option=6 --dhcp-authoritative --log-dhcp --dhcp-leasefile=/run/luma-duo2-usb.leases
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF
for unit in luma-duo2-touch luma-duo2-booted luma-duo2-usb-dhcp sshd; do
    ln -sf "../$unit.service" "$U/multi-user.target.wants/$unit.service"
done
# Bring-up root shell on the USB serial console.
install -d "$U/serial-getty@ttyGS0.service.d" "$U/getty.target.wants"
cat >"$U/serial-getty@ttyGS0.service.d/10-duo2-bringup.conf" <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --keep-baud 115200 %I $TERM
EOF
ln -sf ../serial-getty@.service "$U/getty.target.wants/serial-getty@ttyGS0.service"

# "bootloader" sends the phone to fastboot from the booted system, the same
# command the bring-up init and the initramfs answer to.
install -d "$LAYER/usr/bin"
printf '%s\n' '#!/bin/sh' '# Luma Duo 2 bring-up: reboot into fastboot.' \
    'exec systemctl reboot --reboot-argument=bootloader' >"$LAYER/usr/bin/bootloader"
chmod 0755 "$LAYER/usr/bin/bootloader"

# USB network, read-only from /usr.
install -d -m 0755 "$LAYER/usr/lib/NetworkManager/system-connections"
printf '%s\n' '[connection]' 'id=duo2-usb' 'type=ethernet' 'interface-name=usb0' \
    'autoconnect=true' '' '[ipv4]' 'method=manual' 'address1=172.16.42.1/24' '' \
    '[ipv6]' 'method=disabled' \
    >"$LAYER/usr/lib/NetworkManager/system-connections/duo2-usb.nmconnection"
chmod 0600 "$LAYER/usr/lib/NetworkManager/system-connections/duo2-usb.nmconnection"

# /etc defaults (OSTree's /usr/etc; merged into /etc on deploy).
E="$LAYER/usr/etc"
install -d "$E/ssh/sshd_config.d" "$E/systemd/system" "$E/dconf/db/luma.d" "$E/dconf/db/gdm.d"
# SSH over the USB link: root by the bring-up key only. Read before Fedora's
# 50-redhat.conf, so these values win.
printf '%s\n' '# Luma Duo 2 bring-up: root by key over the USB link.' \
    'PermitRootLogin prohibit-password' 'PasswordAuthentication no' \
    'Match User root' '    AuthorizedKeysFile /usr/lib/luma-duo2/ssh/root_authorized_keys' \
    >"$E/ssh/sshd_config.d/01-luma-duo2.conf"
# The Microsoft kernel registers an hvc0 console with nothing behind it.
ln -sf /dev/null "$E/systemd/system/serial-getty@hvc0.service"
# No device kit: the Duo 2's layer is in the commit itself.
ln -sf /dev/null "$E/systemd/system/luma-device-layer.service"
# The handheld profile, as luma-fp6-platform carries it for the FP6: the
# device class that selects GNOME Shell's Mobile presentation, its session
# environment, and the handheld dconf defaults for the session and GDM.
echo handheld >"$E/luma-device-class"
install -D -m 0644 "$LUMA/config/mobile/environment.d/90-luma-handheld.conf" \
    "$LAYER/usr/lib/environment.d/90-luma-handheld.conf"
install -m 0644 "$LUMA/config/mobile/luma-shell-handheld.dconf" "$E/dconf/db/luma.d/10-luma-handheld"
install -m 0644 "$LUMA/config/mobile/luma-greeter-handheld.dconf" "$E/dconf/db/gdm.d/10-luma-handheld"
# Each touchscreen on its own panel. Both panels carry the same EDID, so
# Mutter cannot match a touchscreen to its panel by itself: the output is set
# per touchscreen (by product id: 0C1D left, 0C1E right), and it has to be the
# user's own value, since Mutter reads a dconf default as "automatic". The
# fourth field, the connector, tells the twin panels apart.
install -D -m 0755 /dev/stdin "$LAYER/usr/libexec/luma-duo2-touch-outputs" <<'EOF'
#!/bin/sh
# Luma Duo 2: tie each touchscreen to its own panel (duo2-compose-ostree.sh).
S=org.gnome.desktop.peripherals.touchscreen
P=/org/gnome/desktop/peripherals/touchscreens
gsettings set "$S:$P/045e:0c1d/" output "['QCM', '0x0001', '0x00000001', 'DSI-1']"
gsettings set "$S:$P/045e:0c1e/" output "['QCM', '0x0001', '0x00000001', 'DSI-2']"
EOF
install -D -m 0644 /dev/stdin "$E/xdg/autostart/luma-duo2-touch-outputs.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Luma Duo 2 touchscreen outputs
Exec=/usr/libexec/luma-duo2-touch-outputs
NoDisplay=true
X-GNOME-Autostart-Phase=Initialization
EOF
# Both panels at Android's density for them (400 dpi: scale 2.5), side by
# side, the left one primary. 2.5 does not divide 1344x1892 exactly; Luma's
# Mutter offers such scales on handhelds with the logical size rounded
# (538x757) and never picks one by itself, so this system default does. The
# panels run their 60 Hz mode (stock's default; Luma's Mutter takes the
# rate from the mode's vrefresh, as the SDE link clock works out at 43.57).
# A user's own monitors.xml still wins.
install -D -m 0644 /dev/stdin "$E/xdg/monitors.xml" <<'EOF'
<monitors version="2">
  <configuration>
    <layoutmode>logical</layoutmode>
    <logicalmonitor>
      <x>0</x>
      <y>0</y>
      <scale>2.5</scale>
      <primary>yes</primary>
      <monitor>
        <monitorspec>
          <connector>DSI-1</connector>
          <vendor>QCM</vendor>
          <product>0x0001</product>
          <serial>0x00000001</serial>
        </monitorspec>
        <mode>
          <width>1344</width>
          <height>1892</height>
          <rate>60.000</rate>
        </mode>
      </monitor>
    </logicalmonitor>
    <logicalmonitor>
      <x>538</x>
      <y>0</y>
      <scale>2.5</scale>
      <monitor>
        <monitorspec>
          <connector>DSI-2</connector>
          <vendor>QCM</vendor>
          <product>0x0001</product>
          <serial>0x00000001</serial>
        </monitorspec>
        <mode>
          <width>1344</width>
          <height>1892</height>
          <rate>60.000</rate>
        </mode>
      </monitor>
    </logicalmonitor>
  </configuration>
</monitors>
EOF
# Both panels for the Shell's display geometry (schema 2: several built-in
# displays by connector). No camera openings on either panel.
install -d "$LAYER/usr/share/luma"
printf '%s\n' '{"schema": 2, "displays": [' \
    '  {"connector": "DSI-1", "nativeWidth": 1344, "nativeHeight": 1892, "cutouts": []},' \
    '  {"connector": "DSI-2", "nativeWidth": 1344, "nativeHeight": 1892, "cutouts": []}' \
    ']}' >"$LAYER/usr/share/luma/display-geometry.json"

# OSTree on Microsoft's 5.4 kernel: no composefs (it needs data-only overlayfs
# lower layers, Linux 6.5). The initramfs carries the same file
# (duo2-mkinitramfs-ostree.sh); this copy keeps the deployment consistent.
install -D -m 0644 /dev/stdin "$LAYER/usr/lib/ostree/prepare-root.conf" <<'EOF'
# Luma Duo 2: Microsoft's 5.4 kernel cannot mount composefs (data-only
# overlayfs lower layers arrived in Linux 6.5).
[composefs]
enabled = no
[sysroot]
readonly = true
EOF

export VERSION_SUFFIX="duo2.$(date -u +%Y%m%d%H%M)"
info "committing the Duo 2 layer onto the nightly ($VERSION_SUFFIX)"
docker run --rm --privileged -v /dev:/dev -e LUMA_PASSWORD_HASH -e VERSION_SUFFIX \
    -v "$OUT:/out" "$IMAGE" bash -euo pipefail -c '
dnf -q install -y ostree e2fsprogs util-linux dconf android-tools >/dev/null 2>&1
L=$(losetup -f --show -b 4096 -P /out/disk.raw)
cleanup() { umount /mnt 2>/dev/null || true; losetup -d "$L" 2>/dev/null || true; }
trap cleanup EXIT
mount "${L}p2" /mnt
repo=/mnt/ostree/repo
base=$(ostree --repo=$repo rev-parse "$(ostree --repo=$repo refs | grep -m1 -E "luma/1/aarch64/nightly$")")
version=$(ostree --repo=$repo show --print-metadata-key=version "$base" | tr -d "\047")
echo "base $base ($version)"
basedeploy=/mnt/ostree/deploy/luma/deploy/$base.0

# The handheld dconf databases, compiled from the base keyfiles plus ours.
for db in luma gdm; do
    d=$(mktemp -d)
    cp -a "$basedeploy/usr/etc/dconf/db/$db.d/." "$d/" 2>/dev/null || true
    cp -a "/out/layer/usr/etc/dconf/db/$db.d/." "$d/"
    dconf compile "/out/layer/usr/etc/dconf/db/$db" "$d"
    rm -rf "$d"
done

new=$(ostree --repo=$repo commit --branch=luma-duo2/aarch64/nightly \
    --tree=ref="$base" --tree=dir=/out/layer --owner-uid=0 --owner-gid=0 \
    --selinux-policy-from-base --add-metadata-string=version="$version+$VERSION_SUFFIX" \
    --subject="Luma $version for the Surface Duo 2")
echo "commit $new"

# Deploy as the default, keeping the nightly deployment as the fallback.
ostree admin deploy --sysroot=/mnt --os=luma --retain "$new"
newdeploy=/mnt/ostree/deploy/luma/deploy/$new.0
echo "deployed $newdeploy"
rel=${newdeploy#/mnt/ostree/}
ln -sfn "$rel" /mnt/ostree/root.a
ln -sfn "$rel" /mnt/ostree/root.b

# Per-install /etc changes in the new deployment.
rm -f "$newdeploy/etc/luma/device-kit.conf"
sed -i "s/^SELINUX=.*/SELINUX=permissive/" "$newdeploy/etc/selinux/config"
usermod --root "$newdeploy" --password "$LUMA_PASSWORD_HASH" luma
# Bring-up: sign straight in to the Mobile shell, as the earlier greetd image
# did. The Shell still locks on idle with the same passcode.
sed -i "s/^\[daemon\]$/[daemon]\nAutomaticLoginEnable=True\nAutomaticLogin=luma/" "$newdeploy/etc/gdm/custom.conf"
# A first boot has no network or RTC: start the clock from now.
install -d /mnt/ostree/deploy/luma/var/lib/systemd/timesync
touch /mnt/ostree/deploy/luma/var/lib/systemd/timesync/clock

echo "$new" > /out/commit; echo "$version+$VERSION_SUFFIX" > /out/version
sync
'

info "writing userdata.sparse"
rm -f "$OUT/userdata.sparse"
img2simg "$RAW" "$OUT/userdata.sparse"
rm -f "$RAW"
info "built $(cat "$OUT/version"): $OUT/userdata.sparse ($(stat -c%s "$OUT/userdata.sparse") bytes)"
