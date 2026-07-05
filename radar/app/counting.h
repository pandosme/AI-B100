#ifndef COUNTING_H
#define COUNTING_H

#include "radar_usecase.h"

void Counting_Reset(void);
void Counting_Process_Transition(int bucket, int in_area, int was_inside, int eligible, int* counted_inside, int* counted_bucket);
RadarAreaBalance Counting_Area_Balance(void);
RadarCounts Counting_Current_Counts(void);
RadarCounts Counting_Peak_Counts(void);
void Counting_Mark_Published(RadarAreaBalance published);

#endif
