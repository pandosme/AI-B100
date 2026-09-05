#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include "ACAP.h"
#include "Model.h"
#include "detection_nms.h"
#include "labelparse.h"
#include "larod.h"

#define LOG(fmt, args...) do { syslog(LOG_INFO, fmt, ##args); printf(fmt, ##args); } while (0)
#define LOG_WARN(fmt, args...) do { syslog(LOG_WARNING, fmt, ##args); printf(fmt, ##args); } while (0)

#define MODEL_LABEL_MAX 60
#define MODEL_MAX_LABELS 255

typedef struct {
	unsigned width;
	unsigned height;
	unsigned channels;
	unsigned boxes;
	unsigned classes;
	char input_type[16];
	char output_type[16];
	float output_scale;
	int output_zero_point;
	char model_sha256[65];
	char labels_sha256[65];
	char description[128];
	int supports_artpec8;
	int supports_artpec9;
} ModelMetadata;

static larodConnection* g_connection;
static larodModel* g_model;
static larodModel* g_preprocess_model;
static larodTensor** g_inputs;
static larodTensor** g_outputs;
static larodTensor** g_preprocess_inputs;
static larodTensor** g_preprocess_outputs;
static larodJobRequest* g_inference_request;
static larodJobRequest* g_preprocess_request;
static larodMap* g_preprocess_map;
static size_t g_input_count;
static size_t g_output_count;
static size_t g_preprocess_input_count;
static size_t g_preprocess_output_count;
static int g_model_fd = -1;
static int g_nv12_fd = -1;
static int g_rgb_fd = -1;
static int g_output_fd = -1;
static void* g_nv12 = MAP_FAILED;
static void* g_rgb = MAP_FAILED;
static void* g_output = MAP_FAILED;
static size_t g_nv12_size;
static size_t g_rgb_size;
static size_t g_output_size;
static char** g_labels;
static char* g_label_buffer;
static size_t g_label_count;
static ModelMetadata g_metadata;
static cJSON* g_config;
static pthread_mutex_t g_settings_mutex = PTHREAD_MUTEX_INITIALIZER;
static float g_objectness = 0.25f;
static float g_confidence = 0.30f;
static float g_nms = 0.45f;
static int g_output_float;
static int g_output_signed;

static const char* const MODEL_PATHS[] = {
	"localdata/model.tflite", "localdata/model.previous.tflite", "model/model.tflite"
};
static const char* const LABEL_PATHS[] = {
	"localdata/labels.txt", "localdata/labels.previous.txt", "model/labels.txt"
};
static const char* const METADATA_PATHS[] = {
	"localdata/metadata.json", "localdata/metadata.previous.json", "model/metadata.json"
};
static void Set_Error(char* error, size_t error_size, const char* message) {
	if (error && error_size) snprintf(error, error_size, "%s", message ? message : "Unknown error");
}

static int JSON_Array_UInt(const cJSON* array, int index, unsigned* value) {
	const cJSON* item = cJSON_GetArrayItem(array, index);
	if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > UINT_MAX) return 0;
	*value = (unsigned)item->valuedouble;
	return 1;
}

static int Parse_Metadata(const cJSON* root, ModelMetadata* metadata, char* error, size_t error_size) {
	memset(metadata, 0, sizeof(*metadata));
	const cJSON* format = cJSON_GetObjectItemCaseSensitive(root, "format");
	const cJSON* input = cJSON_GetObjectItemCaseSensitive(root, "input");
	const cJSON* output = cJSON_GetObjectItemCaseSensitive(root, "output");
	const cJSON* input_shape = input ? cJSON_GetObjectItemCaseSensitive(input, "shape") : NULL;
	const cJSON* output_shape = output ? cJSON_GetObjectItemCaseSensitive(output, "shape") : NULL;
	const cJSON* input_type = input ? cJSON_GetObjectItemCaseSensitive(input, "dtype") : NULL;
	const cJSON* output_type = output ? cJSON_GetObjectItemCaseSensitive(output, "dtype") : NULL;
	const cJSON* scale = output ? cJSON_GetObjectItemCaseSensitive(output, "scale") : NULL;
	const cJSON* zero_point = output ? cJSON_GetObjectItemCaseSensitive(output, "zeroPoint") : NULL;
	const cJSON* model_hash = cJSON_GetObjectItemCaseSensitive(root, "modelSha256");
	const cJSON* labels_hash = cJSON_GetObjectItemCaseSensitive(root, "labelsSha256");
	const cJSON* description = cJSON_GetObjectItemCaseSensitive(root, "description");
	const cJSON* targets = cJSON_GetObjectItemCaseSensitive(root, "targets");

	if (!cJSON_IsString(format) || strcmp(format->valuestring, "yolov5-1-n-5+c") != 0 ||
		!cJSON_IsArray(input_shape) || cJSON_GetArraySize(input_shape) != 4 ||
		!cJSON_IsArray(output_shape) || cJSON_GetArraySize(output_shape) != 3 ||
		!cJSON_IsString(input_type) || strcmp(input_type->valuestring, "uint8") != 0 ||
		!cJSON_IsString(output_type) || !cJSON_IsString(model_hash) || !cJSON_IsString(labels_hash) ||
		!cJSON_IsArray(targets) || cJSON_GetArraySize(targets) == 0 ||
		strlen(model_hash->valuestring) != 64 || strlen(labels_hash->valuestring) != 64) {
		Set_Error(error, error_size, "Invalid or unsupported metadata contract");
		return 0;
	}
	unsigned batch = 0, output_batch = 0, stride = 0;
	if (!JSON_Array_UInt(input_shape, 0, &batch) || !JSON_Array_UInt(input_shape, 1, &metadata->height) ||
		!JSON_Array_UInt(input_shape, 2, &metadata->width) || !JSON_Array_UInt(input_shape, 3, &metadata->channels) ||
		!JSON_Array_UInt(output_shape, 0, &output_batch) || !JSON_Array_UInt(output_shape, 1, &metadata->boxes) ||
		!JSON_Array_UInt(output_shape, 2, &stride) || batch != 1 || output_batch != 1 ||
		metadata->width == 0 || metadata->height == 0 || metadata->width % 8 || metadata->height % 8 ||
		metadata->channels != 3 || metadata->boxes == 0 || stride <= 5 || stride > MODEL_MAX_LABELS + 5) {
		Set_Error(error, error_size, "Tensor shapes do not match uint8 NHWC YOLOv5 [1,N,5+C]");
		return 0;
	}
	metadata->classes = stride - 5;
	if (strcmp(output_type->valuestring, "float32") == 0) {
		metadata->output_scale = 1.0f;
		metadata->output_zero_point = 0;
	} else if ((strcmp(output_type->valuestring, "uint8") == 0 || strcmp(output_type->valuestring, "int8") == 0) &&
		cJSON_IsNumber(scale) && scale->valuedouble > 0 && cJSON_IsNumber(zero_point)) {
		metadata->output_scale = (float)scale->valuedouble;
		metadata->output_zero_point = zero_point->valueint;
	} else {
		Set_Error(error, error_size, "Output must be float32 or per-tensor int8/uint8 with scale and zeroPoint");
		return 0;
	}
	snprintf(metadata->input_type, sizeof(metadata->input_type), "%s", input_type->valuestring);
	snprintf(metadata->output_type, sizeof(metadata->output_type), "%s", output_type->valuestring);
	snprintf(metadata->model_sha256, sizeof(metadata->model_sha256), "%s", model_hash->valuestring);
	snprintf(metadata->labels_sha256, sizeof(metadata->labels_sha256), "%s", labels_hash->valuestring);
	if (cJSON_IsString(description)) snprintf(metadata->description, sizeof(metadata->description), "%s", description->valuestring);
	const cJSON* target = NULL;
	cJSON_ArrayForEach(target, targets) {
		if (!cJSON_IsString(target)) continue;
		if (strcmp(target->valuestring, "artpec8") == 0) metadata->supports_artpec8 = 1;
		else if (strcmp(target->valuestring, "artpec9") == 0) metadata->supports_artpec9 = 1;
	}
	if (!metadata->supports_artpec8 && !metadata->supports_artpec9) {
		Set_Error(error, error_size, "Metadata must declare artpec8 and/or artpec9 support");
		return 0;
	}
	return 1;
}

static char* File_SHA256(const char* path) {
	FILE* file = ACAP_FILE_Open(path, "rb");
	if (!file) return NULL;
	GChecksum* checksum = g_checksum_new(G_CHECKSUM_SHA256);
	unsigned char buffer[8192];
	size_t count;
	while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) g_checksum_update(checksum, buffer, count);
	fclose(file);
	char* result = g_strdup(g_checksum_get_string(checksum));
	g_checksum_free(checksum);
	return result;
}

static int Validate_Labels(const char* path, unsigned expected, char* error, size_t error_size) {
	char** labels = NULL;
	char* buffer = NULL;
	size_t count = 0;
	if (!labels_parse_file(path, &labels, &buffer, &count) || count != expected) {
		labels_free(labels, buffer);
		Set_Error(error, error_size, "Labels must contain exactly one line for each model class");
		return 0;
	}
	for (size_t index = 0; index < count; index++) {
		size_t length = strlen(labels[index]);
		if (length == 0 || length > MODEL_LABEL_MAX) {
			labels_free(labels, buffer);
			Set_Error(error, error_size, "Labels must be non-empty and at most 60 characters");
			return 0;
		}
		for (size_t previous = 0; previous < index; previous++) {
			if (strcmp(labels[index], labels[previous]) == 0) {
				labels_free(labels, buffer);
				Set_Error(error, error_size, "Labels must be unique");
				return 0;
			}
		}
	}
	labels_free(labels, buffer);
	return 1;
}

static int Validate_Files(const char* model_path, const char* labels_path, const char* metadata_path,
	ModelMetadata* metadata, char* error, size_t error_size) {
	cJSON* metadata_json = ACAP_FILE_Read(metadata_path);
	if (!metadata_json || !Parse_Metadata(metadata_json, metadata, error, error_size)) {
		cJSON_Delete(metadata_json);
		return 0;
	}
	cJSON_Delete(metadata_json);
	char* model_hash = File_SHA256(model_path);
	char* labels_hash = File_SHA256(labels_path);
	int valid = model_hash && labels_hash && g_ascii_strcasecmp(model_hash, metadata->model_sha256) == 0 &&
		g_ascii_strcasecmp(labels_hash, metadata->labels_sha256) == 0;
	g_free(model_hash);
	g_free(labels_hash);
	if (!valid) {
		Set_Error(error, error_size, "Model or labels SHA-256 does not match metadata");
		return 0;
	}
	return Validate_Labels(labels_path, metadata->classes, error, error_size);
}

static int Create_Mapped_File(size_t size, int* fd, void** address) {
	char path[] = "/tmp/detectx-XXXXXX";
	*fd = mkstemp(path);
	if (*fd < 0 || ftruncate(*fd, (off_t)size) < 0) return 0;
	unlink(path);
	*address = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
	return *address != MAP_FAILED;
}

static void Release_Runtime(void) {
	larodError* error = NULL;
	if (g_inference_request) larodDestroyJobRequest(&g_inference_request);
	if (g_preprocess_request) larodDestroyJobRequest(&g_preprocess_request);
	if (g_inputs) larodDestroyTensors(g_connection, &g_inputs, g_input_count, &error);
	if (g_outputs) larodDestroyTensors(g_connection, &g_outputs, g_output_count, &error);
	if (g_preprocess_inputs) larodDestroyTensors(g_connection, &g_preprocess_inputs, g_preprocess_input_count, &error);
	if (g_preprocess_outputs) larodDestroyTensors(g_connection, &g_preprocess_outputs, g_preprocess_output_count, &error);
	if (g_preprocess_map) larodDestroyMap(&g_preprocess_map);
	if (g_preprocess_model) larodDestroyModel(&g_preprocess_model);
	if (g_model) larodDestroyModel(&g_model);
	if (g_connection) larodDisconnect(&g_connection, NULL);
	larodClearError(&error);
	if (g_nv12 != MAP_FAILED) munmap(g_nv12, g_nv12_size);
	if (g_rgb != MAP_FAILED) munmap(g_rgb, g_rgb_size);
	if (g_output != MAP_FAILED) munmap(g_output, g_output_size);
	if (g_nv12_fd >= 0) close(g_nv12_fd);
	if (g_rgb_fd >= 0) close(g_rgb_fd);
	if (g_output_fd >= 0) close(g_output_fd);
	if (g_model_fd >= 0) close(g_model_fd);
	g_nv12 = g_rgb = g_output = MAP_FAILED;
	g_nv12_fd = g_rgb_fd = g_output_fd = g_model_fd = -1;
	g_inputs = g_outputs = g_preprocess_inputs = g_preprocess_outputs = NULL;
	g_inference_request = g_preprocess_request = NULL;
	g_model = g_preprocess_model = NULL;
	g_preprocess_map = NULL;
	labels_free(g_labels, g_label_buffer);
	g_labels = NULL;
	g_label_buffer = NULL;
	g_label_count = 0;
}

static int Data_Type_Matches(larodTensorDataType type, const char* name) {
	return (type == LAROD_TENSOR_DATA_TYPE_UINT8 && strcmp(name, "uint8") == 0) ||
		(type == LAROD_TENSOR_DATA_TYPE_INT8 && strcmp(name, "int8") == 0) ||
		(type == LAROD_TENSOR_DATA_TYPE_FLOAT32 && strcmp(name, "float32") == 0);
}

static int Metadata_Supports_Current_Platform(const ModelMetadata* metadata) {
	const char* platform = ACAP_DEVICE_Prop("platform");
	int artpec9 = platform && strstr(platform, "Artpec-9");
	return artpec9 ? metadata->supports_artpec9 : metadata->supports_artpec8;
}

static int Setup_Path(size_t path_index,
					  const cJSON* detection_settings,
					  char* error_text,
					  size_t error_size) {
	larodError* error = NULL;
	ModelMetadata metadata;
	if (!Validate_Files(MODEL_PATHS[path_index], LABEL_PATHS[path_index], METADATA_PATHS[path_index],
		&metadata, error_text, error_size)) return 0;
	if (!larodConnect(&g_connection, &error)) goto larod_error;
	const char* platform = ACAP_DEVICE_Prop("platform");
	int artpec9 = platform && strstr(platform, "Artpec-9");
	if (!Metadata_Supports_Current_Platform(&metadata)) {
		Set_Error(error_text, error_size, artpec9 ?
			"Model bundle does not declare ARTPEC-9 support" :
			"Model bundle does not declare ARTPEC-8 support");
		return 0;
	}
	const char* device_name = artpec9 ? "a9-dlpu-tflite" : "axis-a8-dlpu-tflite";
	const larodDevice* device = larodGetDevice(g_connection, device_name, 0, &error);
	if (!device) goto larod_error;
	char model_path[PATH_MAX];
	if (snprintf(model_path, sizeof(model_path), "%s%s", ACAP_FILE_AppPath(), MODEL_PATHS[path_index]) >= (int)sizeof(model_path)) {
		Set_Error(error_text, error_size, "Model path is too long");
		return 0;
	}
	g_model_fd = open(model_path, O_RDONLY);
	if (g_model_fd < 0) {
		Set_Error(error_text, error_size, strerror(errno));
		return 0;
	}
	g_model = larodLoadModel(g_connection, g_model_fd, device, LAROD_ACCESS_PRIVATE, "object_detection", NULL, &error);
	if (!g_model) goto larod_error;
	g_inputs = larodCreateModelInputs(g_model, &g_input_count, &error);
	g_outputs = larodCreateModelOutputs(g_model, &g_output_count, &error);
	if (!g_inputs || !g_outputs || g_input_count != 1 || g_output_count != 1) {
		Set_Error(error_text, error_size, "Model must expose exactly one input and one output tensor");
		return 0;
	}
	const larodTensorDims* input_dims = larodGetTensorDims(g_inputs[0], &error);
	const larodTensorDims* output_dims = larodGetTensorDims(g_outputs[0], &error);
	larodTensorDataType input_type = larodGetTensorDataType(g_inputs[0], &error);
	larodTensorDataType output_type = larodGetTensorDataType(g_outputs[0], &error);
	if (!input_dims || !output_dims || input_dims->dims[0] != 1 || input_dims->dims[1] != metadata.height ||
		input_dims->dims[2] != metadata.width || input_dims->dims[3] != metadata.channels ||
		output_dims->dims[0] != 1 || output_dims->dims[1] != metadata.boxes ||
		output_dims->dims[2] != metadata.classes + 5 || !Data_Type_Matches(input_type, metadata.input_type) ||
		!Data_Type_Matches(output_type, metadata.output_type)) {
		Set_Error(error_text, error_size, "larod tensor metadata does not match uploaded metadata.json");
		return 0;
	}
	if (!labels_parse_file(LABEL_PATHS[path_index], &g_labels, &g_label_buffer, &g_label_count)) {
		Set_Error(error_text, error_size, "Unable to load labels");
		return 0;
	}
	const cJSON* capture_mode_item = detection_settings ?
		cJSON_GetObjectItemCaseSensitive(detection_settings, "captureMode") : NULL;
	const char* capture_mode = cJSON_IsString(capture_mode_item) ? capture_mode_item->valuestring : "balanced";
	if (strcmp(capture_mode, "crop") != 0 && strcmp(capture_mode, "letterbox") != 0)
		capture_mode = "balanced";
	unsigned video_width = metadata.width;
	unsigned video_height = metadata.height;
	const char* image_fit = "crop";
	if (strcmp(capture_mode, "balanced") == 0) {
		video_width = ((metadata.height * 4 / 3) + 7) & ~7u;
		video_height = metadata.height;
	} else if (strcmp(capture_mode, "letterbox") == 0) {
		image_fit = "scale";
	}

	g_preprocess_map = larodCreateMap(&error);
	if (!g_preprocess_map ||
		!larodMapSetStr(g_preprocess_map, "image.input.format", "nv12", &error) ||
		!larodMapSetIntArr2(g_preprocess_map, "image.input.size", video_width, video_height, &error) ||
		!larodMapSetStr(g_preprocess_map, "image.output.format", "rgb-interleaved", &error) ||
		!larodMapSetIntArr2(g_preprocess_map, "image.output.size", metadata.width, metadata.height, &error)) goto larod_error;
	const larodDevice* preprocessor = larodGetDevice(g_connection, "cpu-proc", 0, &error);
	if (!preprocessor) goto larod_error;
	g_preprocess_model = larodLoadModel(g_connection, -1, preprocessor, LAROD_ACCESS_PRIVATE, "", g_preprocess_map, &error);
	if (!g_preprocess_model) goto larod_error;
	g_preprocess_inputs = larodCreateModelInputs(g_preprocess_model, &g_preprocess_input_count, &error);
	g_preprocess_outputs = larodCreateModelOutputs(g_preprocess_model, &g_preprocess_output_count, &error);
	if (!g_preprocess_inputs || !g_preprocess_outputs) goto larod_error;
	const larodTensorPitches* nv12_pitches = larodGetTensorPitches(g_preprocess_inputs[0], &error);
	const larodTensorPitches* rgb_pitches = larodGetTensorPitches(g_preprocess_outputs[0], &error);
	const larodTensorPitches* output_pitches = larodGetTensorPitches(g_outputs[0], &error);
	if (!nv12_pitches || !rgb_pitches || !output_pitches) goto larod_error;
	g_nv12_size = nv12_pitches->pitches[0];
	g_rgb_size = rgb_pitches->pitches[0];
	g_output_size = output_pitches->pitches[0];
	if (!Create_Mapped_File(g_nv12_size, &g_nv12_fd, &g_nv12) ||
		!Create_Mapped_File(g_rgb_size, &g_rgb_fd, &g_rgb) ||
		!Create_Mapped_File(g_output_size, &g_output_fd, &g_output)) {
		Set_Error(error_text, error_size, "Unable to allocate inference buffers");
		return 0;
	}
	if (!larodSetTensorFd(g_preprocess_inputs[0], g_nv12_fd, &error) ||
		!larodSetTensorFd(g_preprocess_outputs[0], g_rgb_fd, &error) ||
		!larodSetTensorFd(g_inputs[0], g_rgb_fd, &error) ||
		!larodSetTensorFd(g_outputs[0], g_output_fd, &error)) goto larod_error;
	g_preprocess_request = larodCreateJobRequest(g_preprocess_model, g_preprocess_inputs, g_preprocess_input_count,
		g_preprocess_outputs, g_preprocess_output_count, NULL, &error);
	g_inference_request = larodCreateJobRequest(g_model, g_inputs, g_input_count, g_outputs, g_output_count, NULL, &error);
	if (!g_preprocess_request || !g_inference_request) goto larod_error;

	g_metadata = metadata;
	g_output_float = strcmp(metadata.output_type, "float32") == 0;
	g_output_signed = strcmp(metadata.output_type, "int8") == 0;
	cJSON_Delete(g_config);
	g_config = cJSON_CreateObject();
	cJSON_AddNumberToObject(g_config, "modelWidth", metadata.width);
	cJSON_AddNumberToObject(g_config, "modelHeight", metadata.height);
	cJSON_AddNumberToObject(g_config, "videoWidth", video_width);
	cJSON_AddNumberToObject(g_config, "videoHeight", video_height);
	cJSON_AddStringToObject(g_config, "captureMode", capture_mode);
	cJSON_AddStringToObject(g_config, "imageFit", image_fit);
	cJSON_AddNumberToObject(g_config, "boxes", metadata.boxes);
	cJSON_AddNumberToObject(g_config, "classes", metadata.classes);
	cJSON_AddStringToObject(g_config, "inputType", metadata.input_type);
	cJSON_AddStringToObject(g_config, "outputType", metadata.output_type);
	cJSON_AddNumberToObject(g_config, "outputScale", metadata.output_scale);
	cJSON_AddNumberToObject(g_config, "outputZeroPoint", metadata.output_zero_point);
	cJSON_AddStringToObject(g_config, "modelSha256", metadata.model_sha256);
	cJSON_AddStringToObject(g_config, "description", metadata.description);
	cJSON_AddStringToObject(g_config, "source", "bundled");
	cJSON* labels_json = cJSON_AddArrayToObject(g_config, "labels");
	for (size_t index = 0; index < g_label_count; index++) cJSON_AddItemToArray(labels_json, cJSON_CreateString(g_labels[index]));
	ACAP_STATUS_SetString("model", "source", "bundled");
	ACAP_STATUS_SetString("model", "status", "Running");
	ACAP_STATUS_SetBool("model", "state", 1);
	return 1;

larod_error:
	Set_Error(error_text, error_size, error && error->msg ? error->msg : "larod setup failed");
	larodClearError(&error);
	return 0;
}

cJSON* Model_Setup(const cJSON* detection_settings) {
	char error[256] = "Bundled model is unavailable";
	if (Setup_Path(2, detection_settings, error, sizeof(error))) {
		ACAP_STATUS_SetNull("model", "activationError");
		ACAP_Set_Config("model", g_config);
		return g_config;
	}
	LOG_WARN("Bundled model rejected: %s\n", error);
	Release_Runtime();
	ACAP_STATUS_SetString("model", "status", "Stopped");
	ACAP_STATUS_SetString("model", "error", error);
	ACAP_STATUS_SetBool("model", "state", 0);
	return NULL;
}

void Model_Apply_Detection_Settings(cJSON* settings) {
	if (!settings) return;
	pthread_mutex_lock(&g_settings_mutex);
	cJSON* item = cJSON_GetObjectItem(settings, "objectness");
	if (cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= 1) g_objectness = (float)item->valuedouble;
	item = cJSON_GetObjectItem(settings, "confidence");
	if (cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= 1) g_confidence = (float)item->valuedouble;
	item = cJSON_GetObjectItem(settings, "nms");
	if (cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= 1) g_nms = (float)item->valuedouble;
	pthread_mutex_unlock(&g_settings_mutex);
}

cJSON* Model_Inference(VdoBuffer* image) {
	cJSON* candidates = cJSON_CreateArray();
	if (!image || !g_model || !candidates) return candidates;
	memcpy(g_nv12, vdo_buffer_get_data(image), g_nv12_size);
	larodError* error = NULL;
	gint64 started = g_get_monotonic_time();
	if (!larodRunJob(g_connection, g_preprocess_request, &error) ||
		lseek(g_output_fd, 0, SEEK_SET) < 0 || !larodRunJob(g_connection, g_inference_request, &error)) {
		ACAP_STATUS_SetString("model", "error", error && error->msg ? error->msg : "Inference failed");
		larodClearError(&error);
		return candidates;
	}
	ACAP_STATUS_SetNumber("model", "inferenceMs", (double)(g_get_monotonic_time() - started) / 1000.0);
	float objectness_threshold, confidence_threshold;
	pthread_mutex_lock(&g_settings_mutex);
	objectness_threshold = g_objectness;
	confidence_threshold = g_confidence;
	pthread_mutex_unlock(&g_settings_mutex);
	uint8_t* output_u8 = g_output;
	int8_t* output_i8 = g_output;
	float* output_f32 = g_output;
#define VALUE(position) (g_output_float ? output_f32[(position)] : \
	((g_output_signed ? (float)output_i8[(position)] : (float)output_u8[(position)]) - g_metadata.output_zero_point) * g_metadata.output_scale)
	for (unsigned box_index = 0; box_index < g_metadata.boxes; box_index++) {
		unsigned offset = box_index * (g_metadata.classes + 5);
		float objectness = VALUE(offset + 4);
		if (objectness < objectness_threshold) continue;
		int class_index = -1;
		float confidence = 0;
		for (unsigned class_id = 0; class_id < g_metadata.classes; class_id++) {
			float candidate = VALUE(offset + 5 + class_id) * objectness;
			if (candidate > confidence) { confidence = candidate; class_index = (int)class_id; }
		}
		if (class_index < 0 || confidence < confidence_threshold) continue;
		float center_x = VALUE(offset), center_y = VALUE(offset + 1);
		float width = VALUE(offset + 2), height = VALUE(offset + 3);
		cJSON* detection = cJSON_CreateObject();
		cJSON_AddStringToObject(detection, "label", labels_get(g_labels, g_label_count, class_index));
		cJSON_AddNumberToObject(detection, "classIndex", class_index);
		cJSON_AddNumberToObject(detection, "c", confidence);
		cJSON_AddNumberToObject(detection, "x", center_x - width / 2);
		cJSON_AddNumberToObject(detection, "y", center_y - height / 2);
		cJSON_AddNumberToObject(detection, "w", width);
		cJSON_AddNumberToObject(detection, "h", height);
		cJSON_AddItemToArray(candidates, detection);
	}
#undef VALUE
	ACAP_STATUS_SetNull("model", "error");
	return candidates;
}

cJSON* Model_Apply_NMS(cJSON* candidates) {
	float threshold;
	pthread_mutex_lock(&g_settings_mutex);
	threshold = g_nms;
	pthread_mutex_unlock(&g_settings_mutex);
	return Detection_NMS_Class_Aware(candidates, threshold);
}

const cJSON* Model_Get_Config(void) {
	return g_config;
}

int Model_Label_Is_Valid(const char* label) {
	if (!label) return 0;
	for (size_t index = 0; index < g_label_count; index++) if (strcmp(label, g_labels[index]) == 0) return 1;
	return 0;
}

void Model_Cleanup(void) {
	Release_Runtime();
	g_config = NULL;
}