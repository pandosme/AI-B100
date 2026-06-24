/*
 * B100.c - AI-B100 LoRaWAN Bridge HTTP Client Implementation
 * Copyright (c) 2026 Fred Juhlin
 * MIT License
 *
 * Uses the AI-B100 HTTP API with callback-driven architecture:
 *   /info     (GET)  - Device info (always available)
 *   /get      (GET)  - Read parameters (always available)
 *   /set      (POST) - Write parameters (always available)
 *   /restart  (GET)  - Hardware restart (always available)
 *   /status   (GET)  - Trigger status callback (requires HTTP API + callbacks)
 *   /join     (POST) - Join LoRaWAN (requires HTTP API + callbacks)
 *   /send     (POST) - Send LoRaWAN message (requires HTTP API + callbacks)
 *   /linkcheck(GET)  - LoRaWAN link check (requires HTTP API + callbacks)
 */

#include "B100.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <syslog.h>

#define LOG(fmt, args...)    { syslog(LOG_INFO, "B100: " fmt, ## args); printf("B100: " fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, "B100: " fmt, ## args); printf("B100: " fmt, ## args);}
#define LOG_API(fmt, args...)    { syslog(LOG_INFO, "B100_API: " fmt, ## args); printf("B100_API: " fmt, ## args);}
#define LOG_TRACE(fmt, args...)    {}

// Configuration
static char g_ip[64] = "192.168.0.3";
static int g_port = 80;
static int g_timeout = 30;

// Status
static B100_Status g_status = {0};
static char g_last_error[256] = {0};

// Callbacks
static B100_Downlink_Callback g_downlink_callback = NULL;
static B100_Status_Callback g_status_callback = NULL;
static B100_GPS_Callback g_gps_callback = NULL;

// GPS state
static B100_GPS g_gps = {0};

// HTTP serialization — the AI-B100 bridge supports only one socket at a time.
#include <pthread.h>
static pthread_mutex_t g_http_mutex = PTHREAD_MUTEX_INITIALIZER;

// Downlink deduplication
static pthread_mutex_t g_downlink_dedup_mutex = PTHREAD_MUTEX_INITIALIZER;
static int    g_downlink_dedup_init = 0;
static unsigned int g_last_dispatched_fcntDown = 0;

// HTTP Response Buffer
typedef struct {
    char* data;
    size_t size;
} HttpResponse;

static HttpResponse* http_response_new(void) {
    HttpResponse* resp = malloc(sizeof(HttpResponse));
    if (resp) {
        resp->data = malloc(1);
        resp->size = 0;
        if (resp->data) resp->data[0] = '\0';
    }
    return resp;
}

static void http_response_free(HttpResponse* resp) {
    if (resp) {
        if (resp->data) free(resp->data);
        free(resp);
    }
}

static size_t http_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    HttpResponse* resp = (HttpResponse*)userp;
    char* ptr = realloc(resp->data, resp->size + realsize + 1);
    if (!ptr) { LOG_WARN("Out of memory\n"); return 0; }
    resp->data = ptr;
    memcpy(&(resp->data[resp->size]), contents, realsize);
    resp->size += realsize;
    resp->data[resp->size] = '\0';
    return realsize;
}

// Perform HTTP GET request. Accepts 200 and 202 responses.
// Returns response body (caller must free) or NULL on error.
// *http_code_out receives the HTTP status code if non-NULL.
static char* http_get_ex(const char* endpoint, long* http_code_out) {
    CURL* curl;
    CURLcode res;
    char url[256];
    HttpResponse* response = NULL;
    char* result = NULL;

    pthread_mutex_lock(&g_http_mutex);

    snprintf(url, sizeof(url), "http://%s:%d%s", g_ip, g_port, endpoint);
    LOG_API("REQUEST GET %s\n", url);

    curl = curl_easy_init();
    if (!curl) {
        snprintf(g_last_error, sizeof(g_last_error), "Failed to initialize curl");
        LOG_WARN("%s\n", g_last_error);
        pthread_mutex_unlock(&g_http_mutex);
        return NULL;
    }

    response = http_response_new();
    if (!response) {
        curl_easy_cleanup(curl);
        pthread_mutex_unlock(&g_http_mutex);
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, g_timeout);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        snprintf(g_last_error, sizeof(g_last_error), "HTTP GET failed: %s", curl_easy_strerror(res));
        LOG_WARN("%s\n", g_last_error);
        LOG_API("RESPONSE GET %s curl_error=%s\n", url, curl_easy_strerror(res));
        http_response_free(response);
        curl_easy_cleanup(curl);
        pthread_mutex_unlock(&g_http_mutex);
        return NULL;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code_out) *http_code_out = http_code;

        LOG_API("RESPONSE GET %s http=%ld body=%s\n",
            url, http_code,
            response->data && response->size > 0 ? response->data : "(empty)");

    if (http_code != 200 && http_code != 202) {
        snprintf(g_last_error, sizeof(g_last_error), "HTTP %ld from %s", http_code, endpoint);
        LOG_WARN("%s\n", g_last_error);
        http_response_free(response);
        curl_easy_cleanup(curl);
        pthread_mutex_unlock(&g_http_mutex);
        return NULL;
    }

    if (response->data && response->size > 0) {
        result = strdup(response->data);
    }

    http_response_free(response);
    curl_easy_cleanup(curl);
    pthread_mutex_unlock(&g_http_mutex);
    return result;
}

// Convenience wrapper: GET expecting 200 with body
static char* http_get(const char* endpoint) {
    return http_get_ex(endpoint, NULL);
}

// Perform HTTP POST with JSON body.
// Returns response body (caller must free) or NULL on error.
// *http_code_out receives the HTTP status code if non-NULL.
static char* http_post_json(const char* endpoint, const char* json_body, long* http_code_out) {
    CURL* curl;
    CURLcode res;
    char url[256];
    HttpResponse* response = NULL;
    char* result = NULL;

    pthread_mutex_lock(&g_http_mutex);

    snprintf(url, sizeof(url), "http://%s:%d%s", g_ip, g_port, endpoint);
    LOG_API("REQUEST POST %s body=%s\n", url, json_body ? json_body : "(none)");

    curl = curl_easy_init();
    if (!curl) {
        snprintf(g_last_error, sizeof(g_last_error), "Failed to initialize curl");
        LOG_WARN("%s\n", g_last_error);
        pthread_mutex_unlock(&g_http_mutex);
        return NULL;
    }

    response = http_response_new();
    if (!response) {
        curl_easy_cleanup(curl);
        pthread_mutex_unlock(&g_http_mutex);
        return NULL;
    }

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (json_body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_body));
    } else {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, g_timeout);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    res = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        snprintf(g_last_error, sizeof(g_last_error), "HTTP POST failed: %s", curl_easy_strerror(res));
        LOG_WARN("%s\n", g_last_error);
        LOG_API("RESPONSE POST %s curl_error=%s\n", url, curl_easy_strerror(res));
        http_response_free(response);
        curl_easy_cleanup(curl);
        pthread_mutex_unlock(&g_http_mutex);
        return NULL;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code_out) *http_code_out = http_code;

    LOG_API("RESPONSE POST %s http=%ld body=%s\n",
            url, http_code,
            response->data && response->size > 0 ? response->data : "(empty)");

    if (response->data && response->size > 0) {
        result = strdup(response->data);
    }

    if (http_code != 200 && http_code != 202) {
        snprintf(g_last_error, sizeof(g_last_error), "HTTP %ld from POST %s", http_code, endpoint);
        LOG_WARN("%s\n", g_last_error);
    }

    http_response_free(response);
    curl_easy_cleanup(curl);
    pthread_mutex_unlock(&g_http_mutex);
    return result;
}

// ==================================================================
// Initialize / Cleanup
// ==================================================================

int B100_Init(const char* ip, int port, int timeout_seconds) {
    if (ip) {
        strncpy(g_ip, ip, sizeof(g_ip) - 1);
        g_ip[sizeof(g_ip) - 1] = '\0';
    }
    if (port > 0) g_port = port;
    if (timeout_seconds > 0) g_timeout = timeout_seconds;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    memset(&g_status, 0, sizeof(g_status));
    g_status.connected = B100_NOT_CONNECTED;

    LOG("Initialized: %s:%d (timeout: %ds)\n", g_ip, g_port, g_timeout);
    return 1;
}

void B100_Cleanup(void) {
    curl_global_cleanup();
}

int B100_Set_IP(const char* ip) {
    if (!ip) return 0;
    strncpy(g_ip, ip, sizeof(g_ip) - 1);
    g_ip[sizeof(g_ip) - 1] = '\0';
    return 1;
}

int B100_Set_Port(int port) {
    if (port <= 0 || port > 65535) return 0;
    g_port = port;
    return 1;
}

int B100_Set_Timeout(int timeout_seconds) {
    if (timeout_seconds <= 0) return 0;
    g_timeout = timeout_seconds;
    return 1;
}

const char* B100_Get_IP(void) {
    return g_ip;
}

// ==================================================================
// /info endpoint — always available, returns device information JSON
// ==================================================================

static void parse_info_json(cJSON* json) {
    cJSON* item;

    if ((item = cJSON_GetObjectItem(json, "HW")) && item->valuestring)
        strncpy(g_status.hardware, item->valuestring, sizeof(g_status.hardware) - 1);
    if ((item = cJSON_GetObjectItem(json, "HW_ver")) && item->valuestring)
        strncpy(g_status.hardwareVersion, item->valuestring, sizeof(g_status.hardwareVersion) - 1);
    if ((item = cJSON_GetObjectItem(json, "FW_ver")) && item->valuestring)
        strncpy(g_status.firmwareVersion, item->valuestring, sizeof(g_status.firmwareVersion) - 1);
    if ((item = cJSON_GetObjectItem(json, "power")) && item->valuestring)
        strncpy(g_status.powerSource, item->valuestring, sizeof(g_status.powerSource) - 1);
    if ((item = cJSON_GetObjectItem(json, "dhcp_enable")))
        g_status.dhcpEnabled = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "ip_addr")) && item->valuestring)
        strncpy(g_status.ipAddr, item->valuestring, sizeof(g_status.ipAddr) - 1);
    if ((item = cJSON_GetObjectItem(json, "dev_eui")) && item->valuestring)
        strncpy(g_status.devEUI, item->valuestring, sizeof(g_status.devEUI) - 1);
    if ((item = cJSON_GetObjectItem(json, "dev_addr"))) {
        if (cJSON_IsString(item)) {
            strncpy(g_status.devAddrStr, item->valuestring, sizeof(g_status.devAddrStr) - 1);
            if (strcmp(item->valuestring, "0") != 0)
                g_status.devAddr = (unsigned int)strtoul(item->valuestring, NULL, 16);
            else
                g_status.devAddr = 0;
        }
    }
    if ((item = cJSON_GetObjectItem(json, "restart_counter")))
        g_status.restartCounter = (unsigned int)item->valueint;
    if ((item = cJSON_GetObjectItem(json, "mqtt_enable")))
        g_status.mqttEnabled = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "http_api_enable")))
        g_status.httpApiEnabled = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "tUnix")))
        g_status.tUnix = (unsigned long)item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "TempC")))
        g_status.tempC = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "tamper")))
        g_status.tamper = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "gps_status")))
        g_status.gpsStatus = item->valueint;
}

cJSON* B100_Get_Info(void) {
    char* response = http_get("/info");
    if (!response) return NULL;
    cJSON* json = cJSON_Parse(response);
    free(response);
    return json;
}

int B100_Fetch_Device_Info(void) {
    cJSON* info = B100_Get_Info();
    if (!info) return 0;

    parse_info_json(info);
    g_status.connected = B100_CONNECTED;

    // Determine join status from dev_addr ("0" = not joined)
    g_status.joined = (g_status.devAddr != 0) ? 1 : 0;

    LOG("Device info: %s v%s, FW %s, power=%s, dev_eui=%s, dev_addr=%s\n",
        g_status.hardware, g_status.hardwareVersion,
        g_status.firmwareVersion, g_status.powerSource,
        g_status.devEUI, g_status.devAddrStr);

    cJSON_Delete(info);
    return 1;
}

// ==================================================================
// /gps endpoint — always available
// ==================================================================

cJSON* B100_Get_GPS(void) {
    char* response = http_get("/gps");
    if (!response) return NULL;
    cJSON* json = cJSON_Parse(response);
    free(response);
    if (!json) return NULL;
    // Also update internal GPS state and fire the registered callback
    // so ACAP status stays current even when polled (not only via push callback).
    B100_Process_GPS_Callback(json);
    return json;
}

B100_GPS* B100_Get_GPS_Status(void) {
    return &g_gps;
}

// Parse GPS JSON into g_gps and invoke the GPS callback.
// Called from both the HTTP GPS callback endpoint and B100_Get_GPS() polling.
int B100_Process_GPS_Callback(cJSON* json) {
    if (!json) return 0;

    cJSON* item;

    if ((item = cJSON_GetObjectItem(json, "gps_status")))
        g_gps.gps_status = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "ns")) && item->valuestring)
        strncpy(g_gps.ns, item->valuestring, sizeof(g_gps.ns) - 1);
    if ((item = cJSON_GetObjectItem(json, "lat")))
        g_gps.lat = item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "ew")) && item->valuestring)
        strncpy(g_gps.ew, item->valuestring, sizeof(g_gps.ew) - 1);
    if ((item = cJSON_GetObjectItem(json, "lon")))
        g_gps.lon = item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "alt")))
        g_gps.alt = item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "nosv")))
        g_gps.nosv = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "pdop")))
        g_gps.pdop = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "hdop")))
        g_gps.hdop = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "vdop")))
        g_gps.vdop = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "utc")) && item->valuestring)
        strncpy(g_gps.utc, item->valuestring, sizeof(g_gps.utc) - 1);
    if ((item = cJSON_GetObjectItem(json, "date")) && item->valuestring)
        strncpy(g_gps.date, item->valuestring, sizeof(g_gps.date) - 1);
    if ((item = cJSON_GetObjectItem(json, "sog")))
        g_gps.sog = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "cog")))
        g_gps.cog = (float)item->valuedouble;

    LOG_TRACE("GPS callback: status=%d lat=%.6f lon=%.6f nosv=%d\n",
              g_gps.gps_status, g_gps.lat, g_gps.lon, g_gps.nosv);

    if (g_gps_callback)
        g_gps_callback(&g_gps);

    return 1;
}

// Configure GPS callback URI and update interval on the B100.
// gps_uri    — path the B100 will POST to (e.g. "/local/aib100/b100_gps")
// interval   — seconds between GPS callbacks (0 = disabled)
int B100_Configure_GPS_Callback(const char* gps_uri, int interval_seconds) {
    if (!gps_uri) return 0;
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "callback_gps_uri", gps_uri);
    cJSON_AddNumberToObject(params, "gps_update_interval", interval_seconds);

    int result = B100_Set_Params(params);
    cJSON_Delete(params);

    if (result) {
        LOG("GPS callback configured: uri=%s interval=%ds\n", gps_uri, interval_seconds);
    } else {
        LOG_WARN("Failed to configure GPS callback\n");
    }
    return result;
}

void B100_Set_GPS_Callback(B100_GPS_Callback callback) {
    g_gps_callback = callback;
}

// ==================================================================
// /get and /set endpoints — always available
// ==================================================================

cJSON* B100_Get_Params(const char* param) {
    char endpoint[128];
    if (param && param[0])
        snprintf(endpoint, sizeof(endpoint), "/get?%s", param);
    else
        snprintf(endpoint, sizeof(endpoint), "/get");

    char* response = http_get(endpoint);
    if (!response) return NULL;

    cJSON* json = cJSON_Parse(response);
    free(response);
    return json;
}

int B100_Set_Params(cJSON* params) {
    if (!params) return 0;

    char* body = cJSON_PrintUnformatted(params);
    if (!body) return 0;

    long http_code = 0;
    char* response = http_post_json("/set", body, &http_code);
    free(body);

    if (http_code == 200) {
        if (response) {
            cJSON* json = cJSON_Parse(response);
            if (json) {
                cJSON* restart = cJSON_GetObjectItem(json, "restart_required");
                if (restart && restart->valueint)
                    LOG("Parameter update requires device restart\n");
                cJSON_Delete(json);
            }
            free(response);
        }
        return 1;
    }

    if (response) {
        cJSON* json = cJSON_Parse(response);
        if (json) {
            cJSON* failed = cJSON_GetObjectItem(json, "failed");
            if (failed && failed->valuestring)
                snprintf(g_last_error, sizeof(g_last_error), "Parameter error: %s", failed->valuestring);
            cJSON_Delete(json);
        }
        free(response);
    }
    return 0;
}

// ==================================================================
// Connection Test (via /info — always available)
// ==================================================================

int B100_Test_Connection(void) {
    if (B100_Fetch_Device_Info()) {
        g_status.connected = B100_CONNECTED;
        return 1;
    }
    g_status.connected = B100_NOT_CONNECTED;
    return 0;
}

int B100_Is_Connected(void) {
    return g_status.connected == B100_CONNECTED;
}

// ==================================================================
// Status
// ==================================================================

const char* B100_Status_Text(int statusCode) {
    switch (statusCode) {
        case B100_STATUS_OK:              return "OK";
        case B100_STATUS_RESTARTED:       return "Restarted - Ready to Join";
        case B100_STATUS_NO_PAYLOAD:      return "No Payload";
        case B100_STATUS_PAYLOAD_TOO_LONG:return "Payload Too Long";
        case B100_STATUS_JOIN_FAILED:     return "Join Failed";
        case B100_STATUS_AUTOJOIN_ENABLED:return "Restarted - Auto Join Enabled";
        case B100_STATUS_UNKNOWN_ERROR:   return "Unknown Error";
        case B100_STATUS_JOINED:          return "Joined";
        case B100_STATUS_PAYLOAD_RECEIVED:return "Payload Received";
        case B100_STATUS_PAYLOAD_SENT:    return "Payload Sent";
        case B100_STATUS_SENT_CONFIRMED:  return "Payload Confirmed";
        case B100_STATUS_NOT_CONFIRMED:   return "Payload Not Confirmed";
        case B100_STATUS_MQTT_HEARTBEAT:  return "MQTT Heartbeat";
        case B100_STATUS_DUTY_CYCLE:      return "Uplink Unavailable (Duty Cycle)";
        case B100_STATUS_LOST_CONNECTION: return "Lost Connection";
        case B100_STATUS_INVALID_PORT:    return "Invalid Port";
        case B100_STATUS_UPLINK_FAILED:   return "Uplink Failed";
        case B100_STATUS_PARAMETER_ERROR: return "Parameter Error";
        case B100_STATUS_NOT_JOINED:      return "Not Joined";
        case B100_STATUS_PARAMETER_UPDATED:return "Parameter Updated";
        default: return "Unknown Status";
    }
}

B100_Status* B100_Get_Status(void) {
    return &g_status;
}

// Trigger a status request via GET /status.
// The B100 returns 202 and delivers the actual status asynchronously
// via POST to the configured callback_status_uri.
int B100_Request_Status(void) {
    long http_code = 0;
    char* response = http_get_ex("/status", &http_code);
    if (response) free(response);

    if (http_code == 202) {
        g_status.connected = B100_CONNECTED;
        return 1;
    }
    if (http_code == 412) {
        snprintf(g_last_error, sizeof(g_last_error), "Callbacks not configured");
        LOG_WARN("Status request failed: callbacks not configured (412)\n");
        g_status.connected = B100_CONNECTED;  // device is reachable
        return 0;
    }
    g_status.connected = B100_NOT_CONNECTED;
    return 0;
}

// ==================================================================
// Callback processing — called from ACAP HTTP endpoint handlers
// when the B100 POSTs status or downlink data
// ==================================================================

// Process a status callback POST from the B100.
// Fields: status, dev_addr, confirmed, fcntUp, data_rate, maxUp, tUnix, next_upload_ms
int B100_Process_Status_Callback(cJSON* json) {
    if (!json) return 0;

    cJSON* item;

    g_status.connected = B100_CONNECTED;

    if ((item = cJSON_GetObjectItem(json, "status"))) {
        g_status.statusCode = item->valueint;
        strncpy(g_status.statusText, B100_Status_Text(g_status.statusCode),
                sizeof(g_status.statusText) - 1);

        g_status.joined = (g_status.statusCode == B100_STATUS_JOINED ||
                          g_status.statusCode == B100_STATUS_OK ||
                          g_status.statusCode == B100_STATUS_PAYLOAD_RECEIVED ||
                          g_status.statusCode == B100_STATUS_PAYLOAD_SENT ||
                          g_status.statusCode == B100_STATUS_SENT_CONFIRMED ||
                          g_status.statusCode == B100_STATUS_NOT_CONFIRMED);
        LOG_TRACE("Status callback: code=%d (%s) joined=%d\n",
                  g_status.statusCode, g_status.statusText, g_status.joined);
    }

    if ((item = cJSON_GetObjectItem(json, "dev_addr"))) {
        if (cJSON_IsString(item)) {
            strncpy(g_status.devAddrStr, item->valuestring, sizeof(g_status.devAddrStr) - 1);
            if (strcmp(item->valuestring, "0") != 0)
                g_status.devAddr = (unsigned int)strtoul(item->valuestring, NULL, 16);
            else
                g_status.devAddr = 0;
        }
    }

    if ((item = cJSON_GetObjectItem(json, "confirmed")))
        g_status.confirmed = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "fcntUp")))
        g_status.fcntUp = (unsigned int)item->valueint;
    if ((item = cJSON_GetObjectItem(json, "data_rate")))
        g_status.dataRate = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "maxUp")))
        g_status.maxPayload = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "tUnix")))
        g_status.tUnix = (unsigned long)item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "next_upload_ms")))
        g_status.nextUploadMs = (unsigned long)item->valuedouble;

    g_status.timestamp = time(NULL);

    if (g_status_callback)
        g_status_callback(&g_status);

    return 1;
}

// Process a receive callback POST from the B100.
// Fields: confirmed, fcntDown, rssi, snr, tUnix, margin, gwCount,
//         next_upload_ms, port, length, payload
int B100_Process_Receive_Callback(cJSON* json) {
    if (!json) return 0;

    cJSON* item;

    g_status.connected = B100_CONNECTED;

    // Update signal quality and timing
    if ((item = cJSON_GetObjectItem(json, "confirmed")))
        g_status.confirmed = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "fcntDown")))
        g_status.fcntDown = (unsigned int)item->valueint;
    if ((item = cJSON_GetObjectItem(json, "rssi")))
        g_status.rssi = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "snr")))
        g_status.snr = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "tUnix")))
        g_status.tUnix = (unsigned long)item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "next_upload_ms")))
        g_status.nextUploadMs = (unsigned long)item->valuedouble;

    // Linkcheck fields (only present in linkcheck responses)
    if ((item = cJSON_GetObjectItem(json, "margin")))
        g_status.margin = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "gwCount")))
        g_status.gwCount = item->valueint;

    LOG("Receive callback: fcntDown=%u rssi=%.1f snr=%.1f\n",
        g_status.fcntDown, g_status.rssi, g_status.snr);

    g_status.timestamp = time(NULL);

    // Check for downlink payload
    cJSON* length_item = cJSON_GetObjectItem(json, "length");
    int payload_length = length_item ? length_item->valueint : 0;

    if (payload_length == 0) {
        LOG("Receive callback: no payload (ACK or linkcheck), fcntDown=%u\n", g_status.fcntDown);
    }

    if (payload_length > 0 && g_downlink_callback) {
        cJSON* payload_item = cJSON_GetObjectItem(json, "payload");
        if (!payload_item)
            LOG_WARN("Receive callback: length=%d but no 'payload' field in JSON\n", payload_length);
        if (payload_item) {
            unsigned int this_fcnt = g_status.fcntDown;

            // Deduplication by fcntDown
            pthread_mutex_lock(&g_downlink_dedup_mutex);
            int already_done = g_downlink_dedup_init &&
                               (g_last_dispatched_fcntDown == this_fcnt);
            if (!already_done) {
                g_last_dispatched_fcntDown = this_fcnt;
                g_downlink_dedup_init = 1;
            }
            pthread_mutex_unlock(&g_downlink_dedup_mutex);

            if (!already_done) {
                B100_Downlink* dl = malloc(sizeof(B100_Downlink));
                if (dl) {
                    memset(dl, 0, sizeof(B100_Downlink));

                    // Payload can be string (ASCII), array [1,2,255], or Buffer object
                    if (cJSON_IsString(payload_item)) {
                        strncpy(dl->payload, payload_item->valuestring,
                                sizeof(dl->payload) - 1);
                        strncpy(dl->payload_type, "ASCII", sizeof(dl->payload_type) - 1);
                    } else if (cJSON_IsArray(payload_item)) {
                        // Byte array → hex string for backward compat
                        int n = cJSON_GetArraySize(payload_item);
                        int pos = 0;
                        for (int i = 0; i < n && pos < (int)sizeof(dl->payload) - 2; i++) {
                            cJSON* b = cJSON_GetArrayItem(payload_item, i);
                            pos += snprintf(dl->payload + pos, sizeof(dl->payload) - pos,
                                            "%02X", b ? (b->valueint & 0xFF) : 0);
                        }
                        strncpy(dl->payload_type, "HEX", sizeof(dl->payload_type) - 1);
                    } else if (cJSON_IsObject(payload_item)) {
                        // Node.js Buffer: {"type":"Buffer","data":[1,2,255]}
                        cJSON* bdata = cJSON_GetObjectItem(payload_item, "data");
                        if (bdata && cJSON_IsArray(bdata)) {
                            int n = cJSON_GetArraySize(bdata);
                            int pos = 0;
                            for (int i = 0; i < n && pos < (int)sizeof(dl->payload) - 2; i++) {
                                cJSON* b = cJSON_GetArrayItem(bdata, i);
                                pos += snprintf(dl->payload + pos, sizeof(dl->payload) - pos,
                                                "%02X", b ? (b->valueint & 0xFF) : 0);
                            }
                        }
                        strncpy(dl->payload_type, "HEX", sizeof(dl->payload_type) - 1);
                    } else {
                        strncpy(dl->payload_type, "HEX", sizeof(dl->payload_type) - 1);
                    }

                    dl->length = payload_length;
                    cJSON* port_item = cJSON_GetObjectItem(json, "port");
                    if (port_item) dl->port = port_item->valueint;
                    dl->rssi = g_status.rssi;
                    dl->snr = g_status.snr;
                    dl->fcntDown = (int)this_fcnt;
                    dl->confirming = g_status.confirmed;

                    LOG("Downlink callback: port=%d, %d bytes, fcntDown=%u\n",
                        dl->port, dl->length, this_fcnt);
                    g_downlink_callback(dl);
                    free(dl);
                }
            } else {
                LOG_TRACE("Downlink fcntDown=%u already dispatched\n", this_fcnt);
            }
        }
    }

    g_status.receiveTUnix = (unsigned long)time(NULL);

    if (g_status_callback)
        g_status_callback(&g_status);

    return 1;
}

// ==================================================================
// /join endpoint — POST with JSON body
// ==================================================================

int B100_Join(int drJoin, int adr, int drUp) {
    cJSON* body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "data_rate_join", drJoin);
    cJSON_AddNumberToObject(body, "adr_enable", adr ? 1 : 0);
    cJSON_AddNumberToObject(body, "data_rate", drUp);

    char* json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) return 0;

    long http_code = 0;
    char* response = http_post_json("/join", json_str, &http_code);
    free(json_str);
    if (response) free(response);

    if (http_code == 202) {
        LOG("Join request accepted (DR join=%d, ADR=%d, DR=%d)\n", drJoin, adr, drUp);
        return 1;
    }
    if (http_code == 412) {
        snprintf(g_last_error, sizeof(g_last_error), "Callbacks not configured");
        LOG_WARN("Join failed: callbacks not configured (412)\n");
    } else if (http_code == 400) {
        snprintf(g_last_error, sizeof(g_last_error), "Invalid join parameters");
        LOG_WARN("Join failed: bad request (400)\n");
    } else {
        LOG_WARN("Join failed: HTTP %ld\n", http_code);
    }
    return 0;
}

int B100_Join_Auto(void) {
    return B100_Join(0, 1, 4);  // DR0 for join, ADR enabled, DR4 for uplink
}

// ==================================================================
// /restart endpoint — always available
// ==================================================================

int B100_Restart(void) {
    long http_code = 0;
    char* response = http_get_ex("/restart", &http_code);
    if (response) free(response);

    if (http_code == 202) {
        g_status.joined = 0;
        g_status.statusCode = B100_STATUS_RESTARTED;
        strncpy(g_status.statusText, "Restarted", sizeof(g_status.statusText) - 1);
        LOG("Device restart initiated\n");
        return 1;
    }
    LOG_WARN("Restart failed: HTTP %ld\n", http_code);
    return 0;
}

// ==================================================================
// /send endpoint — POST with JSON body
// Supports ASCII string payload or byte-array payload
// ==================================================================

int B100_Send(const char* payload, int port, int confirmed) {
    if (!payload || port < 1 || port > 223) {
        snprintf(g_last_error, sizeof(g_last_error), "Invalid parameters");
        return 0;
    }

    cJSON* body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "port", port);
    cJSON_AddNumberToObject(body, "confirm", confirmed ? 1 : 0);
    cJSON_AddStringToObject(body, "payload", payload);

    char* json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) return 0;

    long http_code = 0;
    char* response = http_post_json("/send", json_str, &http_code);
    free(json_str);
    if (response) free(response);

    if (http_code == 202) {
        LOG("Send accepted: port=%d, confirm=%d\n", port, confirmed);
        return 1;
    }
    if (http_code == 409) {
        snprintf(g_last_error, sizeof(g_last_error), "Not joined");
    } else if (http_code == 412) {
        snprintf(g_last_error, sizeof(g_last_error), "Callbacks not configured");
    } else if (http_code == 413) {
        snprintf(g_last_error, sizeof(g_last_error), "Payload too large");
    } else if (http_code == 400) {
        snprintf(g_last_error, sizeof(g_last_error), "Invalid send parameters");
    }
    LOG_WARN("Send failed: HTTP %ld\n", http_code);
    return 0;
}

// Send raw byte array via POST /send using JSON array payload: [1, 2, 255]
// This sends the bytes directly as LoRaWAN payload, no base64 encoding needed.
int B100_Send_Bytes(const unsigned char* data, int length, int port, int confirmed) {
    if (!data || length <= 0 || port < 1 || port > 223) {
        snprintf(g_last_error, sizeof(g_last_error), "Invalid parameters");
        return 0;
    }

    cJSON* body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "port", port);
    cJSON_AddNumberToObject(body, "confirm", confirmed ? 1 : 0);

    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < length; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(data[i]));
    cJSON_AddItemToObject(body, "payload", arr);

    char* json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) return 0;

    long http_code = 0;
    char* response = http_post_json("/send", json_str, &http_code);
    free(json_str);
    if (response) free(response);

    if (http_code == 202) {
        LOG("Send bytes accepted: port=%d, confirm=%d, %d bytes\n", port, confirmed, length);
        return 1;
    }
    if (http_code == 409)
        snprintf(g_last_error, sizeof(g_last_error), "Not joined");
    else if (http_code == 412)
        snprintf(g_last_error, sizeof(g_last_error), "Callbacks not configured");
    else if (http_code == 413)
        snprintf(g_last_error, sizeof(g_last_error), "Payload too large");
    LOG_WARN("Send bytes failed: HTTP %ld\n", http_code);
    return 0;
}

// ==================================================================
// /linkcheck endpoint
// ==================================================================

int B100_Link_Check(void) {
    long http_code = 0;
    char* response = http_get_ex("/linkcheck", &http_code);
    if (response) free(response);

    if (http_code == 202) {
        LOG("Linkcheck request accepted\n");
        return 1;
    }
    if (http_code == 409) {
        snprintf(g_last_error, sizeof(g_last_error), "Not joined");
        LOG_WARN("Linkcheck failed: not joined (409)\n");
    } else if (http_code == 412) {
        snprintf(g_last_error, sizeof(g_last_error), "Callbacks not configured");
        LOG_WARN("Linkcheck failed: callbacks not configured (412)\n");
    }
    return 0;
}

// ==================================================================
// Callback Configuration via /set
// ==================================================================

int B100_Configure_Callbacks(const char* callback_ip, int callback_port,
                             const char* status_uri, const char* receive_uri,
                             const char* digest_user, const char* digest_password) {
    cJSON* params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "http_api_enable", 1);
    if (callback_ip && callback_ip[0])
        cJSON_AddStringToObject(params, "callback_addr", callback_ip);
    if (callback_port > 0)
        cJSON_AddNumberToObject(params, "callback_port", callback_port);
    cJSON_AddStringToObject(params, "callback_status_uri", status_uri);
    cJSON_AddStringToObject(params, "callback_receive_uri", receive_uri);
    if (digest_user)
        cJSON_AddStringToObject(params, "callback_digest_user", digest_user);
    if (digest_password)
        cJSON_AddStringToObject(params, "callback_digest_password", digest_password);

    int result = B100_Set_Params(params);
    cJSON_Delete(params);

    if (result) {
        LOG("Callbacks configured: addr=%s:%d status=%s receive=%s user=%s\n",
            callback_ip ? callback_ip : "(unchanged)", callback_port, status_uri, receive_uri,
            digest_user && digest_user[0] ? digest_user : "(none)");
    } else {
        LOG_WARN("Failed to configure callbacks\n");
    }
    return result;
}

// ==================================================================
// Callbacks
// ==================================================================

void B100_Set_Downlink_Callback(B100_Downlink_Callback callback) {
    g_downlink_callback = callback;
}

void B100_Set_Status_Callback(B100_Status_Callback callback) {
    g_status_callback = callback;
}

const char* B100_Get_Last_Error(void) {
    return g_last_error;
}

void B100_Clear_Error(void) {
    g_last_error[0] = '\0';
}
