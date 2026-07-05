#include <pthread.h>

#include "occupancy.h"

static RadarCounts g_current = {0, 0, 0, 0};
static RadarCounts g_peak = {0, 0, 0, 0};
static RadarAreaBalance g_balance = {0, 0};
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static void Update_Peak(void) {
	if (g_current.total > g_peak.total) {
		g_peak = g_current;
	}
}

void Occupancy_Reset(void) {
	pthread_mutex_lock(&g_mutex);
	g_current = RadarCounts_Make(0, 0, 0, 0);
	g_peak = RadarCounts_Make(0, 0, 0, 0);
	g_balance.entering = 0;
	g_balance.exiting = 0;
	pthread_mutex_unlock(&g_mutex);
}

void Occupancy_Update_Current(RadarCounts current) {
	pthread_mutex_lock(&g_mutex);
	g_current = current;
	Update_Peak();
	pthread_mutex_unlock(&g_mutex);
}

void Occupancy_Process_Transition(int bucket, int in_area, int was_inside, int eligible, int* counted_inside, int* counted_bucket) {
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

RadarCounts Occupancy_Current_Counts(void) {
	pthread_mutex_lock(&g_mutex);
	RadarCounts counts = g_current;
	pthread_mutex_unlock(&g_mutex);
	return counts;
}

RadarCounts Occupancy_Peak_Counts(void) {
	pthread_mutex_lock(&g_mutex);
	RadarCounts counts = g_peak;
	pthread_mutex_unlock(&g_mutex);
	return counts;
}

RadarAreaBalance Occupancy_Area_Balance(void) {
	pthread_mutex_lock(&g_mutex);
	RadarAreaBalance balance = g_balance;
	pthread_mutex_unlock(&g_mutex);
	return balance;
}

void Occupancy_Mark_Published(void) {
	pthread_mutex_lock(&g_mutex);
	g_peak = RadarCounts_Make(0, 0, 0, 0);
	pthread_mutex_unlock(&g_mutex);
}

void Occupancy_Mark_Area_Balance_Published(RadarAreaBalance published) {
	pthread_mutex_lock(&g_mutex);
	g_balance.entering -= published.entering;
	g_balance.exiting -= published.exiting;
	if (g_balance.entering < 0) g_balance.entering = 0;
	if (g_balance.exiting < 0) g_balance.exiting = 0;
	pthread_mutex_unlock(&g_mutex);
}
