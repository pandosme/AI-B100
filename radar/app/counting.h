#ifndef COUNTING_H
#define COUNTING_H

#include <stddef.h>
#include <stdint.h>

#include "radar_usecase.h"

#define COUNTING_MAX_SCENES 10
#define COUNTING_NAME_SIZE 32
#define COUNTING_CLASS_HUMAN 0x01
#define COUNTING_CLASS_VEHICLE 0x02

typedef enum {
	COUNTING_DIRECTION_LEFT_TO_RIGHT = 1,
	COUNTING_DIRECTION_RIGHT_TO_LEFT = 2
} CountingDirection;

typedef struct {
	uint16_t id;
	char name[COUNTING_NAME_SIZE];
	int enabled;
	CountingDirection direction;
	uint8_t class_mask;
	int x1;
	int y1;
	int x2;
	int y2;
} CountingSceneConfig;

typedef struct {
	CountingSceneConfig config;
	uint64_t human;
	uint64_t vehicle;
} CountingSceneSnapshot;

int Counting_Configure(const CountingSceneConfig* scenes, size_t scene_count);
int Counting_Validate_Config(const CountingSceneConfig* scenes, size_t scene_count);
size_t Counting_Get_Scenes(CountingSceneSnapshot* scenes, size_t capacity);
int Counting_Set_Totals(uint16_t id, uint64_t human, uint64_t vehicle);
int Counting_Process_Completed_Track(int active, int class_bucket,
	int confidence, int minimum_confidence, double age_seconds,
	double minimum_dwell_seconds, double birth_x, double birth_y,
	double death_x, double death_y);
size_t Counting_Build_Cumulative_Payload(uint8_t* buffer, size_t capacity);
void Counting_Reset_All(void);

void Counting_Reset(void);
void Counting_Process_Transition(int bucket, int in_area, int was_inside, int eligible, int* counted_inside, int* counted_bucket);
RadarAreaBalance Counting_Area_Balance(void);
RadarCounts Counting_Current_Counts(void);
RadarCounts Counting_Peak_Counts(void);
void Counting_Mark_Published(RadarAreaBalance published);

#endif
