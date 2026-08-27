#ifndef OCCUPANCY_H
#define OCCUPANCY_H

#include <stddef.h>
#include <time.h>
#include <stdint.h>

#include "counter.h"
#include "cJSON.h"

int Occupancy_Is_Scenario_Type(const char* scenario_type);
int Occupancy_Is_Valid_Value(const char* value);
void Occupancy_Load_Config(cJSON* config, int fixed_port);
int Occupancy_Enabled(void);
int Occupancy_Port(void);
int Occupancy_Interval_Minutes(void);
const char* Occupancy_Value(void);
time_t Occupancy_Next_Publish_Time(void);
void Occupancy_Reset_Schedule(time_t now);
void Occupancy_Mark_Published(time_t now);
void Occupancy_Set_Interval_Minutes(int minutes);

void Occupancy_Process_AOA_Event(cJSON* event);
void Occupancy_Update_ACAP_Status(void);
void Occupancy_Reset_All(void);
int Occupancy_Count(void);

void Occupancy_Add_Status_JSON(cJSON* occupancy_array);
void Occupancy_Add_Publish_JSON(cJSON* publish, time_t now);
void Occupancy_Build_Decoder_Definitions(cJSON* definitions, int port);
int Occupancy_Build_Payload(unsigned char** out_buffer, size_t* out_size, int* out_sample_count, int* out_class_count);

uint8_t Occupancy_Value_Type_Code(const char* value_type);
uint8_t Occupancy_Encode_U8(double value);
void Append_U8(unsigned char* buffer, size_t* offset, uint8_t value);

#endif
