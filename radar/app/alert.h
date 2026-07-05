#ifndef ALERT_H
#define ALERT_H

#include <time.h>

#include "radar_usecase.h"

void Alert_Reset(void);
void Alert_Reset_Timers(void);
void Alert_Update_Current(RadarCounts current, time_t now, int transition_seconds);
RadarCounts Alert_Current_Counts(void);
RadarCounts Alert_Publish_Counts(int heartbeat);
int Alert_Pending(void);
int Alert_Active(void);
time_t Alert_Last_Time(void);
int Alert_Should_Publish(time_t now, int heartbeat_minutes, int active_interval_seconds, int* heartbeat);
time_t Alert_Next_Publish_Time(time_t now, int heartbeat_minutes, int active_interval_seconds);
void Alert_Mark_Published(time_t now, int heartbeat);

#endif
