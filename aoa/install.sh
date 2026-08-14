#!/bin/sh -eu
#
# Install AI-B100 AOA Counter EAP to Axis camera

if [ $# -ne 2 ]; then
	echo "Usage: $0 <camera-host> <eap-file>" >&2
	echo "Example: $0 back.intern AI-B100_AOA_Counter_1_0_1_aarch64.eap" >&2
	exit 1
fi

CAMERA_HOST="$1"
EAP_FILE="$2"
USERNAME=nodered
PASSWORD=rednode

if [ ! -f "$EAP_FILE" ]; then
	echo "ERROR: $EAP_FILE not found in the current directory" >&2
	exit 1
fi

echo "Installing $EAP_FILE to $CAMERA_HOST..."

curl --digest -u "$USERNAME:$PASSWORD" \
	-F "packfil=@$EAP_FILE;type=application/octet-stream" \
	"http://$CAMERA_HOST/axis-cgi/applications/upload.cgi"

echo '
Done.'
