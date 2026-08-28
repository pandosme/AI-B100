#include <pthread.h>

#include "alert.h"

static RadarCounts g_current = {0, 0, 0, 0};
static RadarCounts g_active_max = {0, 0, 0, 0};
static int g_active = 0;
static int g_pending = 0;
static int g_detecting = 0;
static time_t g_detect_since = 0;
static time_t g_clear_since = 0;
static time_t g_last_alert_time = 0;
static time_t g_last_heartbeat_time = 0;
static time_t g_last_active_publish_time = 0;
static int g_inactive_pending = 0;
static int g_schedule_enabled = 0;
static int g_schedule_start_minutes = 18 * 60;
static int g_schedule_end_minutes = 6 * 60;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static void Update_Max(RadarCounts current) {
	if (current.total > g_active_max.total) {
		g_active_max = current;
	}
}

static int Schedule_Allows(time_t now) {
	if (!g_schedule_enabled) return 1;
	struct tm local_time;
	localtime_r(&now, &local_time);
	int minutes = local_time.tm_hour * 60 + local_time.tm_min;
	if (g_schedule_start_minutes == g_schedule_end_minutes) return 1;
	if (g_schedule_start_minutes < g_schedule_end_minutes)
		return minutes >= g_schedule_start_minutes && minutes < g_schedule_end_minutes;
	return minutes >= g_schedule_start_minutes || minutes < g_schedule_end_minutes;
}

void Alert_Reset(void) {
	pthread_mutex_lock(&g_mutex);
	g_current = RadarCounts_Make(0, 0, 0, 0);
	g_active_max = RadarCounts_Make(0, 0, 0, 0);
	g_active = 0;
	g_pending = 0;
	g_detecting = 0;
	g_detect_since = 0;
	g_clear_since = 0;
	g_last_heartbeat_time = 0;
	g_last_active_publish_time = 0;
	g_inactive_pending = 0;
	pthread_mutex_unlock(&g_mutex);
}

void Alert_Reset_Timers(void) {
	pthread_mutex_lock(&g_mutex);
	g_last_alert_time = 0;
	g_last_heartbeat_time = 0;
	g_last_active_publish_time = 0;
	if (g_active)
		g_pending = 1;
	else
		g_inactive_pending = 1;
	pthread_mutex_unlock(&g_mutex);
}

void Alert_Set_Schedule(int enabled, int start_minutes, int end_minutes) {
	pthread_mutex_lock(&g_mutex);
	g_schedule_enabled = enabled ? 1 : 0;
	g_schedule_start_minutes = start_minutes >= 0 && start_minutes < 1440 ? start_minutes : 18 * 60;
	g_schedule_end_minutes = end_minutes >= 0 && end_minutes < 1440 ? end_minutes : 6 * 60;
	if (!Schedule_Allows(time(NULL))) {
		g_current = RadarCounts_Make(0, 0, 0, 0);
		g_active_max = RadarCounts_Make(0, 0, 0, 0);
		g_active = 0;
		g_pending = 0;
		g_detecting = 0;
		g_detect_since = 0;
		g_clear_since = 0;
		g_inactive_pending = 1;
	}
	pthread_mutex_unlock(&g_mutex);
}

void Alert_Update_Current(RadarCounts current, time_t now, int transition_seconds) {
	pthread_mutex_lock(&g_mutex);
	if (!Schedule_Allows(now)) {
		g_current = RadarCounts_Make(0, 0, 0, 0);
		g_active_max = RadarCounts_Make(0, 0, 0, 0);
		if (g_active || g_detecting) g_inactive_pending = 1;
		g_active = 0;
		g_pending = 0;
		g_detecting = 0;
		g_detect_since = 0;
		g_clear_since = 0;
		pthread_mutex_unlock(&g_mutex);
		return;
	}
	if (transition_seconds < 2) transition_seconds = 2;
	if (transition_seconds > 20) transition_seconds = 20;
	g_current = current;
	if (current.total > 0) {
		g_clear_since = 0;
		if (!g_detecting) {
			g_detecting = 1;
			g_detect_since = now;
		}
		if (!g_active && g_detect_since > 0 && now - g_detect_since >= transition_seconds) {
			g_active = 1;
			g_pending = 1;
			g_inactive_pending = 0;
			g_active_max = RadarCounts_Make(0, 0, 0, 0);
		}
		if (g_active)
			Update_Max(current);
	} else {
		g_detecting = 0;
		g_detect_since = 0;
		if (g_active) {
			if (g_clear_since == 0)
				g_clear_since = now;
			if (now - g_clear_since >= transition_seconds) {
				g_active = 0;
				g_pending = 0;
				g_inactive_pending = 1;
				g_active_max = RadarCounts_Make(0, 0, 0, 0);
			}
		}
	}
	pthread_mutex_unlock(&g_mutex);
}

RadarCounts Alert_Current_Counts(void) {
	pthread_mutex_lock(&g_mutex);
	RadarCounts counts = g_current;
	pthread_mutex_unlock(&g_mutex);
	return counts;
}

RadarCounts Alert_Publish_Counts(int heartbeat) {
	pthread_mutex_lock(&g_mutex);
	RadarCounts counts = heartbeat ? RadarCounts_Make(0, 0, 0, 0) : g_active_max;
	pthread_mutex_unlock(&g_mutex);
	return counts;
}

int Alert_Pending(void) {
	pthread_mutex_lock(&g_mutex);
	int pending = g_pending;
	pthread_mutex_unlock(&g_mutex);
	return pending;
}

int Alert_Active(void) {
	pthread_mutex_lock(&g_mutex);
	int active = g_active;
	pthread_mutex_unlock(&g_mutex);
	return active;
}

time_t Alert_Last_Time(void) {
	pthread_mutex_lock(&g_mutex);
	time_t last_time = g_last_alert_time;
	pthread_mutex_unlock(&g_mutex);
	return last_time;
}

int Alert_Should_Publish(time_t now, int heartbeat_minutes, int active_interval_seconds, int* heartbeat) {
	pthread_mutex_lock(&g_mutex);
	if (heartbeat_minutes < 5) heartbeat_minutes = 5;
	if (heartbeat_minutes > 60) heartbeat_minutes = 60;
	if (active_interval_seconds < 60) active_interval_seconds = 60;
	if (active_interval_seconds > 300) active_interval_seconds = 300;
	int should_publish = 0;
	int is_heartbeat = 0;
	if (g_active) {
		if (g_pending && g_active_max.total > 0) {
			should_publish = 1;
		} else if (g_active_max.total > 0 && g_last_active_publish_time > 0 && now - g_last_active_publish_time >= active_interval_seconds) {
			should_publish = 1;
		} else if (g_active_max.total > 0 && g_last_active_publish_time == 0) {
			should_publish = 1;
		}
	} else {
		is_heartbeat = 1;
		if (g_inactive_pending || g_last_heartbeat_time == 0 || now - g_last_heartbeat_time >= heartbeat_minutes * 60)
			should_publish = 1;
	}
	if (heartbeat) *heartbeat = is_heartbeat;
	pthread_mutex_unlock(&g_mutex);
	return should_publish;
}

time_t Alert_Next_Publish_Time(time_t now, int heartbeat_minutes, int active_interval_seconds) {
	pthread_mutex_lock(&g_mutex);
	if (heartbeat_minutes < 5) heartbeat_minutes = 5;
	if (heartbeat_minutes > 60) heartbeat_minutes = 60;
	if (active_interval_seconds < 60) active_interval_seconds = 60;
	if (active_interval_seconds > 300) active_interval_seconds = 300;
	time_t next_time = now;
	if (g_active) {
		if (g_pending && g_active_max.total > 0)
			next_time = now;
		else if (g_last_active_publish_time > 0)
			next_time = g_last_active_publish_time + active_interval_seconds;
	} else if (g_inactive_pending) {
		next_time = now;
	} else if (g_last_heartbeat_time > 0) {
		next_time = g_last_heartbeat_time + heartbeat_minutes * 60;
	}
	pthread_mutex_unlock(&g_mutex);
	return next_time;
}

void Alert_Mark_Published(time_t now, int heartbeat) {
	pthread_mutex_lock(&g_mutex);
	if (heartbeat) {
		g_last_heartbeat_time = now;
		g_inactive_pending = 0;
	} else {
		g_pending = 0;
		g_last_alert_time = now;
		g_last_active_publish_time = now;
		g_active_max = RadarCounts_Make(0, 0, 0, 0);
	}
	pthread_mutex_unlock(&g_mutex);
}
