#ifndef SPEED_H
#define SPEED_H

#include <stddef.h>
#include <stdint.h>

#define SPEED_PAYLOAD_SIZE 5
#define SPEED_MIN_QUALIFYING_KMH 10.0
#define SPEED_MIN_DISPLACEMENT 250.0

typedef struct {
	int vehicles;
	int speeding;
	double maximum;
	double average;
	double minimum;
} SpeedSummary;

void Speed_Reset(void);
void Speed_Set_Limit(double limit_kmh);
double Speed_Get_Limit(void);
void Speed_Record_Vehicle(double max_speed_kmh);
SpeedSummary Speed_Get_Summary(void);
void Speed_Reset_Interval(void);
double Speed_Convert(double value_kmh, int unit_mph);
size_t Speed_Build_Payload(uint8_t* buffer, size_t capacity, int unit_mph);

#endif
