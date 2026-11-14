#!/usr/bin/env bash
# Build what the cellular modem needs that neither Fedora nor Microsoft's
# kernel provides as is (docs/modem.md):
#
#   userspace   qrtr's libqrtr, tqftpserv and pd-mapper, each with one patch
#               from packaging/ for Qualcomm's downstream subsystem/PIL
#               kernel, and luma-duo2-ipa-wan (tools/duo2-ipa-wan/), which
#               connects the vendor IPA's WAN pipes as Android's netmgrd
#               does; built in a Fedora 44 arm64 container (Luma's pinned
#               build image, under qemu-user) and staged as a /usr tree in
#               build/duo2-modem for duo2-compose-ostree.sh to copy.
#   modules     duo2_ipa_dtfix and duo2_ipa_netparent (kernel/modem/), and
#               the kernel's own rmnet driver (drivers/net/ethernet/qualcomm/
#               rmnet, CONFIG_RMNET is off in Microsoft's configuration) for
#               ModemManager's data links; built out of tree against the
#               vendor build and signed with its module key, into the staged
#               module tree's extra/ directory. Run after
#               duo2-stage-modules.sh, which starts that tree afresh.
#
# Usage: duo2-build-modem.sh

set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/duo2-env.sh"

BUILD="$LUMA_DUO2_ROOT/../build"
WORK="${LUMA_DUO2_MODEM_BUILD:-$BUILD/modem-build}"
OUT="${LUMA_DUO2_MODEM_OUT:-$BUILD/duo2-modem}"
LUMA="${LUMA_SOURCE:-$LUMA_DUO2_ROOT/../luma}"
VTREE="${LUMA_DUO2_VENDOR_TREE:-$LUMA_DUO2_ROOT/../reference/msm-5.4}"
KSRC="$VTREE/kernel/msm-5.4"
KOBJ="$VTREE/out/target/product/duo2/obj/kernel/msm-5.4"
CLANG="$VTREE/prebuilts/clang/host/linux-x86/clang-r416183b/bin"
GCC="$VTREE/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-"
STAGE="${LUMA_DUO2_MODULES_OUT:-$BUILD/duo2-modules}"
PKG="$LUMA_DUO2_ROOT/packaging"
NAME=duo2-modem-build

# Upstream releases matching Fedora 44's qrtr-1.2 and tqftpserv-1.3; pd-mapper
# is not packaged by Fedora.
QRTR_REV=b51ffaf22707b6000ecfb894c5b750f3bb7843b2        # v1.2
TQFTPSERV_REV=b31de6db4f31d09119293203d39636b827f16321   # v1.3
PDMAPPER_REV=5ecd2fe926aca7abfe40724177f63b942cff3947

IMAGE="$(. "$LUMA/config/desktop/inputs.env" && echo "$FEDORA_RPM_BUILD_CONTAINER_AARCH64")"
[ -n "$IMAGE" ] || die "no FEDORA_RPM_BUILD_CONTAINER_AARCH64 in Luma's inputs.env"
[ -f "$KOBJ/Module.symvers" ] || die "no vendor kernel build in $KOBJ -- run duo2-build-vendor.sh"
[ -f "$KOBJ/certs/signing_key.pem" ] || die "no module signing key in $KOBJ/certs"
RELEASE="$(cat "$KOBJ/include/config/kernel.release")"
[ -d "$STAGE/lib/modules/$RELEASE" ] || die "no staged modules for $RELEASE -- run duo2-stage-modules.sh"
command -v depmod >/dev/null || die "no depmod (kmod)"

fetch() {   # repository, revision, directory
    if [ "$(git -C "$3" rev-parse HEAD 2>/dev/null)" != "$2" ]; then
        info "fetching $1 at ${2:0:12}"
        rm -rf "$3"
        git init -q "$3"
        git -C "$3" fetch -q --depth 1 "https://github.com/linux-msm/$1.git" "$2"
        git -C "$3" checkout -q FETCH_HEAD
    fi
    git -C "$3" checkout -q -- . && git -C "$3" clean -qfdx
}

mkdir -p "$WORK/src"
fetch qrtr "$QRTR_REV" "$WORK/src/qrtr"
fetch tqftpserv "$TQFTPSERV_REV" "$WORK/src/tqftpserv"
fetch pd-mapper "$PDMAPPER_REV" "$WORK/src/pd-mapper"
for p in qrtr tqftpserv pd-mapper; do
    for f in "$PKG/$p"/*.patch; do
        info "applying $p/$(basename "$f")"
        git -C "$WORK/src/$p" apply "$f"
    done
done
rm -rf "$WORK/src/duo2-ipa-wan"
cp -a "$LUMA_DUO2_ROOT/tools/duo2-ipa-wan" "$WORK/src/duo2-ipa-wan"

# --- userspace, in a persistent arm64 Fedora container ----------------------
# A container left from a build with another LUMA_DUO2_MODEM_BUILD mounts the
# wrong /work; start it afresh.
mounted="$(docker inspect --format '{{range .Mounts}}{{if eq .Destination "/work"}}{{.Source}}{{end}}{{end}}' "$NAME" 2>/dev/null || true)"
if [ -n "$mounted" ] && [ "$mounted" != "$(cd "$WORK" && pwd)" ]; then
    info "$NAME mounts $mounted, not $WORK; recreating it"
    docker rm -f "$NAME" >/dev/null
fi
if ! docker container inspect "$NAME" >/dev/null 2>&1; then
    info "creating $NAME from $IMAGE (arm64)"
    docker run --detach --init --platform linux/arm64 --name "$NAME" \
        -v "$WORK:/work" "$IMAGE" sleep infinity >/dev/null
    docker exec "$NAME" dnf -y -q install gcc make meson ninja-build pkgconf \
        xz-devel libzstd-devel systemd-rpm-macros >/dev/null
fi
[ "$(docker inspect --format '{{.State.Running}}' "$NAME")" = true ] || docker start "$NAME" >/dev/null

info "building libqrtr, tqftpserv, pd-mapper, luma-duo2-ipa-wan (qemu-user, slow)"
docker exec "$NAME" bash -euc '
    rm -rf /work/root && mkdir -p /work/root
    cd /work/src/qrtr
    rm -rf b && meson setup b --prefix=/usr --libdir=/usr/lib64 -Dqrtr-ns=disabled >/dev/null
    ninja -C b >/dev/null
    DESTDIR=/work/qrtr-root ninja -C b install >/dev/null
    install -D -m 0755 /work/qrtr-root/usr/lib64/libqrtr.so.1.2 /work/root/usr/lib64/libqrtr.so.1.2
    ln -sf libqrtr.so.1.2 /work/root/usr/lib64/libqrtr.so.1
    cd /work/src/tqftpserv && rm -rf b
    PKG_CONFIG_PATH=/work/qrtr-root/usr/lib64/pkgconfig \
        meson setup b --prefix=/usr -Dsystemd-unit-prefix=/usr/lib/systemd/system \
        -Dc_args=-I/work/qrtr-root/usr/include -Dc_link_args=-L/work/qrtr-root/usr/lib64 >/dev/null
    ninja -C b >/dev/null
    install -D -m 0755 b/tqftpserv /work/root/usr/bin/tqftpserv
    install -D -m 0644 b/tqftpserv.service /work/root/usr/lib/systemd/system/tqftpserv.service
    cd /work/src/pd-mapper
    make -s clean >/dev/null 2>&1 || true
    make -s CFLAGS="-O2 -g -Wall -I/work/qrtr-root/usr/include" \
        LDFLAGS="-L/work/qrtr-root/usr/lib64 -lqrtr -llzma" >/dev/null
    install -D -m 0755 pd-mapper /work/root/usr/bin/pd-mapper
    cd /work/src/duo2-ipa-wan
    gcc -O2 -g -Wall -Wextra -o luma-duo2-ipa-wan luma-duo2-ipa-wan.c
    install -D -m 0755 luma-duo2-ipa-wan /work/root/usr/libexec/luma-duo2-ipa-wan
    for l in qrtr tqftpserv pd-mapper; do
        install -D -m 0644 /work/src/$l/LICENSE /work/root/usr/share/licenses/luma-duo2-modem/$l.LICENSE
    done
    chown -R '"$(id -u):$(id -g)"' /work
'
rm -rf "$OUT"
cp -a "$WORK/root" "$OUT"
info "userspace staged in $OUT"

# --- kernel modules ----------------------------------------------------------
kmake() {   # module dir, make arguments...
    local m="$1"; shift
    PATH="$CLANG:$VTREE/prebuilts/build-tools/linux-x86/bin:$PATH" \
    make -C "$KSRC" O="$KOBJ" M="$m" ARCH=arm64 CROSS_COMPILE="$GCC" \
        REAL_CC="$CLANG/clang" CLANG_TRIPLE=aarch64-linux-gnu- \
        AR="$CLANG/llvm-ar" LLVM_NM="$CLANG/llvm-nm" NM="$CLANG/llvm-nm" \
        LD="$CLANG/ld.lld" HOSTCC="$CLANG/clang" -j"$(nproc)" "$@" >/dev/null
}
KM="$WORK/kmod"
rm -rf "$KM"; cp -a "$LUMA_DUO2_ROOT/kernel/modem" "$KM"
kmake "$KM" modules
# The kernel's own rmnet, unchanged, from Microsoft's tree.
RM="$WORK/rmnet"
rm -rf "$RM"; cp -a "$KSRC/drivers/net/ethernet/qualcomm/rmnet" "$RM"
kmake "$RM" CONFIG_RMNET=m modules
for ko in "$KM/duo2_ipa_dtfix.ko" "$KM/duo2_ipa_netparent.ko" "$RM/rmnet.ko"; do
    dest="$STAGE/lib/modules/$RELEASE/extra/luma-duo2-modem/$(basename "$ko")"
    install -d "$(dirname "$dest")"
    "$CLANG/llvm-strip" --strip-debug -o "$dest" "$ko"
    "$KOBJ/scripts/sign-file" sha1 "$KOBJ/certs/signing_key.pem" \
        "$KOBJ/certs/signing_key.x509" "$dest"
    info "$(basename "$ko"): $(stat -c%s "$dest") bytes"
done
depmod -b "$STAGE" "$RELEASE"
info "modules staged in $STAGE/lib/modules/$RELEASE/extra/luma-duo2-modem"
