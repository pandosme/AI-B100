#include <pthread.h>

#include "counting.h"

static RadarAreaBalance g_balance = {0, 0};
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

void Counting_Reset(void) {
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
