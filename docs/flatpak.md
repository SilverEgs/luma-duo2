# Flatpak on the Duo 2's 5.4 kernel

Luma installs its apps as Flatpaks: at first boot `luma-app-baseline.service`
installs the signed offline baseline shipped in the image
(`/usr/share/luma/app-baseline`), and the Shell asks to repair the
installation while that has not happened.

On Microsoft's msm-5.4 kernel every Flatpak install failed:

    Error: Failed to install org.projectluma.Platform.GL.default:
    Failed to open files: statx didn't return all required fields: No data available

Flatpak 1.18 resolves paths below its repositories with libglnx's
`glnx_chaseat()`, which compares mount ids to stay inside the tree it
resolves and asks `statx()` for them (`STATX_MNT_ID`). That field came with
Linux 5.8; on 5.4 `statx()` succeeds without it, and the chase gives up. The
same id is in the file descriptor's `/proc/self/fdinfo` entry (`mnt_id:`,
there since 3.15), which is where systemd falls back to as well.
`packaging/flatpak/0001-libglnx-chase-mount-id-from-fdinfo-before-linux-5.8.patch`
reads it there when `statx()` leaves it out; on newer kernels nothing changes.

ostree, rpm-ostree and bootc do not use the chase and are unaffected.

## Building

`scripts/duo2-build-flatpak.sh` rebuilds Fedora 44's exact source package
(`flatpak-1.18.4-1.fc44.src.rpm`, checked by SHA-256) with the patch, as
release `1.duo2.1`, in the Luma builder container, into
`luma/build/packages/flatpak-duo2/RPMS`. `duo2-compose-ostree.sh` layers
`flatpak`, `flatpak-libs` and `flatpak-session-helper` from there over the
nightly's.

## Checked on the phone

With the rebuilt packages, `luma-app-baseline.service` installs the whole
baseline (the Luma Platform runtime, its GL extension and 23 apps), and
`flatpak run` starts an app in its bubblewrap sandbox on the 5.4 kernel.
