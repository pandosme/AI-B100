# AI-B100 DetectX

AI-B100 DetectX combines local YOLOv5 inference with the shared AI-B100 LoRaWAN bridge client. It is an alternative to the AOA and Radar packages and intentionally uses ACAP `appName` `aib100` so the camera URL, settings store, and bridge callback paths remain stable.

## Support And Payload

- Friendly name: **AI-B100 DetectX**
- Version: `2.0.0`
- Architecture: `aarch64` only, for ARTPEC-8 and ARTPEC-9
- Uplink: fixed port `2`
- Selection: 1-5 unique labels from the active model, always stored in model order
- Payload: one `uint8` interval-maximum occupancy value per selected label
- Saturation: values above 255 encode as 255
- Empty intervals: publish zero bytes for every selected label

Occupancy is the maximum number of post-filter detections simultaneously present in any one inference frame during the interval. It is not arrival counting or tracking. Publishing resets the displayed interval maxima; frames arriving during the send start the next interval, while a failed bridge send merges the detached peak back so it can be retried.

Download a fresh decoder from `/local/aib100/translator` whenever the selected label set changes. The data decoder and both OTA translators embed `transmission.occupancy.selectedLabels` as an ordered `[{ byte, label }]` mapping. Decoded port 2 JSON includes that mapping under `configuration.occupancy.labels` and resolves each payload byte into the label-named `occupancy` object.

## Inference And Model Contract

The lean inference baseline was imported from `/home/fred/acap/DetectX` commit `fc63e05d6e0303d67f3cfce5234ae5bfefdedf7d`. The model runtime, capture-mode integration, and filtering are specific to this app. MQTT, crops, SD capture, certificates, and DetectX label events are excluded.

The model, labels, and generated metadata are fixed when the ACAP is built. Operators cannot upload or replace a model through the application. A build is accepted only when its bundled model has:

- one uint8 NHWC input `[1,H,W,3]`, with `H` and `W` divisible by 8;
- one YOLOv5 output `[1,N,5+C]`;
- output type float32, int8, or uint8;
- valid per-tensor scale and zero point for quantized outputs;
- exactly `C` unique non-empty labels, each at most 60 characters.

The Docker build runs the validator and generates the metadata sidecar before packaging. Developers can run the same validation manually on a machine with TensorFlow installed:

```bash
python3 tools/validate_model.py app/model/model.tflite app/model/labels.txt \
  app/model/metadata.json --description "Site model" --targets artpec8 artpec9
```

For ARTPEC-8, prepare the TFLite model with per-tensor quantization. ARTPEC-9 supports per-channel model weights, but the single output tensor must still expose the supported per-tensor output quantization used by the decoder.

## UI And Management

- **Publish:** scheduled publishing, live next-publish countdown, current per-label interval maxima, fixed port, manual publish, recent uplinks, and a decoder viewer with copy/download actions
- **Occupancy:** live video with detection boxes, draggable area-of-interest polygon, capture mode, 1-5 label selection, five descriptive confidence levels, and three descriptive overlap-handling levels
- **LoRA Bridge:** shared bridge configuration, status, join, restart, and link-check controls
- **About:** device, model, and payload information

Capture mode is stored as `detection.captureMode` in settings schema version `3`. Center-cropping fills the square model input without distortion but clips the image edges and uses a 1:1 preview. Balanced follows DetectX by capturing a 4:3 frame, `856x640` for a `640x640` model, and scaling it to 1:1 for inference while showing the preview at its unstretched 4:3 aspect. Letterbox uses a 1:1 preview with the full 16:9 image centered between black upper and lower padding. Changing capture mode restarts the ACAP so VDO and larod are rebuilt with matching dimensions.

Information icons on the Occupancy page explain capture mode, area, confidence, overlap, and payload-label controls on hover or keyboard focus. GPS and LoRA Downlink callbacks remain supported by the backend but are not shown in the DetectX navigation.

Callbacks remain `/local/aib100/b100_status`, `/local/aib100/b100_receive`, and `/local/aib100/b100_gps`. Bridge HTTP calls are serialized by the shared `B100.c` client with a 30-second timeout.

Management downlinks supported by this MVP are actions on port 100, interval/data-rate/ADR on port 110, and information requests on port 120 with replies on 121/122. The hidden LoRA Downlink page exposes generated JavaScript from `/local/aib100/ota_encoder` and `/local/aib100/ota_decoder`; both use service registries that can be extended when DetectX gains more OTA services. There is no remote model or label-selection OTA command.

## Build And Test

```bash
cc -Wall -Wextra -Werror -pthread -I app tests/test_occupancy.c app/occupancy.c \
  -o /tmp/test_detectx_occupancy
/tmp/test_detectx_occupancy
node tests/test_decoder.js
node tests/test_ota_javascript.js
cc -Wall -Wextra -Werror -I app -I ../common/app \
  tests/test_detection_nms.c app/detection_nms.c ../common/app/cJSON.c -lm \
  -o /tmp/test_detectx_nms
/tmp/test_detectx_nms
./build.sh
```

The build produces `AI-B100_DetectX_2_0_0_aarch64.eap`. Docker validates the bundled model and generates `model/metadata.json` before packaging. No armv7hf package is built.