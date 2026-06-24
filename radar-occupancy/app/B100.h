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

// LoRaWAN Status Codes (from AI-B100 HTTP API documentation)
#define B100_STATUS_OK                  0
#define B100_STATUS_RESTARTED           1
#define B100_STATUS_NO_PAYLOAD          2
#define B100_STATUS_PAYLOAD_TOO_LONG    3
#define B100_STATUS_JOIN_FAILED         4
#define B100_STATUS_AUTOJOIN_ENABLED    5
#define B100_STATUS_UNKNOWN_ERROR       6
#define B100_STATUS_JOINED              7
#define B100_STATUS_PAYLOAD_RECEIVED    8
#define B100_STATUS_PAYLOAD_SENT        9
#define B100_STATUS_SENT_CONFIRMED      10
#define B100_STATUS_NOT_CONFIRMED       11
#define B100_STATUS_MQTT_HEARTBEAT      12
#define B100_STATUS_DUTY_CYCLE          13
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
    
    // Device Identity (from /info)
    char devEUI[17];            // Device EUI (16 hex chars)
    char joinEUI[17];           // Join EUI / App EUI (16 hex chars)
    unsigned int devAddr;       // Device Address (when joined)
    char devAddrStr[16];        // Device Address as hex string
    
    // LoRaWAN Parameters
    int dataRate;               // Current uplink data rate (0-5)
    int maxPayload;             // Maximum payload size in bytes
    int adr;                    // Adaptive Data Rate enabled (1) or not (0)
    
    // Frame Counters
    unsigned int fcntUp;        // Uplink frame counter
    unsigned int fcntDown;      // Downlink frame counter
    
    // Signal Quality
    float rssi;                 // RSSI in dBm
    float snr;                  // SNR in dB
    
    // Timing
    int confirmed;              // Last message was confirmed
    unsigned long tUnix;        // Network UTC time (0 if not synced)
    unsigned long nextUploadMs; // ms until next upload allowed (duty cycle)
    unsigned long receiveTUnix; // Wall-clock time of last receive callback
    
    // Device Info (from /info endpoint)
    char hardware[32];          // e.g. "AI-B100"
    char hardwareVersion[32];   // e.g. "1.3"
    char firmwareVersion[32];   // e.g. "1.9.0"
    char powerSource[16];       // "poe", "usb", "external", "unknown"
    float tempC;                // Device temperature in Celsius
    unsigned int restartCounter;// Number of hardware restarts
    int httpApiEnabled;         // HTTP API enabled
    int mqttEnabled;            // MQTT enabled
    int dhcpEnabled;            // DHCP enabled
    char ipAddr[20];            // B100 IP address
    unsigned long timestamp;    // Last update timestamp (local)
    int tamper;                 // Tamper status: 0=normal, 1=tampered
    int gpsStatus;              // GPS fix: 0=no antenna, 1=no fix, 2=2D, 3=3D

    // Linkcheck results (from receive callback)
    int margin;                 // Link margin from linkcheck
    int gwCount;                // Gateway count from linkcheck
} B100_Status;

// GPS position structure (from /gps endpoint and GPS callback)
typedef struct {
    int    gps_status;  // 0=no antenna, 1=no fix, 2=2D fix, 3=3D fix
    char   ns[2];       // "N" or "S"
    double lat;         // decimal degrees (positive = North)
    char   ew[2];       // "E" or "W"
    double lon;         // decimal degrees (positive = East)
    double alt;         // meters above mean sea level
    int    nosv;        // number of satellites used
    float  pdop;
    float  hdop;
    float  vdop;
    char   utc[16];     // "HHMMSS.ss"
    char   date[8];     // "DDMMYY"
    float  sog;         // speed over ground in knots
    float  cog;         // course over ground in degrees
} B100_GPS;

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
typedef void (*B100_GPS_Callback)(B100_GPS* gps);

// Initialization and Configuration
int B100_Init(const char* ip, int port, int timeout_seconds);
void B100_Cleanup(void);
int B100_Set_IP(const char* ip);
int B100_Set_Port(int port);
int B100_Set_Timeout(int timeout_seconds);

// Connection Management
int B100_Test_Connection(void);
int B100_Is_Connected(void);

// Device Info (via /info endpoint - always available)
cJSON* B100_Get_Info(void);
int B100_Fetch_Device_Info(void);

// GPS (via /gps endpoint - always available)
cJSON* B100_Get_GPS(void);
B100_GPS* B100_Get_GPS_Status(void);

// Parameters (via /get and /set - always available)
cJSON* B100_Get_Params(const char* param);
int B100_Set_Params(cJSON* params);

// Status
B100_Status* B100_Get_Status(void);
int B100_Request_Status(void);
const char* B100_Status_Text(int statusCode);

// LoRaWAN Control
int B100_Join(int drJoin, int adr, int drUp);
int B100_Join_Auto(void);
int B100_Restart(void);

// Messaging
int B100_Send(const char* payload, int port, int confirmed);
int B100_Send_Bytes(const unsigned char* data, int length, int port, int confirmed);

// Callback Configuration
int B100_Configure_Callbacks(const char* callback_ip, int callback_port,
                             const char* status_uri, const char* receive_uri,
                             const char* digest_user, const char* digest_password);
int B100_Configure_GPS_Callback(const char* gps_uri, int interval_seconds);

// Callback Processing (called from ACAP HTTP handlers)
int B100_Process_Status_Callback(cJSON* json);
int B100_Process_Receive_Callback(cJSON* json);
int B100_Process_GPS_Callback(cJSON* json);

// Link Check
int B100_Link_Check(void);

// Callbacks
void B100_Set_Downlink_Callback(B100_Downlink_Callback callback);
void B100_Set_Status_Callback(B100_Status_Callback callback);
void B100_Set_GPS_Callback(B100_GPS_Callback callback);

// Utility Functions
const char* B100_Get_Last_Error(void);
void B100_Clear_Error(void);
const char* B100_Get_IP(void);

#ifdef __cplusplus
}
#endif

#endif /* _B100_H_ */
