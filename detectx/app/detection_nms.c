#include <math.h>
#include <stdlib.h>

#include "detection_nms.h"

static float Detection_IoU(const cJSON* first, const cJSON* second) {
	float x1 = (float)cJSON_GetObjectItem(first, "x")->valuedouble;
	float y1 = (float)cJSON_GetObjectItem(first, "y")->valuedouble;
	float w1 = (float)cJSON_GetObjectItem(first, "w")->valuedouble;
	float h1 = (float)cJSON_GetObjectItem(first, "h")->valuedouble;
	float x2 = (float)cJSON_GetObjectItem(second, "x")->valuedouble;
	float y2 = (float)cJSON_GetObjectItem(second, "y")->valuedouble;
	float w2 = (float)cJSON_GetObjectItem(second, "w")->valuedouble;
	float h2 = (float)cJSON_GetObjectItem(second, "h")->valuedouble;
	float left = fmaxf(x1, x2);
	float top = fmaxf(y1, y2);
	float right = fminf(x1 + w1, x2 + w2);
	float bottom = fminf(y1 + h1, y2 + h2);
	float intersection = fmaxf(0, right - left) * fmaxf(0, bottom - top);
	float union_area = w1 * h1 + w2 * h2 - intersection;
	return union_area > 0 ? intersection / union_area : 0;
}

cJSON* Detection_NMS_Class_Aware(cJSON* candidates, float threshold) {
	int count = cJSON_GetArraySize(candidates);
	if (count < 2) return candidates;
	int* order = malloc((size_t)count * sizeof(int));
	int* suppressed = calloc((size_t)count, sizeof(int));
	if (!order || !suppressed) {
		free(order);
		free(suppressed);
		return candidates;
	}
	for (int index = 0; index < count; index++) order[index] = index;
	for (int index = 0; index < count - 1; index++) {
		int best = index;
		for (int candidate = index + 1; candidate < count; candidate++) {
			cJSON* current = cJSON_GetArrayItem(candidates, order[best]);
			cJSON* alternative = cJSON_GetArrayItem(candidates, order[candidate]);
			if (cJSON_GetObjectItem(alternative, "c")->valuedouble >
				cJSON_GetObjectItem(current, "c")->valuedouble) best = candidate;
		}
		int temporary = order[index];
		order[index] = order[best];
		order[best] = temporary;
	}
	cJSON* result = cJSON_CreateArray();
	for (int position = 0; position < count; position++) {
		int index = order[position];
		if (suppressed[index]) continue;
		cJSON* detection = cJSON_GetArrayItem(candidates, index);
		int class_index = cJSON_GetObjectItem(detection, "classIndex")->valueint;
		cJSON_AddItemToArray(result, cJSON_Duplicate(detection, 1));
		for (int later = position + 1; later < count; later++) {
			int other = order[later];
			if (suppressed[other]) continue;
			cJSON* alternative = cJSON_GetArrayItem(candidates, other);
			if (cJSON_GetObjectItem(alternative, "classIndex")->valueint == class_index &&
				Detection_IoU(detection, alternative) > threshold) suppressed[other] = 1;
		}
	}
	free(order);
	free(suppressed);
	cJSON_Delete(candidates);
	return result;
}