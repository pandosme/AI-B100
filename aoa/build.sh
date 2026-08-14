#!/bin/sh
# Wrapper: the real build lives in ../common/build.sh (overlay of common/app + app/).
exec "$(dirname "$0")/../common/build.sh" "$(dirname "$0")"
