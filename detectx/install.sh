#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
	echo "Usage: $0 <camera-host> <eap-file>" >&2
	exit 1
fi

CAMERA_HOST=$1
EAP_FILE=$2
: "${CAMERA_USER:=root}"

if [ ! -f "$EAP_FILE" ]; then
	echo "error: $EAP_FILE not found" >&2
	exit 1
fi

echo "Installing $EAP_FILE on $CAMERA_HOST as $CAMERA_USER"
curl --digest -u "$CAMERA_USER" -F "packfil=@$EAP_FILE;type=application/octet-stream" \
	"http://$CAMERA_HOST/axis-cgi/applications/upload.cgi"