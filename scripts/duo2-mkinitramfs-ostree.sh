#!/usr/bin/env bash
# The Duo 2 initramfs for Luma's OSTree phone layout.
#
# Luma's phone boot images are the postmarketOS initramfs plus one more cpio
# archive (scripts/mobile/build-fp6-boot-image.sh): Luma's init_functions.sh,
# which leaves /boot on the root filesystem and hands the root to
# ostree-prepare-root, and the shared boot resolver that points
# /ostree/root.<slot> at ostree's default deployment on every boot
# (scripts/mobile/common/initramfs). This appends the same archive to the
# Duo 2's own initramfs (duo2-mkinitramfs-pmos.sh: postmarketOS's, with the
# way home), plus:
#
#   - Luma's ostree-prepare-root and its glibc libraries. Luma builds ostree
#     with patches/ostree/0001-luma-prepare-root-marker.patch, which lets
#     init_functions.sh create /run/ostree/initramfs-mount-var before
#     ostree-prepare-root runs; Fedora's ostree in the deployment refuses
#     ("Failed to create /run/ostree: File exists"). The binary and its
#     libraries come from the archive of Luma's published Fairphone 6 boot
#     image that carries them (pinned below, checked by SHA-256);
#   - /usr/lib/ostree/prepare-root.conf with composefs off: the nightly asks
#     for composefs, which needs data-only overlayfs lower layers (Linux 6.5)
#     that Microsoft's 5.4 kernel does not have;
#   - /etc/luma-fp6-ostree, the marker Luma's scripts take as "this is an
#     OSTree phone" (the name is the FP6's; the scripts are shared).
#
# Output: build/duo2-initramfs/initramfs-luma-ostree

set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/duo2-env.sh"

BUILD="$LUMA_DUO2_ROOT/../build"
LUMA="${LUMA_SOURCE:-$LUMA_DUO2_ROOT/../luma}"
IN="${LUMA_DUO2_INITRAMFS_OUT:-$BUILD/duo2-initramfs}"
# Luma's published Fairphone 6 boot image, kit 20261111.1 (devices/fairphone/fp6/INSTALL.md).
FP6_BOOT_URL="https://media.simplyluma.com/mobile/fp6/kit-20261111.1/luma-fp6-20261111.1-boot.bin"
FP6_BOOT_SHA256="62ece6566207a81327a4d1883a383a53e97beacd4591e48aa9cd779a8dfba8af"
FP6_BOOT="${LUMA_FP6_BOOT:-$BUILD/fp6-kit-20261111.1/luma-fp6-20261111.1-boot.bin}"
UNPACK="${LUMA_DUO2_UNPACK:-$LUMA_DUO2_ROOT/../tools/mkbootimg/unpack_bootimg.py}"
BASE="$IN/initramfs-luma"
OUT="$IN/initramfs-luma-ostree"

[ -f "$BASE" ] || die "no $BASE -- run duo2-mkinitramfs-pmos.sh"
if [ ! -f "$FP6_BOOT" ]; then
    info "fetching Luma's Fairphone 6 boot image for its ostree-prepare-root"
    install -d "$(dirname "$FP6_BOOT")"
    curl -sSf -o "$FP6_BOOT" "$FP6_BOOT_URL"
fi
echo "$FP6_BOOT_SHA256  $FP6_BOOT" | sha256sum -c --quiet - || die "$FP6_BOOT does not match its pinned SHA-256"
shared="$LUMA/scripts/mobile/common/initramfs"
for f in "$LUMA/scripts/mobile/fp6-native-init-functions.sh" "$shared/luma-ostree-boot.sh" \
         "$shared/hooks-extra/50-luma-ostree-default.sh"; do
    [ -f "$f" ] || die "no $f -- is $LUMA a current Luma checkout?"
done

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
o="$STAGE/root"
install -d "$o"
# The FP6 ramdisk is several concatenated cpio archives; take the files below
# from whichever one holds ostree-prepare-root.
python3 -I "$UNPACK" --boot_img "$FP6_BOOT" --out "$STAGE/fp6" >/dev/null
gzip -dc "$STAGE/fp6/ramdisk" >"$STAGE/fp6.cpio"
python3 -I - "$STAGE/fp6.cpio" "$STAGE/seg" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
start, n = 0, 0
while start < len(data):
    end = data.find(b"TRAILER!!!", start)
    if end < 0:
        break
    end += len(b"TRAILER!!!")
    while end < len(data) and data[end] == 0:
        end += 1
    open(f"{sys.argv[2]}{n}.cpio", "wb").write(data[start:end])
    start, n = end, n + 1
PY
found=
for seg in "$STAGE"/seg*.cpio; do
    members="$(cpio -it --quiet <"$seg" 2>/dev/null || true)"
    if printf '%s\n' "$members" | grep -qx 'usr/lib/ostree/ostree-prepare-root'; then
        (cd "$o" && cpio -idm --quiet --no-absolute-filenames \
            'usr/lib/ostree/ostree-prepare-root' 'usr/lib/ld-linux-aarch64.so.1' 'usr/lib64/*' <"$seg")
        found=1
    fi
done
[ -n "$found" ] && [ -x "$o/usr/lib/ostree/ostree-prepare-root" ] || \
    die "no ostree-prepare-root in $FP6_BOOT"
grep -aq 'luma' "$o/usr/lib/ostree/ostree-prepare-root" || \
    die "$FP6_BOOT's ostree-prepare-root is not Luma's build"
install -D -m 0644 "$LUMA/scripts/mobile/fp6-native-init-functions.sh" "$o/init_functions.sh"
install -D -m 0644 "$shared/luma-ostree-boot.sh" "$o/usr/lib/luma/ostree-boot.sh"
install -D -m 0755 "$shared/hooks-extra/50-luma-ostree-default.sh" "$o/hooks-extra/50-luma-ostree-default.sh"
install -D -m 0644 /dev/null "$o/etc/luma-fp6-ostree"
install -D -m 0644 /dev/stdin "$o/usr/lib/ostree/prepare-root.conf" <<'EOF'
# Luma Duo 2: Microsoft's 5.4 kernel cannot mount composefs (data-only
# overlayfs lower layers arrived in Linux 6.5).
[composefs]
enabled = no
[sysroot]
readonly = true
EOF

(cd "$o" && find . -mindepth 1 | LC_ALL=C sort | cpio --quiet --reproducible -o -H newc -R 0:0) \
    | gzip -9n >"$STAGE/ostree.cpio.gz"
cat "$BASE" "$STAGE/ostree.cpio.gz" >"$OUT"
info "Luma OSTree archive $(stat -c%s "$STAGE/ostree.cpio.gz") bytes -> $OUT ($(stat -c%s "$OUT") bytes)"
