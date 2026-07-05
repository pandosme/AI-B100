#ifndef RADAR_USECASE_H
#define RADAR_USECASE_H

typedef enum {
	RADAR_MODE_OCCUPANCY = 0,
	RADAR_MODE_COUNTING = 1,
	RADAR_MODE_ALERT = 2
} RadarMode;

typedef struct {
	int total;
	int human;
	int vehicle;
	int unknown;
} RadarCounts;

typedef struct {
	int entering;
	int exiting;
} RadarAreaBalance;

static inline RadarCounts RadarCounts_Make(int total, int human, int vehicle, int unknown) {
	RadarCounts counts;
	counts.total = total;
	counts.human = human;
	counts.vehicle = vehicle;
	counts.unknown = unknown;
	return counts;
}

static inline void RadarCounts_Clamp_Nonnegative(RadarCounts* counts) {
	if (!counts) return;
	if (counts->total < 0) counts->total = 0;
	if (counts->human < 0) counts->human = 0;
	if (counts->vehicle < 0) counts->vehicle = 0;
	if (counts->unknown < 0) counts->unknown = 0;
}

#endif
