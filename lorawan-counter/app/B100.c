/*
 * B100.c - AI-B100 LoRaWAN Bridge HTTP Client Implementation
 * Copyright (c) 2026 Fred Juhlin
 * MIT License
 */

#include "B100.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <syslog.h>

#define LOG(fmt, args...)    { syslog(LOG_INFO, "B100: " fmt, ## args); printf("B100: " fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, "B100: " fmt, ## args); printf("B100: " fmt, ## args);}
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, "B100_TRACE: " fmt, ## args); printf("B100_TRACE: " fmt, ## args);}
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

// Downlink deduplication — prevents both the health thread and the downlink poller
// from dispatching the same downlink when both see status 8 on /status.
#include <pthread.h>
static pthread_mutex_t g_downlink_dedup_mutex = PTHREAD_MUTEX_INITIALIZER;
static int    g_downlink_dedup_init = 0;
static unsigned int g_last_dispatched_fcntDown = 0;

// Device info is fetched once from GET / on first connect, then on reconnect.
static int g_device_info_fetched = 0;

// HTTP serialization — the AI-B100 bridge supports only one socket at a time.
// This mutex ensures the two background threads (health + downlink poller) never
// make simultaneous HTTP requests to the bridge, and that response-triggered
// uplinks (e.g. port-12 info replies) don't race with the next status poll.
static pthread_mutex_t g_http_mutex = PTHREAD_MUTEX_INITIALIZER;

// HTTP Response Buffer
typedef struct {
    char* data;
    size_t size;
} HttpResponse;

// Initialize HTTP response buffer
static HttpResponse* http_response_new(void) {
    HttpResponse* resp = malloc(sizeof(HttpResponse));
    if (resp) {
        resp->data = malloc(1);
        resp->size = 0;
        if (resp->data) {
            resp->data[0] = '\0';
        }
    }
    return resp;
}

// Free HTTP response buffer
static void http_response_free(HttpResponse* resp) {
    if (resp) {
        if (resp->data) free(resp->data);
        free(resp);
    }
}

// Callback for libcurl to write response data
static size_t http_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    HttpResponse* resp = (HttpResponse*)userp;
    
    char* ptr = realloc(resp->data, resp->size + realsize + 1);
    if (!ptr) {
        LOG_WARN("Out of memory\n");
        return 0;
    }
    
    resp->data = ptr;
    memcpy(&(resp->data[resp->size]), contents, realsize);
    resp->size += realsize;
    resp->data[resp->size] = '\0';
    
    return realsize;
}

// Perform HTTP GET request
static char* http_get(const char* endpoint) {
    CURL* curl;
    CURLcode res;
    char url[256];
    HttpResponse* response = NULL;
    char* result = NULL;

    pthread_mutex_lock(&g_http_mutex);
    
    snprintf(url, sizeof(url), "http://%s:%d%s", g_ip, g_port, endpoint);
    LOG_TRACE("HTTP GET: %s\n", url);
    
    curl = curl_easy_init();
    if (!curl) {
        snprintf(g_last_error, sizeof(g_last_error), "Failed to initialize curl");
        LOG_WARN("%s\n", g_last_error);
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
        http_response_free(response);
        curl_easy_cleanup(curl);
        pthread_mutex_unlock(&g_http_mutex);
        return NULL;
    }
    
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    if (http_code != 200) {
        snprintf(g_last_error, sizeof(g_last_error), "HTTP error %ld", http_code);
        LOG_WARN("%s\n", g_last_error);
        http_response_free(response);
        curl_easy_cleanup(curl);
        pthread_mutex_unlock(&g_http_mutex);
        return NULL;
    }
    
    // Copy result
    if (response->data && response->size > 0) {
        result = strdup(response->data);
        LOG_TRACE("HTTP Response (%zu bytes): %s\n", response->size, result);
    }
    
    http_response_free(response);
    curl_easy_cleanup(curl);
    pthread_mutex_unlock(&g_http_mutex);
    
    return result;
}

// Initialize B100 client
int B100_Init(const char* ip, int port, int timeout_seconds) {
    if (ip) {
        strncpy(g_ip, ip, sizeof(g_ip) - 1);
        g_ip[sizeof(g_ip) - 1] = '\0';
    }
    
    if (port > 0) {
        g_port = port;
    }
    
    if (timeout_seconds > 0) {
        g_timeout = timeout_seconds;
    }
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    memset(&g_status, 0, sizeof(g_status));
    g_status.connected = B100_NOT_CONNECTED;
    
    LOG("Initialized: %s:%d (timeout: %ds)\n", g_ip, g_port, g_timeout);
    
    return 1;
}

// Cleanup
void B100_Cleanup(void) {
    curl_global_cleanup();
}

// Set IP address
int B100_Set_IP(const char* ip) {
    if (!ip) return 0;
    strncpy(g_ip, ip, sizeof(g_ip) - 1);
    g_ip[sizeof(g_ip) - 1] = '\0';
    return 1;
}

// Set port
int B100_Set_Port(int port) {
    if (port <= 0 || port > 65535) return 0;
    g_port = port;
    return 1;
}

// Set timeout
int B100_Set_Timeout(int timeout_seconds) {
    if (timeout_seconds <= 0) return 0;
    g_timeout = timeout_seconds;
    return 1;
}

// Extract a table cell value that follows a label cell in an HTML table.
// Looks for: <td>label</td> ... <td>VALUE</td>
static int extract_html_td_value(const char* html, const char* label, char* out, size_t outlen) {
    char search[128];
    snprintf(search, sizeof(search), "<td>%s</td>", label);
    const char* p = strstr(html, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (strncmp(p, "<td>", 4) != 0) return 0;
    p += 4;
    const char* end = strstr(p, "</td>");
    if (!end) return 0;
    size_t len = (size_t)(end - p);
    if (len >= outlen) len = outlen - 1;
    strncpy(out, p, len);
    out[len] = '\0';
    return 1;
}

// Fetch hardware and software version strings by GETting the bridge root page.
static void B100_Fetch_Device_Info(void) {
    char* html = http_get("/");
    if (!html) return;
    extract_html_td_value(html, "Hardware:", g_status.hardwareVersion, sizeof(g_status.hardwareVersion));
    extract_html_td_value(html, "Software:", g_status.softwareVersion, sizeof(g_status.softwareVersion));
    free(html);
    g_device_info_fetched = 1;
    LOG("Device info: hw=\"%s\" sw=\"%s\"\n", g_status.hardwareVersion, g_status.softwareVersion);
}

// Test connection
int B100_Test_Connection(void) {
    char* response = http_get("/");
    
    if (response) {
        g_status.connected = B100_CONNECTED;
        // Parse hardware/software version from the root page while we have it
        extract_html_td_value(response, "Hardware:", g_status.hardwareVersion, sizeof(g_status.hardwareVersion));
        extract_html_td_value(response, "Software:", g_status.softwareVersion, sizeof(g_status.softwareVersion));
        g_device_info_fetched = 1;
        LOG("Device info: hw=\"%s\" sw=\"%s\"\n", g_status.hardwareVersion, g_status.softwareVersion);
        free(response);
        return 1;
    }
    
    g_status.connected = B100_NOT_CONNECTED;
    return 0;
}

// Check if connected
int B100_Is_Connected(void) {
    return g_status.connected == B100_CONNECTED;
}

// Get status text from code
const char* B100_Status_Text(int statusCode) {
    switch (statusCode) {
        case B100_STATUS_OK: return "OK";
        case B100_STATUS_RESTARTED: return "Restarted - Ready to Join";
        case B100_STATUS_NO_PAYLOAD: return "No Payload";
        case B100_STATUS_PAYLOAD_TOO_LONG: return "Payload Too Long";
        case B100_STATUS_JOIN_FAILED: return "Join Failed";
        case B100_STATUS_TRYING_TO_JOIN: return "Trying to Join";
        case B100_STATUS_UNKNOWN_ERROR: return "Unknown Error";
        case B100_STATUS_JOINED: return "Joined";
        case B100_STATUS_PAYLOAD_RECEIVED: return "Payload Received";
        case B100_STATUS_PAYLOAD_SENT: return "Payload Sent";
        case B100_STATUS_SENT_CONFIRMED: return "Sent and Confirmed";
        case B100_STATUS_NOT_CONFIRMED: return "Not Confirmed";
        case B100_STATUS_LOST_CONNECTION: return "Lost Connection";
        case B100_STATUS_INVALID_PORT: return "Invalid Port";
        case B100_STATUS_UPLINK_FAILED: return "Uplink Failed";
        case B100_STATUS_PARAMETER_ERROR: return "Parameter Error";
        case B100_STATUS_NOT_JOINED: return "Not Joined";
        case B100_STATUS_PARAMETER_UPDATED: return "Parameter Updated";
        default: return "Unknown Status";
    }
}

// Update status from device
int B100_Update_Status(void) {
    LOG_TRACE("B100_Update_Status called\n");
    char* response = http_get("/status");
    
    if (!response) {
        g_status.connected = B100_NOT_CONNECTED;
        g_device_info_fetched = 0;  // Refetch on next reconnect
        LOG_TRACE("No response from B100 device\n");
        return 0;
    }
    
    g_status.connected = B100_CONNECTED;
    // Fetch hardware/software version from root page on first connect (or reconnect)
    if (!g_device_info_fetched) B100_Fetch_Device_Info();
    LOG_TRACE("B100 connected, parsing status JSON\n");
    
    cJSON* json = cJSON_Parse(response);
    free(response);
    
    if (!json) {
        snprintf(g_last_error, sizeof(g_last_error), "Failed to parse status JSON");
        LOG_TRACE("JSON parse failed\n");
        return 0;
    }
    
    // Check for error
    cJSON* error = cJSON_GetObjectItem(json, "error");
    if (error && error->valuestring) {
        strncpy(g_status.statusText, error->valuestring, sizeof(g_status.statusText) - 1);
        cJSON_Delete(json);
        return 0;
    }
    
    // Parse status fields
    cJSON* item;
    
    if ((item = cJSON_GetObjectItem(json, "status"))) {
        g_status.statusCode = item->valueint;
        strncpy(g_status.statusText, B100_Status_Text(g_status.statusCode), sizeof(g_status.statusText) - 1);
        LOG_TRACE("Status code: %d (%s)\n", g_status.statusCode, g_status.statusText);
        g_status.joined = (g_status.statusCode == B100_STATUS_JOINED || 
                          g_status.statusCode == B100_STATUS_OK ||
                          g_status.statusCode == B100_STATUS_PAYLOAD_RECEIVED ||
                          g_status.statusCode == B100_STATUS_PAYLOAD_SENT ||
                          g_status.statusCode == B100_STATUS_SENT_CONFIRMED);
        LOG_TRACE("Joined status: %s (statusCode=%d)\n", g_status.joined ? "YES" : "NO", g_status.statusCode);
    }
    
    if ((item = cJSON_GetObjectItem(json, "confirmed")))
        g_status.confirmed = item->valueint;

    // drUp, maxUp, fcntUp, devAddr return 0 in status-8/9 responses — only trust
    // them from a status-7 (joined/idle) response to avoid clobbering cached values.
    if (g_status.statusCode == B100_STATUS_JOINED || g_status.statusCode == B100_STATUS_OK) {
        if ((item = cJSON_GetObjectItem(json, "drUp")))
            g_status.dataRateUp = item->valueint;
        if ((item = cJSON_GetObjectItem(json, "maxUp")))
            g_status.maxPayload = item->valueint;
        if ((item = cJSON_GetObjectItem(json, "fcntUp"))) {
            g_status.fcntUp = (unsigned int)item->valueint;
            LOG_TRACE("fcntUp: %u\n", g_status.fcntUp);
        }
        if ((item = cJSON_GetObjectItem(json, "devAddr"))) {
            g_status.devAddr = (unsigned int)item->valueint;
            LOG_TRACE("devAddr: 0x%08X\n", g_status.devAddr);
        }
    }

    if ((item = cJSON_GetObjectItem(json, "drDown")))
        g_status.dataRateDown = item->valueint;

    if ((item = cJSON_GetObjectItem(json, "fcntDown"))) {
        g_status.fcntDown = (unsigned int)item->valueint;
        LOG_TRACE("fcntDown: %u\n", g_status.fcntDown);
    }

    if ((item = cJSON_GetObjectItem(json, "rssi"))) {
        g_status.rssi = (float)item->valuedouble;
        LOG_TRACE("RSSI: %.1f\n", g_status.rssi);
    }

    if ((item = cJSON_GetObjectItem(json, "snr"))) {
        g_status.snr = (float)item->valuedouble;
        LOG_TRACE("SNR: %.1f\n", g_status.snr);
    }

    if ((item = cJSON_GetObjectItem(json, "TempC")))
        g_status.tempC = (float)item->valuedouble;

    g_status.timestamp = time(NULL);

    LOG_TRACE("=== Status Summary: Connected=%d, Joined=%d, StatusCode=%d ===\n",
              g_status.connected, g_status.joined, g_status.statusCode);

    // Downlink dispatch: when status 8 is seen here (health-monitor thread), extract
    // the payload and fire the downlink callback directly — same outcome as the
    // downlink-poller thread calling B100_Receive().  Deduplication by fcntDown
    // ensures each downlink is processed exactly once regardless of which thread
    // wins the race.
    if (g_status.statusCode == B100_STATUS_PAYLOAD_RECEIVED && g_downlink_callback) {
        cJSON* payload_item = cJSON_GetObjectItem(json, "payload");
        if (payload_item && payload_item->valuestring) {
            cJSON* fc_item = cJSON_GetObjectItem(json, "fcntDown");
            unsigned int this_fcnt = fc_item ? (unsigned int)fc_item->valueint : 0;

            pthread_mutex_lock(&g_downlink_dedup_mutex);
            int already_done = g_downlink_dedup_init && (g_last_dispatched_fcntDown == this_fcnt);
            if (!already_done) {
                g_last_dispatched_fcntDown = this_fcnt;
                g_downlink_dedup_init = 1;
            }
            pthread_mutex_unlock(&g_downlink_dedup_mutex);

            if (!already_done) {
                B100_Downlink* dl = malloc(sizeof(B100_Downlink));
                if (dl) {
                    memset(dl, 0, sizeof(B100_Downlink));
                    strncpy(dl->payload, payload_item->valuestring, sizeof(dl->payload) - 1);
                    cJSON* pt = cJSON_GetObjectItem(json, "payload_type");
                    if (pt && pt->valuestring)
                        strncpy(dl->payload_type, pt->valuestring, sizeof(dl->payload_type) - 1);
                    else
                        strncpy(dl->payload_type, "HEX", sizeof(dl->payload_type) - 1);
                    cJSON* len_item = cJSON_GetObjectItem(json, "length");
                    dl->length = len_item ? len_item->valueint : (int)strlen(dl->payload);
                    cJSON* port_item = cJSON_GetObjectItem(json, "port");
                    if (port_item) dl->port = port_item->valueint;
                    cJSON* rssi_item = cJSON_GetObjectItem(json, "rssi");
                    if (rssi_item) dl->rssi = (float)rssi_item->valuedouble;
                    cJSON* snr_item = cJSON_GetObjectItem(json, "snr");
                    if (snr_item) dl->snr = (float)snr_item->valuedouble;
                    dl->fcntDown = (int)this_fcnt;
                    cJSON* conf_item = cJSON_GetObjectItem(json, "confirming");
                    if (conf_item) dl->confirming = conf_item->valueint;
                    LOG("Dispatching downlink (health thread) fcntDown=%u port=%d\n", this_fcnt, dl->port);
                    g_downlink_callback(dl);
                    free(dl);
                }
            } else {
                LOG("Downlink fcntDown=%u already dispatched, skipping duplicate\n", this_fcnt);
            }
        }
    }

    cJSON_Delete(json);

    // Trigger status callback if set
    if (g_status_callback) {
        LOG_TRACE("Calling status callback\n");
        g_status_callback(&g_status);
    }

    return 1;
}

// Get current status
B100_Status* B100_Get_Status(void) {
    return &g_status;
}

// Join LoRaWAN network
int B100_Join(int drJoin, int adr, int drUp) {
    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "/join?drjoin=%d&adr=%d&drUp=%d", drJoin, adr, drUp);
    
    char* response = http_get(endpoint);
    if (!response) {
        return 0;
    }
    
    cJSON* json = cJSON_Parse(response);
    free(response);
    
    if (!json) {
        return 0;
    }
    
    // Check for error
    cJSON* error = cJSON_GetObjectItem(json, "error");
    if (error && error->valuestring) {
        strncpy(g_last_error, error->valuestring, sizeof(g_last_error) - 1);
        cJSON_Delete(json);
        return 0;
    }
    
    // Parse response
    cJSON* status = cJSON_GetObjectItem(json, "status");
    if (status) {
        g_status.statusCode = status->valueint;
        if (g_status.statusCode == B100_STATUS_JOINED) {
            g_status.joined = 1;
            LOG("Successfully joined LoRaWAN network\n");
        }
    }
    
    cJSON* devAddr = cJSON_GetObjectItem(json, "devAddr");
    if (devAddr) {
        g_status.devAddr = (unsigned int)devAddr->valueint;
    }
    
    cJSON_Delete(json);
    return 1;
}

// Join with auto settings
int B100_Join_Auto(void) {
    return B100_Join(0, 1, 4);  // DR0 for join, ADR enabled, DR4 for uplink
}

// Restart device
int B100_Restart(void) {
    char* response = http_get("/set.html?reset");
    if (response) {
        free(response);
        g_status.joined = 0;
        g_status.statusCode = B100_STATUS_RESTARTED;
        LOG("Device restart initiated\n");
        return 1;
    }
    return 0;
}

// Set data rate
int B100_Set_DataRate(int dr) {
    if (dr < 0 || dr > 5) {
        snprintf(g_last_error, sizeof(g_last_error), "Invalid data rate: %d (must be 0-5)", dr);
        return 0;
    }
    
    char endpoint[64];
    snprintf(endpoint, sizeof(endpoint), "/set.html?data_rate=%d", dr);
    
    char* response = http_get(endpoint);
    if (response) {
        free(response);
        return 1;
    }
    return 0;
}

// Set ADR
int B100_Set_ADR(int enabled) {
    char endpoint[64];
    snprintf(endpoint, sizeof(endpoint), "/set.html?adr=%s", enabled ? "yes" : "no");
    
    char* response = http_get(endpoint);
    if (response) {
        free(response);
        g_status.adr = enabled;
        return 1;
    }
    return 0;
}

// Sanitise a payload string for safe use as a URL query value.
// Spaces become '_'; characters outside [A-Za-z0-9._,-] are dropped.
static void sanitize_payload(const char* src, char* dst, size_t dstlen) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dstlen - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == ' ') {
            dst[j++] = '_';
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.' || c == ',') {
            dst[j++] = c;
        }
        // all other characters (spaces already handled, <, >, =, etc.) are dropped
    }
    dst[j] = '\0';
}

// Send message
int B100_Send(const char* payload, int port, int confirmed) {
    if (!payload || port < 1 || port > 223) {
        snprintf(g_last_error, sizeof(g_last_error), "Invalid parameters");
        return 0;
    }

    char safe_payload[960];  // endpoint prefix is ~40 bytes; 960 + 64 fits in 1024
    sanitize_payload(payload, safe_payload, sizeof(safe_payload));

    char endpoint[1024];
    snprintf(endpoint, sizeof(endpoint), "/send?port=%d&confirm=%d&payload=%s",
             port, confirmed ? 1 : 0, safe_payload);

    char* response = http_get(endpoint);
    if (!response) {
        return 0;
    }
    
    cJSON* json = cJSON_Parse(response);
    free(response);
    
    if (!json) {
        return 0;
    }
    
    // Check for error
    cJSON* error = cJSON_GetObjectItem(json, "error");
    if (error && error->valuestring) {
        strncpy(g_last_error, error->valuestring, sizeof(g_last_error) - 1);
        cJSON_Delete(json);
        return 0;
    }
    
    // Check for downlink in response
    cJSON* payload_item = cJSON_GetObjectItem(json, "payload");
    if (payload_item && payload_item->valuestring && g_downlink_callback) {
        B100_Downlink* downlink = malloc(sizeof(B100_Downlink));
        if (downlink) {
            memset(downlink, 0, sizeof(B100_Downlink));
            
            cJSON* port_item = cJSON_GetObjectItem(json, "port");
            if (port_item) downlink->port = port_item->valueint;
            
            strncpy(downlink->payload, payload_item->valuestring, sizeof(downlink->payload) - 1);
            downlink->length = strlen(downlink->payload);
            
            cJSON* rssi_item = cJSON_GetObjectItem(json, "rssi");
            if (rssi_item) downlink->rssi = (float)rssi_item->valuedouble;
            
            cJSON* snr_item = cJSON_GetObjectItem(json, "snr");
            if (snr_item) downlink->snr = (float)snr_item->valuedouble;
            
            g_downlink_callback(downlink);
            free(downlink);
        }
    }
    
    cJSON_Delete(json);
    return 1;
}

// Send JSON message
int B100_Send_JSON(cJSON* json, int port, int confirmed) {
    if (!json) return 0;
    
    char* payload = cJSON_PrintUnformatted(json);
    if (!payload) return 0;
    
    int result = B100_Send(payload, port, confirmed);
    free(payload);
    
    return result;
}

// Receive/poll for downlink
// Polls /status; returns a downlink struct only when status == 8 (payload received).
// Also updates g_status and triggers the status callback as a side effect.
B100_Downlink* B100_Receive(void) {
    syslog(LOG_INFO, "B100: Polling /status for downlink...\n");
    char* response = http_get("/status");
    if (!response) {
        syslog(LOG_INFO, "B100: /status returned NULL (HTTP error)\n");
        g_status.connected = B100_NOT_CONNECTED;
        return NULL;
    }

    syslog(LOG_INFO, "B100: /status response: %s\n", response);

    cJSON* json = cJSON_Parse(response);
    free(response);

    if (!json) {
        syslog(LOG_WARNING, "B100: Failed to parse JSON from /status\n");
        return NULL;
    }

    g_status.connected = B100_CONNECTED;

    // Update status fields (mirrors B100_Update_Status logic)
    cJSON* item;

    if ((item = cJSON_GetObjectItem(json, "status"))) {
        g_status.statusCode = item->valueint;
        strncpy(g_status.statusText, B100_Status_Text(g_status.statusCode), sizeof(g_status.statusText) - 1);
        g_status.joined = (g_status.statusCode == B100_STATUS_JOINED ||
                           g_status.statusCode == B100_STATUS_OK ||
                           g_status.statusCode == B100_STATUS_PAYLOAD_RECEIVED ||
                           g_status.statusCode == B100_STATUS_PAYLOAD_SENT ||
                           g_status.statusCode == B100_STATUS_SENT_CONFIRMED);
    }

    // Only update fields that are reliably present in all /status responses.
    // fcntUp, drUp, maxUp, devAddr are absent or zero in downlink-phase responses
    // (status 8/9) - leave those to B100_Update_Status() to avoid clobbering
    // valid cached values with stale zeros.
    if ((item = cJSON_GetObjectItem(json, "fcntDown")))
        g_status.fcntDown = (unsigned int)item->valueint;
    if ((item = cJSON_GetObjectItem(json, "rssi")))
        g_status.rssi = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "snr")))
        g_status.snr = (float)item->valuedouble;

    g_status.timestamp = time(NULL);

    if (g_status_callback)
        g_status_callback(&g_status);

    // A downlink is present only when status == 8 (B100_STATUS_PAYLOAD_RECEIVED)
    if (g_status.statusCode != B100_STATUS_PAYLOAD_RECEIVED) {
        syslog(LOG_INFO, "B100: No downlink (status=%d)\n", g_status.statusCode);
        cJSON_Delete(json);
        return NULL;
    }

    cJSON* payload_item = cJSON_GetObjectItem(json, "payload");
    if (!payload_item || !payload_item->valuestring) {
        syslog(LOG_INFO, "B100: Status=8 but no payload field in response\n");
        cJSON_Delete(json);
        return NULL;
    }

    B100_Downlink* downlink = malloc(sizeof(B100_Downlink));
    if (!downlink) {
        cJSON_Delete(json);
        return NULL;
    }

    memset(downlink, 0, sizeof(B100_Downlink));

    strncpy(downlink->payload, payload_item->valuestring, sizeof(downlink->payload) - 1);

    // Use the length field from the response (byte count), fall back to string length
    if ((item = cJSON_GetObjectItem(json, "length")))
        downlink->length = item->valueint;
    else
        downlink->length = (int)strlen(downlink->payload);

    cJSON* payload_type_item = cJSON_GetObjectItem(json, "payload_type");
    if (payload_type_item && payload_type_item->valuestring)
        strncpy(downlink->payload_type, payload_type_item->valuestring, sizeof(downlink->payload_type) - 1);
    else
        strncpy(downlink->payload_type, "HEX", sizeof(downlink->payload_type) - 1);

    if ((item = cJSON_GetObjectItem(json, "port")))
        downlink->port = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "rssi")))
        downlink->rssi = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "snr")))
        downlink->snr = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(json, "fcntDown")))
        downlink->fcntDown = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "confirming")))
        downlink->confirming = item->valueint;

    syslog(LOG_INFO, "B100: Downlink on port %d, %d bytes (%s): %s\n",
           downlink->port, downlink->length, downlink->payload_type, downlink->payload);

    // Register this fcntDown as dispatched to prevent the health-monitor thread
    // from double-dispatching the same downlink if it also sees status 8.
    pthread_mutex_lock(&g_downlink_dedup_mutex);
    g_last_dispatched_fcntDown = (unsigned int)downlink->fcntDown;
    g_downlink_dedup_init = 1;
    pthread_mutex_unlock(&g_downlink_dedup_mutex);

    cJSON_Delete(json);
    return downlink;
}

// Free downlink
void B100_Free_Downlink(B100_Downlink* downlink) {
    if (downlink) {
        free(downlink);
    }
}

// Link test
int B100_Link_Test(void) {
    char* response = http_get("/linktest");
    if (response) {
        free(response);
        return 1;
    }
    return 0;
}

// Read LoRaWAN configuration from device (HTML scraping - simplified)
int B100_Read_LoRaWAN_Config(char* devEUI, char* joinEUI, char* appKey) {
    // This would require parsing HTML - for now return not implemented
    // In production, you'd parse the /lora.html page
    snprintf(g_last_error, sizeof(g_last_error), "Config reading not yet implemented");
    return 0;
}

// Set callbacks
void B100_Set_Downlink_Callback(B100_Downlink_Callback callback) {
    g_downlink_callback = callback;
}

void B100_Set_Status_Callback(B100_Status_Callback callback) {
    g_status_callback = callback;
}

// Get last error
const char* B100_Get_Last_Error(void) {
    return g_last_error;
}

// Clear error
void B100_Clear_Error(void) {
    g_last_error[0] = '\0';
}
