#!/bin/sh
# Fails if any app keeps its own copy of a file that lives in common/app.
#
# The overlay build copies common/app first and the app's app/ directory second,
# so a same-path file in an app would silently win and drift from common without
# any visible error. That is exactly the failure this repo already had, where
# aoa/ ran months behind radar/ on ACAP.c and B100.c. Catch it at build time.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
COMMON="$ROOT/common/app"
status=0

for app_dir in "$ROOT"/*/; do
	app=$(basename "$app_dir")
	[ "$app" = "common" ] && continue
	[ "$app" = "images" ] && continue
	[ -d "$app_dir/app" ] || continue

	for shared in $(cd "$COMMON" && find . -type f | sed 's|^\./||'); do
		if [ -e "$app_dir/app/$shared" ]; then
			echo "drift: $app/app/$shared shadows common/app/$shared" >&2
			status=1
		fi
	done
done

if [ "$status" -ne 0 ]; then
	echo "" >&2
	echo "Shared files are edited in common/app only. Delete the app-local copy," >&2
	echo "or move the change into common/app so every app gets it." >&2
	exit 1
fi

echo "check-shared: ok"
