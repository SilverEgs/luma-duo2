#!/usr/bin/env bash
# Rebuild Fedora 44's flatpak for the Duo 2's msm-5.4 kernel (docs/flatpak.md).
#
# flatpak 1.18 resolves paths with libglnx's chase, which asks statx() for
# mount ids, and kernels before 5.8 do not return them: every install failed,
# so Luma's offline application baseline never installed and the Shell kept
# asking to repair the installation. packaging/flatpak/ carries the fix (the
# mount id from /proc/self/fdinfo instead). This rebuilds Fedora's exact
# source package with it, in the Luma builder (duo2-luma-builder.sh's arm64
# Fedora 44 container), under its own release, into
# luma/build/packages/flatpak-duo2/ for duo2-compose-ostree.sh to layer.
#
# Usage: duo2-build-flatpak.sh

set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/duo2-env.sh"

LUMA="$(cd "${LUMA_DUO2_LUMA_TREE:-$LUMA_DUO2_ROOT/../luma}" && pwd)"
NAME="${LUMA_DUO2_BUILDER_NAME:-luma-duo2-builder}"
SRPM_URL=https://kojipkgs.fedoraproject.org/packages/flatpak/1.18.4/1.fc44/src/flatpak-1.18.4-1.fc44.src.rpm
SRPM_SHA256=ddb9f0c8693e3021a00e4f16af08c8e5126569bae6f50f99f2f689b86696921a
RELEASE_SUFFIX=duo2.1

docker container inspect "$NAME" >/dev/null 2>&1 ||
    die "no builder $NAME -- run duo2-luma-builder.sh once to create it"
[ "$(docker inspect --format '{{.State.Running}}' "$NAME")" = true ] || docker start "$NAME" >/dev/null

PATCHES="$LUMA_DUO2_ROOT/packaging/flatpak"
STAGE="$LUMA/build/packages/flatpak-duo2/patches"
docker exec "$NAME" mkdir -p /luma/build/packages/flatpak-duo2/patches
for p in "$PATCHES"/*.patch; do
    docker cp "$p" "$NAME:/luma/build/packages/flatpak-duo2/patches/"
done

info "rebuilding flatpak with $(ls "$PATCHES"/*.patch | wc -l) patch(es) (qemu-user, slow)"
docker exec -e SRPM_URL="$SRPM_URL" -e SRPM_SHA256="$SRPM_SHA256" -e SUFFIX="$RELEASE_SUFFIX" \
    "$NAME" bash -euc '
    out=/luma/build/packages/flatpak-duo2
    top=$out/rpmbuild
    srpm=/luma/build/cache/srpm/$(basename "$SRPM_URL")
    mkdir -p /luma/build/cache/srpm
    [ -f "$srpm" ] || curl -fL --retry 3 -o "$srpm" "$SRPM_URL"
    echo "$SRPM_SHA256  $srpm" | sha256sum -c --quiet
    rm -rf "$top" "$out/RPMS"
    mkdir -p "$top"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
    rpm -i --define "_topdir $top" "$srpm"
    spec=$top/SPECS/flatpak.spec
    n=9000
    for p in $out/patches/*.patch; do
        cp "$p" "$top/SOURCES/"
        sed -i "0,/^Source[0-9]*:/s//Patch$n: $(basename "$p")\n&/" "$spec"
        n=$((n + 1))
    done
    # The spec applies its patches with %autosetup -p1; ours join them.
    grep -q "^%autosetup -p1" "$spec" || { echo "flatpak.spec no longer uses %autosetup -p1" >&2; exit 1; }
    # Fedora built 1.fc44 (%autorelease, the first release); ours follows it.
    grep -q "^Release:[[:space:]]*%autorelease" "$spec" || { echo "flatpak.spec Release changed" >&2; exit 1; }
    sed -i "s/^Release:[[:space:]]*%autorelease/Release:        1.$SUFFIX%{?dist}/" "$spec"
    grep -E "^(Release|Patch9)" "$spec"
    dnf -y -q builddep "$spec" >/dev/null
    rpmbuild --define "_topdir $top" --nocheck -bb "$spec" >"$out/rpmbuild.log" 2>&1 ||
        { tail -40 "$out/rpmbuild.log" >&2; exit 1; }
    mkdir -p "$out/RPMS"
    cp "$top"/RPMS/*/flatpak-1*.rpm "$top"/RPMS/*/flatpak-libs-1*.rpm \
       "$top"/RPMS/*/flatpak-session-helper-1*.rpm "$out/RPMS/"
'
ls "$LUMA/build/packages/flatpak-duo2/RPMS/"
info "flatpak staged in $LUMA/build/packages/flatpak-duo2/RPMS"
