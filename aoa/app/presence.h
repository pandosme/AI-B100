#ifndef PRESENCE_H
#define PRESENCE_H

#include <stddef.h>
#include <time.h>

#include "cJSON.h"

void Presence_Load_Config(cJSON* config, int fixed_port);
void Presence_Initialize_State(void);
void Presence_Process_AOA_Event(cJSON* event);
void Presence_Update(time_t now);

int Presence_Enabled(void);
int Presence_Port(void);
int Presence_Should_Publish(time_t now);
time_t Presence_Next_Publish_Time(time_t now);
void Presence_Mark_Published(time_t now, unsigned long generation);
void Presence_Reset_Timers(void);
void Presence_Reset_All(void);

void Presence_Update_ACAP_Status(void);
void Presence_Add_Status_JSON(cJSON* presence_array);
void Presence_Add_Publish_JSON(cJSON* publish, time_t now);
void Presence_Build_Decoder_Definitions(cJSON* definitions, int port);
int Presence_Build_Payload(unsigned char** out_buffer, size_t* out_size, int* out_area_count, unsigned long* out_generation);

#endif