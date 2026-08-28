#include <math.h>
#include <pthread.h>
#include <string.h>

#include "counting.h"

typedef struct {
	CountingSceneConfig config;
	uint64_t human;
	uint64_t vehicle;
} CountingScene;

static CountingScene g_scenes[COUNTING_MAX_SCENES];
static size_t g_scene_count = 0;
static RadarAreaBalance g_balance = {0, 0};
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static int Counting_Config_Valid(const CountingSceneConfig* scene) {
	if (!scene || scene->id == 0) return 0;
	size_t name_length = strnlen(scene->name, COUNTING_NAME_SIZE);
	if (name_length == 0 || name_length >= COUNTING_NAME_SIZE) return 0;
	if (scene->enabled != 0 && scene->enabled != 1) return 0;
	if (scene->direction != COUNTING_DIRECTION_LEFT_TO_RIGHT &&
		scene->direction != COUNTING_DIRECTION_RIGHT_TO_LEFT) return 0;
	if ((scene->class_mask & ~(COUNTING_CLASS_HUMAN | COUNTING_CLASS_VEHICLE)) != 0)
		return 0;
	if (scene->enabled && scene->class_mask == 0) return 0;
	if (scene->x1 < 0 || scene->x1 > 1000 || scene->y1 < 0 || scene->y1 > 1000 ||
		scene->x2 < 0 || scene->x2 > 1000 || scene->y2 < 0 || scene->y2 > 1000)
		return 0;
	return scene->x1 != scene->x2 || scene->y1 != scene->y2;
}

int Counting_Validate_Config(const CountingSceneConfig* scenes, size_t scene_count) {
	if (scene_count > COUNTING_MAX_SCENES || (scene_count > 0 && !scenes)) return 0;
	for (size_t index = 0; index < scene_count; index++) {
		if (!Counting_Config_Valid(&scenes[index])) return 0;
		for (size_t other = 0; other < index; other++) {
			if (scenes[index].id == scenes[other].id ||
				strcmp(scenes[index].name, scenes[other].name) == 0) return 0;
		}
	}
	return 1;
}

int Counting_Configure(const CountingSceneConfig* scenes, size_t scene_count) {
	if (!Counting_Validate_Config(scenes, scene_count)) return 0;

	pthread_mutex_lock(&g_mutex);
	CountingScene configured[COUNTING_MAX_SCENES] = {0};
	for (size_t index = 0; index < scene_count; index++) {
		configured[index].config = scenes[index];
		for (size_t existing = 0; existing < g_scene_count; existing++) {
			if (g_scenes[existing].config.id != scenes[index].id) continue;
			configured[index].human = g_scenes[existing].human;
			configured[index].vehicle = g_scenes[existing].vehicle;
			break;
		}
	}
	memcpy(g_scenes, configured, sizeof(g_scenes));
	g_scene_count = scene_count;
	pthread_mutex_unlock(&g_mutex);
	return 1;
}

size_t Counting_Get_Scenes(CountingSceneSnapshot* scenes, size_t capacity) {
	pthread_mutex_lock(&g_mutex);
	size_t count = g_scene_count;
	if (scenes) {
		size_t copied = count < capacity ? count : capacity;
		for (size_t index = 0; index < copied; index++) {
			scenes[index].config = g_scenes[index].config;
			scenes[index].human = g_scenes[index].human;
			scenes[index].vehicle = g_scenes[index].vehicle;
		}
	}
	pthread_mutex_unlock(&g_mutex);
	return count;
}

int Counting_Set_Totals(uint16_t id, uint64_t human, uint64_t vehicle) {
	pthread_mutex_lock(&g_mutex);
	for (size_t index = 0; index < g_scene_count; index++) {
		if (g_scenes[index].config.id != id) continue;
		g_scenes[index].human = human;
		g_scenes[index].vehicle = vehicle;
		pthread_mutex_unlock(&g_mutex);
		return 1;
	}
	pthread_mutex_unlock(&g_mutex);
	return 0;
}

static double Counting_Cross(double ax, double ay, double bx, double by,
	double px, double py) {
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static int Counting_Properly_Crosses(const CountingSceneConfig* scene,
	double birth_x, double birth_y, double death_x, double death_y) {
	const double epsilon = 0.000001;
	double birth_side = Counting_Cross(scene->x1, scene->y1, scene->x2, scene->y2,
		birth_x, birth_y);
	double death_side = Counting_Cross(scene->x1, scene->y1, scene->x2, scene->y2,
		death_x, death_y);
	if (fabs(birth_side) <= epsilon || fabs(death_side) <= epsilon ||
		(birth_side > 0) == (death_side > 0)) return 0;

	double first_endpoint_side = Counting_Cross(birth_x, birth_y, death_x, death_y,
		scene->x1, scene->y1);
	double second_endpoint_side = Counting_Cross(birth_x, birth_y, death_x, death_y,
		scene->x2, scene->y2);
	if (fabs(first_endpoint_side) <= epsilon || fabs(second_endpoint_side) <= epsilon ||
		(first_endpoint_side > 0) == (second_endpoint_side > 0)) return 0;

	return scene->direction == COUNTING_DIRECTION_LEFT_TO_RIGHT
		? birth_side > 0 && death_side < 0
		: birth_side < 0 && death_side > 0;
}

int Counting_Process_Completed_Track(int active, int class_bucket,
	int confidence, int minimum_confidence, double age_seconds,
	double minimum_dwell_seconds, double birth_x, double birth_y,
	double death_x, double death_y) {
	if (active || confidence < minimum_confidence || age_seconds < minimum_dwell_seconds)
		return 0;
	uint8_t class_mask = class_bucket == 1 ? COUNTING_CLASS_HUMAN
		: class_bucket == 2 ? COUNTING_CLASS_VEHICLE : 0;
	if (!class_mask || (birth_x == death_x && birth_y == death_y)) return 0;

	int counted = 0;
	pthread_mutex_lock(&g_mutex);
	for (size_t index = 0; index < g_scene_count; index++) {
		CountingScene* scene = &g_scenes[index];
		if (!scene->config.enabled || !(scene->config.class_mask & class_mask) ||
			!Counting_Properly_Crosses(&scene->config, birth_x, birth_y, death_x, death_y))
			continue;
		if (class_mask == COUNTING_CLASS_HUMAN) scene->human++;
		else scene->vehicle++;
		counted++;
	}
	pthread_mutex_unlock(&g_mutex);
	return counted;
}

size_t Counting_Build_Cumulative_Payload(uint8_t* buffer, size_t capacity) {
	pthread_mutex_lock(&g_mutex);
	size_t required = 0;
	for (size_t index = 0; index < g_scene_count; index++) {
		if (!g_scenes[index].config.enabled) continue;
		if (g_scenes[index].config.class_mask & COUNTING_CLASS_HUMAN) required += 2;
		if (g_scenes[index].config.class_mask & COUNTING_CLASS_VEHICLE) required += 2;
	}
	if (!buffer || capacity < required) {
		pthread_mutex_unlock(&g_mutex);
		return required;
	}
	size_t offset = 0;
	for (size_t index = 0; index < g_scene_count; index++) {
		CountingScene* scene = &g_scenes[index];
		if (!scene->config.enabled) continue;
		if (scene->config.class_mask & COUNTING_CLASS_HUMAN) {
			uint16_t value = (uint16_t)scene->human;
			buffer[offset++] = (uint8_t)(value >> 8);
			buffer[offset++] = (uint8_t)value;
		}
		if (scene->config.class_mask & COUNTING_CLASS_VEHICLE) {
			uint16_t value = (uint16_t)scene->vehicle;
			buffer[offset++] = (uint8_t)(value >> 8);
			buffer[offset++] = (uint8_t)value;
		}
	}
	pthread_mutex_unlock(&g_mutex);
	return required;
}

void Counting_Reset_All(void) {
	pthread_mutex_lock(&g_mutex);
	for (size_t index = 0; index < g_scene_count; index++) {
		g_scenes[index].human = 0;
		g_scenes[index].vehicle = 0;
	}
	pthread_mutex_unlock(&g_mutex);
}

void Counting_Reset(void) {
	Counting_Reset_All();
	pthread_mutex_lock(&g_mutex);
	g_balance.entering = 0;
	g_balance.exiting = 0;
	pthread_mutex_unlock(&g_mutex);
}

void Counting_Process_Transition(int bucket, int in_area, int was_inside, int eligible, int* counted_inside, int* counted_bucket) {
	(void)bucket;
	if (!counted_inside || !counted_bucket) return;
	pthread_mutex_lock(&g_mutex);
	if (eligible && in_area && !*counted_inside) {
		g_balance.entering++;
		*counted_inside = 1;
		*counted_bucket = bucket;
	} else if (was_inside && !in_area && *counted_inside) {
		g_balance.exiting++;
		*counted_inside = 0;
		*counted_bucket = 0;
	}
	pthread_mutex_unlock(&g_mutex);
}

RadarAreaBalance Counting_Area_Balance(void) {
	pthread_mutex_lock(&g_mutex);
	RadarAreaBalance balance = g_balance;
	pthread_mutex_unlock(&g_mutex);
	return balance;
}

RadarCounts Counting_Current_Counts(void) {
	pthread_mutex_lock(&g_mutex);
	RadarCounts counts = RadarCounts_Make(g_balance.entering + g_balance.exiting, g_balance.entering, g_balance.exiting, 0);
	pthread_mutex_unlock(&g_mutex);
	return counts;
}

RadarCounts Counting_Peak_Counts(void) {
	return Counting_Current_Counts();
}

void Counting_Mark_Published(RadarAreaBalance published) {
	pthread_mutex_lock(&g_mutex);
	g_balance.entering -= published.entering;
	g_balance.exiting -= published.exiting;
	if (g_balance.entering < 0) g_balance.entering = 0;
	if (g_balance.exiting < 0) g_balance.exiting = 0;
	pthread_mutex_unlock(&g_mutex);
}
