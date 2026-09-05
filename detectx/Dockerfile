ARG ARCH=aarch64
ARG VERSION=12.11.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

RUN apt-get update && apt-get install -y --no-install-recommends python3 python3-pip python3-venv \
	&& rm -rf /var/lib/apt/lists/* \
	&& python3 -m venv /opt/model-validator \
	&& /opt/model-validator/bin/pip install --no-cache-dir tensorflow-cpu==2.16.2

WORKDIR /opt/app
COPY ./.stage .
COPY ./tools/validate_model.py /opt/validate_model.py
ARG CHIP=

RUN /opt/model-validator/bin/python /opt/validate_model.py \
	model/model.tflite model/labels.txt model/metadata.json \
	--description "Bundled COCO YOLOv5 model" --targets artpec8 artpec9

RUN . /opt/axis/acapsdk/environment-setup* && acap-build . \
	-a 'settings/settings.json' \
	-a 'settings/events.json' \
	-a 'settings/subscriptions.json' \
	-a 'model/model.tflite' \
	-a 'model/labels.txt' \
	-a 'model/metadata.json' \
	-a 'localdata'