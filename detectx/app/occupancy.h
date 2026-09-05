#ifndef DETECTX_OCCUPANCY_H
#define DETECTX_OCCUPANCY_H

#include <stddef.h>
#include <stdint.h>

#define OCCUPANCY_MAX_LABELS 5
#define OCCUPANCY_LABEL_MAX_LENGTH 60

typedef struct {
	unsigned long token;
	size_t label_count;
	char labels[OCCUPANCY_MAX_LABELS][OCCUPANCY_LABEL_MAX_LENGTH + 1];
	uint32_t maxima[OCCUPANCY_MAX_LABELS];
} OccupancyPublishSnapshot;

typedef struct {
	size_t label_count;
	char labels[OCCUPANCY_MAX_LABELS][OCCUPANCY_LABEL_MAX_LENGTH + 1];
	uint32_t current[OCCUPANCY_MAX_LABELS];
	uint32_t maxima[OCCUPANCY_MAX_LABELS];
	uint32_t last_published[OCCUPANCY_MAX_LABELS];
	int publish_in_flight;
} OccupancyStatus;

int Occupancy_Configure(const char* const* labels, size_t label_count);
void Occupancy_Reset(void);
void Occupancy_Record_Frame(const char* const* detection_labels, size_t detection_count);
void Occupancy_Get_Status(OccupancyStatus* status);
int Occupancy_Begin_Publish(OccupancyPublishSnapshot* snapshot);
int Occupancy_Finish_Publish(unsigned long token, int success);
size_t Occupancy_Build_Payload(const OccupancyPublishSnapshot* snapshot,
	uint8_t* payload, size_t capacity);

#endif