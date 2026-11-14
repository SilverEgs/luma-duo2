# Luma's ARM nightly on the Surface Duo 2

Luma Mobile is no longer a separate phone root filesystem. Every ARM phone
runs the one generic **Luma ARM nightly** -- an OSTree commit with GNOME Shell
and its Mobile presentation (Home widgets, dock, search, notification island),
signed in through GDM -- and adds its own hardware on top. This is how the Duo
2 runs it.

## Layout on the phone

Luma's phone userdata image (`scripts/mobile/build-phone-userdata.sh` in
Luma) is a GPT with 4096-byte sectors written to `userdata`:

| Partition | Filesystem | Holds |
|---|---|---|
| `LumaBoot` | FAT32, volume ID `032D-57AD` | a device kit (unused on the Duo 2) |
| `LumaRoot` | ext4, UUID `03797c05-cf87-4eda-adc4-6ce111baf2d3` | `/ostree`, `/boot` |

The initramfs grows `LumaRoot` to fill `userdata` on the first boot.

## How the Duo 2 differs from the Fairphone 6

The FP6 adds its hardware with a **device kit**: signed RPMs that
`luma-device-layer` layers with rpm-ostree on the first boot. A kit must be
signed with Luma's release key, so the Duo 2 cannot use that path. Instead
`scripts/duo2-compose-ostree.sh` derives a commit on the workstation:

1. the nightly's commit, unchanged, plus a **Duo 2 layer**: Microsoft's 5.4
   kernel modules, the Adreno 660 firmware, the native touch daemon duo2-touch,
   Turnip for KGSL (under `/usr/lib/luma-duo2`, since `/opt` is not part of a
   commit), the bring-up services (USB network, SSH by key, serial console,
   the way home), and the handheld profile the FP6's `luma-fp6-platform`
   carries (`/etc/luma-device-class` = `handheld`, the handheld dconf for the
   session and GDM);
2. labelled with the nightly's own SELinux policy, deployed as the default,
   with the nightly's own deployment kept as the fallback;
3. written back out as an Android sparse image for `userdata`.

## Boot

`scripts/duo2-mkinitramfs-ostree.sh` appends Luma's OSTree archive to the Duo
2 initramfs, as Luma's FP6 boot image does: Luma's `init_functions.sh`, the
shared boot resolver that points `/ostree/root.b` at ostree's default
deployment on every boot, and Luma's `ostree-prepare-root`.

Two things are specific to Microsoft's 5.4 kernel:

- **Composefs is off.** The nightly asks for it (`prepare-root.conf`:
  `enabled = yes`); it needs data-only overlayfs lower layers, from Linux 6.5.
  The initramfs carries its own `prepare-root.conf` with composefs disabled.
- **Luma's patched ostree.** Luma's `init_functions.sh` creates
  `/run/ostree/initramfs-mount-var` before `ostree-prepare-root` runs. Luma
  builds ostree with `patches/ostree/0001-luma-prepare-root-marker.patch` to
  allow that; Fedora's ostree in the deployment refuses ("Failed to create
  /run/ostree: File exists"). The initramfs takes Luma's binary and its
  libraries from Luma's published FP6 boot image, pinned by SHA-256.

`duo2-mkboot.sh --luma-ostree` adds the root and boot UUIDs,
`ostree=/ostree/root.b` (the ABL passes `androidboot.slot_suffix=_b`) and a
longer first-boot allowance for the way home (`duo2.booted=900`).

## Build and flash

```sh
./scripts/duo2-compose-ostree.sh luma-nightly-<build>-aarch64-disk.sparse
./scripts/duo2-mkinitramfs-ostree.sh
./scripts/duo2-mkboot.sh --luma-ostree
. scripts/duo2-env.sh && duo2_fastboot flash userdata ../build/duo2-ostree/userdata.sparse
./scripts/duo2-flash.sh --stock-dtbo
```

The nightly comes from `https://media.simplyluma.com/media/aarch64/nightly/latest.json`
(`disk_image`), checked against its SHA-256 there.

## Sign-in

GDM signs `luma` in automatically (bring-up); the Shell locks on idle with the
passcode from `.duo2.local`.

## Screenshots

GNOME Shell serves its screenshot interface only to a few system callers, and
neither grim nor a KMS grab can read Mutter's buffers here. `tools/duo2-shot`
asks the desktop portal instead, after granting host programs the screenshot
permission once:

```sh
flatpak permission-set screenshot screenshot "" yes   # as luma
python3 duo2-shot.py /tmp/home.png
```

## Known issues

- Both panels are separate monitors (kernel patch 0010): DSI-1 on the
  left at 0,0 and DSI-2 on the right at 672,0, both at scale 2. They share
  one EDID (vendor `QCM`, serial `0x00000001`), so Mutter cannot tell them
  apart in `monitors.xml`.
- Failed units on the first boot, none of them blocking: `luma-grow-root`
  (the initramfs already grew the root), `systemd-sysctl` (keys the 5.4
  kernel lacks), `luma-greenboot-fallback`, `luma-app-baseline`, `firewalld`,
  `var-lib-nfs-rpc_pipefs.mount`. Since fixed in the composed image, except
  `luma-greenboot-fallback`: grow-root and rpc_pipefs run only where they
  apply, sysctl gets a swappiness 5.4 accepts, the app baseline installs with
  the rebuilt flatpak (docs/flatpak.md), and firewalld runs on nf_tables
  modules (docs/firewall.md).
- The GPU is not used by GNOME Shell: Mutter renders with llvmpipe (Zink
  cannot find a KGSL device).
