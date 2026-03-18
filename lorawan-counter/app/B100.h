/*
 * B100.h - AI-B100 LoRaWAN Bridge HTTP Client Library
 * Copyright (c) 2026 Fred Juhlin
 * MIT License
 */

#ifndef _B100_H_
#define _B100_H_

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// Connection status
#define B100_NOT_CONNECTED  0
#define B100_CONNECTED      1
#define B100_ERROR         -1

// LoRaWAN Status Codes (from AI-B100 documentation)
#define B100_STATUS_OK                  0
#define B100_STATUS_RESTARTED           1
#define B100_STATUS_NO_PAYLOAD          2
#define B100_STATUS_PAYLOAD_TOO_LONG    3
#define B100_STATUS_JOIN_FAILED         4
#define B100_STATUS_TRYING_TO_JOIN      5
#define B100_STATUS_UNKNOWN_ERROR       6
#define B100_STATUS_JOINED              7
#define B100_STATUS_PAYLOAD_RECEIVED    8
#define B100_STATUS_PAYLOAD_SENT        9
#define B100_STATUS_SENT_CONFIRMED      10
#define B100_STATUS_NOT_CONFIRMED       11
#define B100_STATUS_LOST_CONNECTION     14
#define B100_STATUS_INVALID_PORT        15
#define B100_STATUS_UPLINK_FAILED       16
#define B100_STATUS_PARAMETER_ERROR     17
#define B100_STATUS_NOT_JOINED          18
#define B100_STATUS_PARAMETER_UPDATED   19

// AI-B100 Device Status Structure
typedef struct {
    // Connection
    int connected;              // B100_CONNECTED or B100_NOT_CONNECTED
    
    // LoRaWAN Status
    int joined;                 // 1 if joined to network, 0 otherwise
    int statusCode;             // Status code (see B100_STATUS_* defines)
    char statusText[128];       // Human-readable status
    
    // Device Identity
    char devEUI[17];            // Device EUI (16 hex chars)
    char joinEUI[17];           // Join EUI / App EUI (16 hex chars)
    unsigned int devAddr;       // Device Address (when joined)
    
    // LoRaWAN Parameters
    int dataRateUp;             // Upload data rate (0-5)
    int dataRateDown;           // Download data rate (0-5)
    int maxPayload;             // Maximum payload size in bytes
    int adr;                    // Adaptive Data Rate enabled (1) or not (0)
    
    // Frame Counters
    unsigned int fcntUp;        // Uplink frame counter
    unsigned int fcntDown;      // Downlink frame counter
    
    // Signal Quality
    float rssi;                 // RSSI in dBm
    float snr;                  // SNR in dB
    
    // Other
    int confirmed;              // Last message was confirmed
    float tempC;                // Device temperature in Celsius
    unsigned long timestamp;    // Last update timestamp

    // Device Information (parsed once from GET / on first connect)
    char hardwareVersion[64];   // e.g. "AI-B100 Version <= 1.2"
    char softwareVersion[64];   // e.g. "1.8.1"
} B100_Status;

// Downlink message structure
typedef struct {
    int port;                   // LoRaWAN port number
    char payload[256];          // Message payload (HEX string or ASCII)
    char payload_type[16];      // Payload encoding: "HEX" or "ASCII"
    int length;                 // Payload length in bytes
    float rssi;                 // RSSI of reception
    float snr;                  // SNR of reception
    int fcntDown;               // Downlink frame counter
    int confirming;             // 1 if device is awaiting uplink confirmation
} B100_Downlink;

// Callback function types
typedef void (*B100_Downlink_Callback)(B100_Downlink* downlink);
typedef void (*B100_Status_Callback)(B100_Status* status);

// Initialization and Configuration
int B100_Init(const char* ip, int port, int timeout_seconds);
void B100_Cleanup(void);
int B100_Set_IP(const char* ip);
int B100_Set_Port(int port);
int B100_Set_Timeout(int timeout_seconds);

// Connection Management
int B100_Test_Connection(void);
int B100_Is_Connected(void);

// Status Retrieval
B100_Status* B100_Get_Status(void);
int B100_Update_Status(void);
const char* B100_Status_Text(int statusCode);

// LoRaWAN Control
int B100_Join(int drJoin, int adr, int drUp);
int B100_Join_Auto(void);                        // Use default settings
int B100_Restart(void);
int B100_Set_DataRate(int dr);
int B100_Set_ADR(int enabled);

// Messaging
int B100_Send(const char* payload, int port, int confirmed);
int B100_Send_JSON(cJSON* json, int port, int confirmed);
B100_Downlink* B100_Receive(void);              // Poll for downlink
void B100_Free_Downlink(B100_Downlink* downlink);

// Link Test
int B100_Link_Test(void);

// Configuration Reading (from AI-B100 HTML pages)
int B100_Read_LoRaWAN_Config(char* devEUI, char* joinEUI, char* appKey);

// Callbacks
void B100_Set_Downlink_Callback(B100_Downlink_Callback callback);
void B100_Set_Status_Callback(B100_Status_Callback callback);

// Utility Functions
const char* B100_Get_Last_Error(void);
void B100_Clear_Error(void);

#ifdef __cplusplus
}
#endif

#endif /* _B100_H_ */
