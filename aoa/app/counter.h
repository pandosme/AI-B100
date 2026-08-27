#ifndef COUNTER_H
#define COUNTER_H

#include <stddef.h>
#include <time.h>
#include <stdint.h>

#include "cJSON.h"

typedef struct {
	int human;
	int car;
	int bike;
	int bus;
	int truck;
	int other;
} ClassSelection;

void Counting_Load_Config(cJSON* config, int fixed_port);
int Counting_Enabled(void);
int Counting_Port(void);
int Counting_Interval_Minutes(void);
time_t Counting_Next_Publish_Time(void);
void Counting_Reset_Schedule(time_t now);
void Counting_Mark_Published(time_t now);
void Counting_Set_Interval_Minutes(int minutes);

void Counting_Process_AOA_Event(cJSON* event);
void Counting_Load_From_File(void);
int Counting_Save_To_File(void);
void Counting_Delete_By_Scenario(const char* scenario);
void Counting_Sync_With_AOA_List(cJSON* scenario_array);
void Counting_Reset_All(void);
int Counting_Set_Values_From_JSON(cJSON* body);
int Counting_Count(void);
int Counting_Has_Scenario(const char* scenario);
int Counting_References_Ready(void);

void Counting_Add_Counters_JSON(cJSON* counters_array);
void Counting_Add_Publish_JSON(cJSON* publish, time_t now);
void Counting_Build_Decoder_Definitions(cJSON* definitions, int port);
int Counting_Build_Payload(unsigned char** out_buffer, size_t* out_size, int* out_counter_count, int* out_class_count);

int Class_Count(ClassSelection classes);
cJSON* Class_Array_JSON(ClassSelection classes);
uint16_t Wrap_U16_Int(int value);
void Append_U16(unsigned char* buffer, size_t* offset, uint16_t value);

#endif
