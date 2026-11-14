#!/usr/bin/env bash
# Build nf_tables and the nftables expressions firewalld (Luma's firewall,
# nftables backend) and iptables-nft need, and add them to the staged module
# tree (docs/firewall.md).
#
# Microsoft's kernel has netfilter, conntrack, NAT and the legacy x_tables
# built in, but CONFIG_NF_TABLES is off. nf_tables only needs exported
# kernel functions, so it is built here out of tree from the vendor tree's
# own sources (net/netfilter, net/ipv4/netfilter, net/ipv6/netfilter) with
# two patches from kernel/netfilter/:
#
#   0001  struct net has no "nft" member in a kernel without CONFIG_NF_TABLES
#         (net_namespace.h), so the per-namespace nf_tables state moves to
#         net_generic storage. The built-in code and these modules then
#         agree on struct net.
#   0002  nf_nat_inet_register_fn() is not built in (it needs
#         CONFIG_NF_TABLES_INET and CONFIG_NFT_NAT), so nft_chain_nat
#         registers inet NAT chains through the per-family functions.
#
# The kernel's generated autoconf.h is left alone: the inet, ipv4, ipv6 and
# netdev families are switched on with -D flags that only the copied
# sources look at, and the patched nf_tables.h is found ahead of the
# kernel's own through NOSTDINC_FLAGS.
#
# Everything is built against the vendor build (duo2-build-vendor.sh) and
# signed with its module key. Run after duo2-stage-modules.sh, which starts
# the staged tree afresh: the modules go into its extra/luma-duo2-netfilter
# directory and depmod runs again.
#
# Usage: duo2-build-netfilter-modules.sh

set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/duo2-env.sh"

VTREE="${LUMA_DUO2_VENDOR_TREE:-$LUMA_DUO2_ROOT/../reference/msm-5.4}"
KSRC="$VTREE/kernel/msm-5.4"
KOBJ="$VTREE/out/target/product/duo2/obj/kernel/msm-5.4"
CLANG="$VTREE/prebuilts/clang/host/linux-x86/clang-r416183b/bin"
GCC="$VTREE/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-"
STAGE="${LUMA_DUO2_MODULES_OUT:-$LUMA_DUO2_ROOT/../build/duo2-modules}"
WORK="${LUMA_DUO2_NETFILTER_BUILD:-$LUMA_DUO2_ROOT/../build/netfilter-modules}"
PATCHES="$LUMA_DUO2_ROOT/kernel/netfilter"

[ -f "$KOBJ/Module.symvers" ] || die "no vendor kernel build in $KOBJ -- run duo2-build-vendor.sh"
[ -f "$KOBJ/certs/signing_key.pem" ] || die "no module signing key in $KOBJ/certs"
RELEASE="$(cat "$KOBJ/include/config/kernel.release")"
[ -d "$STAGE/lib/modules/$RELEASE" ] || die "no staged modules for $RELEASE -- run duo2-stage-modules.sh"
command -v depmod >/dev/null || die "no depmod (kmod)"
grep -q '^# CONFIG_NF_TABLES is not set' "$KOBJ/.config" ||
    die "the vendor kernel has CONFIG_NF_TABLES set; these modules are for one without it"

# module: objects (sources by kernel path; each is copied flat into the build)
MODULES=(
    "nf_tables: net/netfilter/nf_tables_core net/netfilter/nf_tables_api
        net/netfilter/nft_chain_filter net/netfilter/nf_tables_trace
        net/netfilter/nft_immediate net/netfilter/nft_cmp net/netfilter/nft_range
        net/netfilter/nft_bitwise net/netfilter/nft_byteorder
        net/netfilter/nft_payload net/netfilter/nft_lookup net/netfilter/nft_dynset
        net/netfilter/nft_meta net/netfilter/nft_rt net/netfilter/nft_exthdr
        net/netfilter/nft_chain_route net/netfilter/nf_tables_offload"
    "nf_tables_set: net/netfilter/nf_tables_set_core net/netfilter/nft_set_hash
        net/netfilter/nft_set_bitmap net/netfilter/nft_set_rbtree"
    "nft_compat: net/netfilter/nft_compat"
    "nft_ct: net/netfilter/nft_ct"
    "nft_counter: net/netfilter/nft_counter"
    "nft_limit: net/netfilter/nft_limit"
    "nft_quota: net/netfilter/nft_quota"
    "nft_objref: net/netfilter/nft_objref"
    "nft_hash: net/netfilter/nft_hash"
    "nft_numgen: net/netfilter/nft_numgen"
    "nft_connlimit: net/netfilter/nft_connlimit"
    "nft_socket: net/netfilter/nft_socket"
    "nft_log: net/netfilter/nft_log"
    "nft_nat: net/netfilter/nft_nat"
    "nft_chain_nat: net/netfilter/nft_chain_nat"
    "nft_masq: net/netfilter/nft_masq"
    "nft_redir: net/netfilter/nft_redir"
    "nft_reject: net/netfilter/nft_reject"
    "nft_reject_inet: net/netfilter/nft_reject_inet"
    "nft_reject_ipv4: net/ipv4/netfilter/nft_reject_ipv4"
    "nft_reject_ipv6: net/ipv6/netfilter/nft_reject_ipv6"
    "nft_fib: net/netfilter/nft_fib"
    "nft_fib_inet: net/netfilter/nft_fib_inet"
    "nft_fib_ipv4: net/ipv4/netfilter/nft_fib_ipv4"
    "nft_fib_ipv6: net/ipv6/netfilter/nft_fib_ipv6"
    "nf_log_common: net/netfilter/nf_log_common"
    "nf_log_ipv4: net/ipv4/netfilter/nf_log_ipv4"
    "nf_log_ipv6: net/ipv6/netfilter/nf_log_ipv6"
)

kmake() {   # module dir, make arguments...
    local m="$1"; shift
    PATH="$CLANG:$VTREE/prebuilts/build-tools/linux-x86/bin:$PATH" \
    make -C "$KSRC" O="$KOBJ" M="$m" ARCH=arm64 CROSS_COMPILE="$GCC" \
        REAL_CC="$CLANG/clang" CLANG_TRIPLE=aarch64-linux-gnu- \
        AR="$CLANG/llvm-ar" LLVM_NM="$CLANG/llvm-nm" NM="$CLANG/llvm-nm" \
        LD="$CLANG/ld.lld" HOSTCC="$CLANG/clang" -j"$(nproc)" "$@"
}

# --- sources: the vendor tree's files, patched ------------------------------
TREE="$WORK/tree"
KM="$WORK/kmod"
rm -rf "$TREE" "$KM"
mkdir -p "$TREE" "$KM/include/net/netfilter"
srcs=(include/net/netfilter/nf_tables.h)
for m in "${MODULES[@]}"; do
    for o in ${m#*:}; do srcs+=("$o.c"); done
done
for f in "${srcs[@]}"; do
    install -D -m 0644 "$KSRC/$f" "$TREE/$f"
done
for p in "$PATCHES"/*.patch; do
    info "applying netfilter/$(basename "$p")"
    patch -d "$TREE" -p1 --batch --forward --quiet -i "$p" </dev/null ||
        die "$p does not apply"
done
cp "$TREE/include/net/netfilter/nf_tables.h" "$KM/include/net/netfilter/"

{
    echo '# Generated by duo2-build-netfilter-modules.sh'
    echo '# The patched nf_tables.h comes ahead of the kernel'"'"'s include/.'
    echo 'NOSTDINC_FLAGS := -I$(src)/include $(NOSTDINC_FLAGS)'
    echo 'ccflags-y += -DCONFIG_NF_TABLES_INET=1 -DCONFIG_NF_TABLES_IPV4=1'
    echo 'ccflags-y += -DCONFIG_NF_TABLES_IPV6=1 -DCONFIG_NF_TABLES_NETDEV=1'
    for m in "${MODULES[@]}"; do
        name="${m%%:*}"
        objs=""
        for o in ${m#*:}; do
            cp "$TREE/$o.c" "$KM/"
            objs+=" $(basename "$o").o"
        done
        echo "obj-m += $name.o"
        # A single-object module whose source has its own name needs no list.
        [ "$objs" = " $name.o" ] || echo "$name-y :=$objs"
    done
} >"$KM/Kbuild"

# --- build, check, sign, stage -----------------------------------------------
info "building nf_tables and nft modules for $RELEASE"
kmake "$KM" modules >"$WORK/build.log" 2>&1 || die "build failed -- see $WORK/build.log"
# modpost names any symbol the kernel does not export; such a module would
# not load.
if grep -E 'WARNING: .*undefined!' "$WORK/build.log"; then
    die "unresolved symbols -- see $WORK/build.log"
fi

DEST="$STAGE/lib/modules/$RELEASE/extra/luma-duo2-netfilter"
rm -rf "$DEST"
install -d "$DEST"
for m in "${MODULES[@]}"; do
    name="${m%%:*}"
    ko="$KM/$name.ko"
    [ -f "$ko" ] || die "no $ko"
    "$CLANG/llvm-strip" --strip-debug -o "$DEST/$name.ko" "$ko"
    "$KOBJ/scripts/sign-file" sha1 "$KOBJ/certs/signing_key.pem" \
        "$KOBJ/certs/signing_key.x509" "$DEST/$name.ko"
    chmod 0644 "$DEST/$name.ko"
done
info "$(ls "$DEST" | wc -l) modules, $(du -sh "$DEST" | cut -f1)"
depmod -b "$STAGE" "$RELEASE"
info "netfilter modules staged in $DEST"
