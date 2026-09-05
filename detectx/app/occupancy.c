#include <pthread.h>
#include <string.h>

#include "occupancy.h"

typedef struct {
	size_t label_count;
	char labels[OCCUPANCY_MAX_LABELS][OCCUPANCY_LABEL_MAX_LENGTH + 1];
	uint32_t current[OCCUPANCY_MAX_LABELS];
	uint32_t maxima[OCCUPANCY_MAX_LABELS];
	uint32_t pending[OCCUPANCY_MAX_LABELS];
	uint32_t last_published[OCCUPANCY_MAX_LABELS];
	unsigned long next_token;
	unsigned long pending_token;
	int publish_in_flight;
} OccupancyState;

static OccupancyState g_state = {.next_token = 1};
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static int Labels_Are_Valid(const char* const* labels, size_t label_count) {
	if (!labels || label_count == 0 || label_count > OCCUPANCY_MAX_LABELS) return 0;

	for (size_t index = 0; index < label_count; index++) {
		if (!labels[index]) return 0;
		size_t length = strlen(labels[index]);
		if (length == 0 || length > OCCUPANCY_LABEL_MAX_LENGTH) return 0;
		for (size_t previous = 0; previous < index; previous++) {
			if (strcmp(labels[index], labels[previous]) == 0) return 0;
		}
	}
	return 1;
}

int Occupancy_Configure(const char* const* labels, size_t label_count) {
	if (!Labels_Are_Valid(labels, label_count)) return 0;

	pthread_mutex_lock(&g_mutex);
	unsigned long next_token = g_state.next_token;
	memset(&g_state, 0, sizeof(g_state));
	g_state.next_token = next_token ? next_token : 1;
	g_state.label_count = label_count;
	for (size_t index = 0; index < label_count; index++) {
		strncpy(g_state.labels[index], labels[index], OCCUPANCY_LABEL_MAX_LENGTH);
		g_state.labels[index][OCCUPANCY_LABEL_MAX_LENGTH] = '\0';
	}
	pthread_mutex_unlock(&g_mutex);
	return 1;
}

void Occupancy_Reset(void) {
	pthread_mutex_lock(&g_mutex);
	memset(g_state.current, 0, sizeof(g_state.current));
	memset(g_state.maxima, 0, sizeof(g_state.maxima));
	memset(g_state.pending, 0, sizeof(g_state.pending));
	memset(g_state.last_published, 0, sizeof(g_state.last_published));
	g_state.pending_token = 0;
	g_state.publish_in_flight = 0;
	pthread_mutex_unlock(&g_mutex);
}

void Occupancy_Record_Frame(const char* const* detection_labels, size_t detection_count) {
	uint32_t counts[OCCUPANCY_MAX_LABELS] = {0};

	pthread_mutex_lock(&g_mutex);
	if (detection_labels) {
		for (size_t detection = 0; detection < detection_count; detection++) {
			if (!detection_labels[detection]) continue;
			for (size_t label = 0; label < g_state.label_count; label++) {
				if (strcmp(detection_labels[detection], g_state.labels[label]) == 0) {
					counts[label]++;
					break;
				}
			}
		}
	}

	for (size_t label = 0; label < g_state.label_count; label++) {
		g_state.current[label] = counts[label];
		if (counts[label] > g_state.maxima[label]) g_state.maxima[label] = counts[label];
	}
	pthread_mutex_unlock(&g_mutex);
}

void Occupancy_Get_Status(OccupancyStatus* status) {
	if (!status) return;

	pthread_mutex_lock(&g_mutex);
	memset(status, 0, sizeof(*status));
	status->label_count = g_state.label_count;
	status->publish_in_flight = g_state.publish_in_flight;
	for (size_t label = 0; label < g_state.label_count; label++) {
		strncpy(status->labels[label], g_state.labels[label], OCCUPANCY_LABEL_MAX_LENGTH);
		status->current[label] = g_state.current[label];
		status->maxima[label] = g_state.maxima[label];
		status->last_published[label] = g_state.last_published[label];
	}
	pthread_mutex_unlock(&g_mutex);
}

int Occupancy_Begin_Publish(OccupancyPublishSnapshot* snapshot) {
	if (!snapshot) return 0;

	pthread_mutex_lock(&g_mutex);
	if (g_state.label_count == 0 || g_state.publish_in_flight) {
		pthread_mutex_unlock(&g_mutex);
		return 0;
	}

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->token = g_state.next_token++;
	if (g_state.next_token == 0) g_state.next_token = 1;
	snapshot->label_count = g_state.label_count;
	g_state.pending_token = snapshot->token;
	g_state.publish_in_flight = 1;

	for (size_t label = 0; label < g_state.label_count; label++) {
		strncpy(snapshot->labels[label], g_state.labels[label], OCCUPANCY_LABEL_MAX_LENGTH);
		snapshot->maxima[label] = g_state.maxima[label];
		g_state.pending[label] = g_state.maxima[label];
		g_state.maxima[label] = 0;
	}
	pthread_mutex_unlock(&g_mutex);
	return 1;
}

int Occupancy_Finish_Publish(unsigned long token, int success) {
	pthread_mutex_lock(&g_mutex);
	if (!g_state.publish_in_flight || token == 0 || token != g_state.pending_token) {
		pthread_mutex_unlock(&g_mutex);
		return 0;
	}

	for (size_t label = 0; label < g_state.label_count; label++) {
		if (success) {
			g_state.last_published[label] = g_state.pending[label];
		} else if (g_state.pending[label] > g_state.maxima[label]) {
			g_state.maxima[label] = g_state.pending[label];
		}
		g_state.pending[label] = 0;
	}
	g_state.pending_token = 0;
	g_state.publish_in_flight = 0;
	pthread_mutex_unlock(&g_mutex);
	return 1;
}

size_t Occupancy_Build_Payload(const OccupancyPublishSnapshot* snapshot,
	uint8_t* payload, size_t capacity) {
	if (!snapshot || !payload || snapshot->label_count == 0 ||
		snapshot->label_count > OCCUPANCY_MAX_LABELS || capacity < snapshot->label_count) {
		return 0;
	}

	for (size_t label = 0; label < snapshot->label_count; label++) {
		payload[label] = snapshot->maxima[label] > UINT8_MAX ? UINT8_MAX :
			(uint8_t)snapshot->maxima[label];
	}
	return snapshot->label_count;
}