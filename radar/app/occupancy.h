#ifndef OCCUPANCY_H
#define OCCUPANCY_H

#include "radar_usecase.h"

void Occupancy_Reset(void);
void Occupancy_Update_Current(RadarCounts current);
void Occupancy_Process_Transition(int bucket, int in_area, int was_inside, int eligible, int* counted_inside, int* counted_bucket);
RadarCounts Occupancy_Current_Counts(void);
RadarCounts Occupancy_Peak_Counts(void);
RadarAreaBalance Occupancy_Area_Balance(void);
void Occupancy_Mark_Published(void);
void Occupancy_Mark_Area_Balance_Published(RadarAreaBalance published);

#endif
