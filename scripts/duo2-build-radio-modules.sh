#!/usr/bin/env bash
# Build the two kernel modules Wi-Fi and Bluetooth need that Microsoft's
# kernel tree does not carry, and add them to the staged module tree
# (docs/wifi-bt.md):
#
#   wlan.ko      Qualcomm's qcacld-3.0 driver for the QCA6490, from
#                CodeLinaro, at the release Microsoft's own build shipped
#                (host driver 2.0.8.31N, LA.UM.9.14.r1-21000).
#   hci_uart.ko  The kernel's own HCI UART line discipline, with H4 and the
#                QCA protocol; hci_qca is patched to run without serdev,
#                which this kernel does not have (kernel/radio/).
#
# Both are built out of tree against the vendor build (duo2-build-vendor.sh)
# and signed with its module key, since the kernel only loads signed
# modules. Run after duo2-stage-modules.sh, which starts the staged tree
# afresh: the modules go into its extra/ directory and depmod runs again.
#
# Usage: duo2-build-radio-modules.sh

set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/duo2-env.sh"

VTREE="${LUMA_DUO2_VENDOR_TREE:-$LUMA_DUO2_ROOT/../reference/msm-5.4}"
KSRC="$VTREE/kernel/msm-5.4"
KOBJ="$VTREE/out/target/product/duo2/obj/kernel/msm-5.4"
CLANG="$VTREE/prebuilts/clang/host/linux-x86/clang-r416183b/bin"
GCC="$VTREE/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-"
STAGE="${LUMA_DUO2_MODULES_OUT:-$LUMA_DUO2_ROOT/../build/duo2-modules}"
WORK="${LUMA_DUO2_RADIO_BUILD:-$LUMA_DUO2_ROOT/../build/radio-modules}"

CLO=https://git.codelinaro.org/clo/la/platform/vendor/qcom-opensource/wlan
# LA.UM.9.14.r1-21000-LAHAINA.QSSI12.0. qca-wifi-host-cmn's copy of that tag
# on CodeLinaro points at a qcacld-3.0 commit, so it is pinned by the
# release merge that tag should name (same day, "Release 2.0.8.31N" era).
QCACLD_REV=78f084631af1111066a6bedb8eafe5727460d81b
HOSTCMN_REV=abadbdc873d227d96ee648e4625a00120c88b4af
FWAPI_REV=b236035824dd2dca027ebeb394a4cd0d90b350c8

[ -f "$KOBJ/Module.symvers" ] || die "no vendor kernel build in $KOBJ -- run duo2-build-vendor.sh"
[ -f "$KOBJ/certs/signing_key.pem" ] || die "no module signing key in $KOBJ/certs"
RELEASE="$(cat "$KOBJ/include/config/kernel.release")"
[ -d "$STAGE/lib/modules/$RELEASE" ] || die "no staged modules for $RELEASE -- run duo2-stage-modules.sh"
command -v depmod >/dev/null || die "no depmod (kmod)"

kmake() {   # module dir, make arguments...
    local m="$1"; shift
    PATH="$CLANG:$VTREE/prebuilts/build-tools/linux-x86/bin:$PATH" \
    make -C "$KSRC" O="$KOBJ" M="$m" ARCH=arm64 CROSS_COMPILE="$GCC" \
        REAL_CC="$CLANG/clang" CLANG_TRIPLE=aarch64-linux-gnu- \
        AR="$CLANG/llvm-ar" LLVM_NM="$CLANG/llvm-nm" NM="$CLANG/llvm-nm" \
        LD="$CLANG/ld.lld" HOSTCC="$CLANG/clang" -j"$(nproc)" "$@"
}

fetch() {   # repository, revision, directory
    if [ "$(git -C "$3" rev-parse HEAD 2>/dev/null)" != "$2" ]; then
        info "fetching $1 at ${2:0:12}"
        rm -rf "$3"
        git init -q "$3"
        git -C "$3" fetch -q --depth 1 "$CLO/$1.git" "$2"
        git -C "$3" checkout -q FETCH_HEAD
    fi
}

install_ko() {   # built module, staged name
    local dest="$STAGE/lib/modules/$RELEASE/extra/$2"
    install -d "$(dirname "$dest")"
    "$CLANG/llvm-strip" --strip-debug -o "$dest" "$1"
    "$KOBJ/scripts/sign-file" sha1 "$KOBJ/certs/signing_key.pem" \
        "$KOBJ/certs/signing_key.x509" "$dest"
    chmod 0644 "$dest"
    info "$2: $(stat -c%s "$dest") bytes"
}

mkdir -p "$WORK"
SRC="$WORK/src"
mkdir -p "$SRC"
fetch qcacld-3.0 "$QCACLD_REV" "$SRC/qcacld-3.0"
fetch qca-wifi-host-cmn "$HOSTCMN_REV" "$SRC/qca-wifi-host-cmn"
fetch fw-api "$FWAPI_REV" "$SRC/fw-api"

# qcacld-3.0 as Android builds it for this chip (the "wlan" device with the
# default profile and QCA6490 features), except that a host-driver assertion
# logs instead of panicking the phone.
info "building wlan.ko (qcacld-3.0)"
kmake "$SRC/qcacld-3.0" modules \
    WLAN_ROOT="$SRC/qcacld-3.0" WLAN_COMMON_ROOT=../qca-wifi-host-cmn \
    WLAN_COMMON_INC="$SRC/qca-wifi-host-cmn" WLAN_FW_API="$SRC/fw-api" \
    MODNAME=wlan DEVNAME=wlan WLAN_PROFILE=default BOARD_PLATFORM=lahaina \
    CONFIG_QCA_CLD_WLAN=m CONFIG_QCA_WIFI_ISOC=0 CONFIG_QCA_WIFI_2_0=1 \
    CONFIG_CNSS_QCA6490=y \
    WLAN_CFG_OVERRIDE="PANIC_ON_BUG:=n WLAN_WARN_ON_ASSERT:=n" \
    >"$WORK/wlan-build.log" 2>&1 || die "wlan.ko build failed -- see $WORK/wlan-build.log"
install_ko "$SRC/qcacld-3.0/wlan.ko" wlan.ko

info "building hci_uart.ko (H4 + QCA line discipline)"
HU="$WORK/hci_uart"
rm -rf "$HU"
mkdir -p "$HU"
for f in hci_ldisc.c hci_h4.c hci_qca.c hci_uart.h btqca.c btqca.h; do
    cp "$KSRC/drivers/bluetooth/$f" "$HU/"
done
patch -s -d "$HU" -p3 <"$LUMA_DUO2_ROOT/kernel/radio/hci_qca-ldisc.patch" ||
    die "kernel/radio/hci_qca-ldisc.patch does not apply"
cat >"$HU/Kbuild" <<'EOF'
obj-m := hci_uart.o
hci_uart-y := hci_ldisc.o hci_h4.o hci_qca.o btqca.o
ccflags-y += -DCONFIG_BT_HCIUART_H4 -DCONFIG_BT_HCIUART_QCA -DCONFIG_BT_QCA_MODULE=1
ccflags-y += -I$(srctree)/drivers/bluetooth
EOF
kmake "$HU" modules >"$WORK/hci_uart-build.log" 2>&1 ||
    die "hci_uart.ko build failed -- see $WORK/hci_uart-build.log"
install_ko "$HU/hci_uart.ko" hci_uart.ko

depmod -b "$STAGE" "$RELEASE"
info "radio modules staged in $STAGE/lib/modules/$RELEASE/extra"
