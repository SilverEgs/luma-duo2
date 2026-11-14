#!/usr/bin/env bash
# Build duo2-touch, the Duo 2's native touch daemon (tools/duo2-touch), for
# the phone.
#
# With an aarch64 cross compiler (CROSS_COMPILE, default aarch64-linux-gnu-)
# it builds on the workstation; otherwise inside the arm64 Fedora builder
# that duo2-luma-builder.sh keeps (luma-duo2-builder), the same glibc the
# nightly runs.
#
# Output: build/tools/duo2-touch

set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/duo2-env.sh"

SRC="$LUMA_DUO2_ROOT/tools/duo2-touch"
OUT="$LUMA_DUO2_ROOT/../build/tools"
NAME="${LUMA_DUO2_BUILDER_NAME:-luma-duo2-builder}"
# No fused multiply-adds: the stock library's arithmetic is reproduced
# operation for operation (tools/duo2-touch/heat.c).
CC_FLAGS=(-O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE -ffp-contract=off)
SOURCES=(duo2-touch.c heat.c tracks.c tracklib.c)

install -d "$OUT"
cc="${CROSS_COMPILE:-aarch64-linux-gnu-}gcc"
if command -v "$cc" >/dev/null 2>&1; then
    (cd "$SRC" && "$cc" "${CC_FLAGS[@]}" -o "$OUT/duo2-touch" "${SOURCES[@]}" -lm)
else
    docker container inspect "$NAME" >/dev/null 2>&1 || \
        die "no $cc and no $NAME builder -- run duo2-luma-builder.sh --shell once to create it"
    docker start "$NAME" >/dev/null
    docker exec "$NAME" rm -rf /tmp/duo2-touch-build
    docker exec "$NAME" mkdir -p /tmp/duo2-touch-build
    for f in "${SOURCES[@]}" heat.h; do docker cp "$SRC/$f" "$NAME:/tmp/duo2-touch-build/$f"; done
    docker exec -w /tmp/duo2-touch-build "$NAME" gcc "${CC_FLAGS[@]}" -o duo2-touch "${SOURCES[@]}" -lm
    docker cp "$NAME:/tmp/duo2-touch-build/duo2-touch" "$OUT/duo2-touch"
fi
file "$OUT/duo2-touch" | grep -q 'ARM aarch64' || die "$OUT/duo2-touch is not an aarch64 binary"
info "built $OUT/duo2-touch"
