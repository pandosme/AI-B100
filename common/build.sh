#!/bin/sh
# Shared ACAP build driver.
#
# Each app's build.sh is a one-line wrapper around this script. The build is an
# overlay: common/app is staged first, then the app's own app/ directory is
# copied on top. The ACAP SDK needs a single flat source directory, so the
# staged result is what Docker actually builds.
#
# Files under common/app must never be shadowed by an app-specific file of the
# same path. check-shared.sh enforces that; this script refuses to build if it
# reports a collision.
set -e

APP_DIR=$(cd "$(dirname "$0")/.." && pwd)
[ -n "$1" ] && APP_DIR=$(cd "$1" && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
COMMON="$ROOT/common"

if [ ! -d "$APP_DIR/app" ]; then
	echo "error: no app/ directory in $APP_DIR" >&2
	exit 1
fi

"$COMMON/check-shared.sh"

STAGE="$APP_DIR/.stage"
rm -rf "$STAGE" "$APP_DIR/build"
mkdir -p "$STAGE"
cp -R "$COMMON/app/." "$STAGE/"
cp -R "$APP_DIR/app/." "$STAGE/"

cd "$APP_DIR"

build_arch() {
	docker build --progress=plain --no-cache --build-arg ARCH="$1" --tag acap .
	CONTAINER_ID=$(docker create acap)
	docker cp "$CONTAINER_ID":/opt/app ./build
	docker rm "$CONTAINER_ID"
	mv build/*.eap .
	rm -rf build
}

build_arch aarch64
build_arch armv7hf

rm -rf "$STAGE"
