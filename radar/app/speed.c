#include <math.h>
#include <pthread.h>
#include <string.h>

#include "speed.h"

#define SPEED_KMH_TO_MPH 0.621371

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static double g_limit_kmh = 50.0;
static int g_vehicles = 0;
static int g_speeding = 0;
static double g_maximum = 0.0;
static double g_minimum = 0.0;
static double g_sum = 0.0;

void Speed_Reset(void) {
	pthread_mutex_lock(&g_mutex);
	g_vehicles = 0;
	g_speeding = 0;
	g_maximum = 0.0;
	g_minimum = 0.0;
	g_sum = 0.0;
	pthread_mutex_unlock(&g_mutex);
}

void Speed_Set_Limit(double limit_kmh) {
	if (!(limit_kmh > 0.0)) return;
	pthread_mutex_lock(&g_mutex);
	g_limit_kmh = limit_kmh;
	pthread_mutex_unlock(&g_mutex);
}

double Speed_Get_Limit(void) {
	pthread_mutex_lock(&g_mutex);
	double limit = g_limit_kmh;
	pthread_mutex_unlock(&g_mutex);
	return limit;
}

void Speed_Record_Vehicle(double max_speed_kmh) {
	if (!(max_speed_kmh > 0.0)) return;
	pthread_mutex_lock(&g_mutex);
	if (g_vehicles == 0 || max_speed_kmh < g_minimum) g_minimum = max_speed_kmh;
	if (max_speed_kmh > g_maximum) g_maximum = max_speed_kmh;
	if (max_speed_kmh > g_limit_kmh) g_speeding++;
	g_sum += max_speed_kmh;
	g_vehicles++;
	pthread_mutex_unlock(&g_mutex);
}

SpeedSummary Speed_Get_Summary(void) {
	SpeedSummary summary = {0, 0, 0.0, 0.0, 0.0};
	pthread_mutex_lock(&g_mutex);
	summary.vehicles = g_vehicles;
	summary.speeding = g_speeding;
	if (g_vehicles > 0) {
		summary.maximum = g_maximum;
		summary.minimum = g_minimum;
		summary.average = g_sum / (double)g_vehicles;
	}
	pthread_mutex_unlock(&g_mutex);
	return summary;
}

void Speed_Reset_Interval(void) {
	pthread_mutex_lock(&g_mutex);
	g_vehicles = 0;
	g_speeding = 0;
	g_maximum = 0.0;
	g_minimum = 0.0;
	g_sum = 0.0;
	pthread_mutex_unlock(&g_mutex);
}

double Speed_Convert(double value_kmh, int unit_mph) {
	return unit_mph ? value_kmh * SPEED_KMH_TO_MPH : value_kmh;
}

static uint8_t Speed_Encode(double value) {
	if (!(value > 0.0)) return 0;
	double rounded = floor(value + 0.5);
	if (rounded > 255.0) return 255;
	return (uint8_t)rounded;
}

size_t Speed_Build_Payload(uint8_t* buffer, size_t capacity, int unit_mph) {
	if (!buffer || capacity < SPEED_PAYLOAD_SIZE) return 0;
	SpeedSummary summary = Speed_Get_Summary();
	buffer[0] = Speed_Encode((double)summary.vehicles);
	buffer[1] = Speed_Encode((double)summary.speeding);
	buffer[2] = Speed_Encode(Speed_Convert(summary.maximum, unit_mph));
	buffer[3] = Speed_Encode(Speed_Convert(summary.average, unit_mph));
	buffer[4] = Speed_Encode(Speed_Convert(summary.minimum, unit_mph));
	return SPEED_PAYLOAD_SIZE;
}
