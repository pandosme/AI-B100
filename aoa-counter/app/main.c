#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <glib.h>
#include <glib-unix.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ACAP.h"
#include "cJSON.h"
#include "B100.h"

#define APP_PACKAGE	"aib100"
#define DOWNLINK_LOG_MAX 10

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, "TRACE: " fmt, ## args); printf("TRACE: " fmt, ## args); }

// Global state
static GMainLoop *main_loop = NULL;
static pthread_t health_thread;
static pthread_t publish_thread;
static int running = 1;
static cJSON* eventSubscriptions = NULL;
static int g_callbacks_configured = 0;
static cJSON* g_downlink_log = NULL;

// Transmission settings
static int g_publish_interval_minutes = 15;
static int g_publish_enabled = 1;
static int g_publish_human = 1;
static int g_publish_car = 1;
static int g_publish_bike = 1;
static int g_publish_bus = 1;
static int g_publish_truck = 1;
static int g_publish_other = 1;
static time_t g_next_publish_time = 0;
static pthread_mutex_t g_publish_mutex = PTHREAD_MUTEX_INITIALIZER;

// Counter tracking structures
typedef struct {
	char scenario[64];
	int aoa_reference_total;
	int aoa_reference_human;
	int aoa_reference_car;
	int aoa_reference_bike;
	int aoa_reference_bus;
	int aoa_reference_truck;
	int aoa_reference_other;
	int internal_total;
	int internal_human;
	int internal_car;
	int internal_bike;
	int internal_bus;
	int internal_truck;
	int internal_other;
	int has_reference;
} CounterState;

static CounterState g_counters[10];
static int g_counter_count = 0;
static time_t g_last_save_time = 0;
static pthread_mutex_t g_counter_mutex = PTHREAD_MUTEX_INITIALIZER;

// Settings cache
static char g_b100_ip[64] = "10.13.8.47";
static int g_b100_port = 80;
static char g_callback_ip[64] = "";
static int g_callback_port = 80;
static int g_lorawan_port = 10;
static int g_health_check_interval = 60;

// App start time for uptime calculation
static time_t g_app_start_time = 0;

// Connection health-check state (all protected by g_health_mutex)
// Every 10th uplink is sent confirmed to validate the link.
// If no ACK within 4 minutes: send "Hello" on port 7 (up to 3 times).
// After 3 failures: force a rejoin.
static int g_unconf_count = 0;        // counts consecutive unconfirmed uplinks
static int g_awaiting_confirm = 0;    // 1 while waiting for a confirmed ACK
static int g_conf_trial_count = 0;    // number of Hello-probe retries attempted
static time_t g_conf_sent_time = 0;   // when the last confirmed uplink was sent
static pthread_mutex_t g_health_mutex = PTHREAD_MUTEX_INITIALIZER;

// ==================================================================
// Counter Management
// ==================================================================

CounterState* Find_Or_Create_Counter(const char* scenario) {
	pthread_mutex_lock(&g_counter_mutex);
	
	// Find existing counter
	for (int i = 0; i < g_counter_count; i++) {
		if (strcmp(g_counters[i].scenario, scenario) == 0) {
			pthread_mutex_unlock(&g_counter_mutex);
			return &g_counters[i];
		}
	}
	
	// Create new counter if space available
	if (g_counter_count < 10) {
		CounterState* counter = &g_counters[g_counter_count];
		memset(counter, 0, sizeof(CounterState));
		strncpy(counter->scenario, scenario, sizeof(counter->scenario) - 1);
		g_counter_count++;
		LOG("Created new counter for scenario: %s\n", scenario);
		pthread_mutex_unlock(&g_counter_mutex);
		return counter;
	}
	
	pthread_mutex_unlock(&g_counter_mutex);
	LOG_WARN("Too many counters, cannot create for %s\n", scenario);
	return NULL;
}

void Load_Counters_From_File() {
	pthread_mutex_lock(&g_counter_mutex);
	
	cJSON* data = ACAP_FILE_Read("localdata/counters.json");
	if (!data) {
		LOG("No saved counters found, starting fresh\n");
		pthread_mutex_unlock(&g_counter_mutex);
		return;
	}
	
	g_counter_count = 0;
	cJSON* counter_item = data->child;
	while (counter_item && g_counter_count < 10) {
		CounterState* counter = &g_counters[g_counter_count];
		memset(counter, 0, sizeof(CounterState));
		
		cJSON* scenario = cJSON_GetObjectItem(counter_item, "scenario");
		if (scenario && scenario->valuestring) {
			strncpy(counter->scenario, scenario->valuestring, sizeof(counter->scenario) - 1);
		}
		
		cJSON* total = cJSON_GetObjectItem(counter_item, "total");
		if (total) counter->internal_total = total->valueint;
		
		cJSON* human = cJSON_GetObjectItem(counter_item, "human");
		if (human) counter->internal_human = human->valueint;
		
		cJSON* car = cJSON_GetObjectItem(counter_item, "car");
		if (car) counter->internal_car = car->valueint;
		
		cJSON* bike = cJSON_GetObjectItem(counter_item, "bike");
		if (bike) counter->internal_bike = bike->valueint;
		
		cJSON* bus = cJSON_GetObjectItem(counter_item, "bus");
		if (bus) counter->internal_bus = bus->valueint;
		
		cJSON* truck = cJSON_GetObjectItem(counter_item, "truck");
		if (truck) counter->internal_truck = truck->valueint;
		
		cJSON* other = cJSON_GetObjectItem(counter_item, "other");
		if (other) counter->internal_other = other->valueint;
		
		counter->has_reference = 0;  // Will be set on first event
		
		LOG("Loaded counter %s: total=%d, human=%d, car=%d\n", 
		    counter->scenario, counter->internal_total, counter->internal_human, counter->internal_car);
		
		g_counter_count++;
		counter_item = counter_item->next;
	}
	
	cJSON_Delete(data);
	pthread_mutex_unlock(&g_counter_mutex);
	LOG("Loaded %d counters from file\n", g_counter_count);
}

void Save_Counters_To_File() {
	pthread_mutex_lock(&g_counter_mutex);
	
	LOG("Saving %d counters to file...\n", g_counter_count);
	
	cJSON* root = cJSON_CreateArray();
	
	for (int i = 0; i < g_counter_count; i++) {
		CounterState* counter = &g_counters[i];
		LOG("  - %s (total=%d)\n", counter->scenario, counter->internal_total);
		cJSON* item = cJSON_CreateObject();
		
		cJSON_AddStringToObject(item, "scenario", counter->scenario);
		cJSON_AddNumberToObject(item, "total", counter->internal_total);
		cJSON_AddNumberToObject(item, "human", counter->internal_human);
		cJSON_AddNumberToObject(item, "car", counter->internal_car);
		cJSON_AddNumberToObject(item, "bike", counter->internal_bike);
		cJSON_AddNumberToObject(item, "bus", counter->internal_bus);
		cJSON_AddNumberToObject(item, "truck", counter->internal_truck);
		cJSON_AddNumberToObject(item, "other", counter->internal_other);
		
		cJSON_AddItemToArray(root, item);
	}
	
	ACAP_FILE_Write("localdata/counters.json", root);
	cJSON_Delete(root);
	
	pthread_mutex_unlock(&g_counter_mutex);
	g_last_save_time = time(NULL);
	LOG("Counters saved to localdata/counters.json\n");
}

void Delete_Counter_By_Scenario(const char* scenario) {
	pthread_mutex_lock(&g_counter_mutex);
	
	int found_index = -1;
	for (int i = 0; i < g_counter_count; i++) {
		if (strcmp(g_counters[i].scenario, scenario) == 0) {
			found_index = i;
			break;
		}
	}
	
	if (found_index >= 0) {
		LOG("Deleting counter: %s\n", scenario);
		
		// Shift remaining counters down
		for (int i = found_index; i < g_counter_count - 1; i++) {
			memcpy(&g_counters[i], &g_counters[i + 1], sizeof(CounterState));
		}
		
		g_counter_count--;
		
		// Clear the last slot
		memset(&g_counters[g_counter_count], 0, sizeof(CounterState));
		
		pthread_mutex_unlock(&g_counter_mutex);
		
		// Save updated counters
		Save_Counters_To_File();
	} else {
		pthread_mutex_unlock(&g_counter_mutex);
		LOG_WARN("Counter not found for deletion: %s\n", scenario);
	}
}

void Sync_Counters_With_AOA_List(cJSON* scenario_array) {
	if (!scenario_array || !cJSON_IsArray(scenario_array)) {
		LOG_WARN("Invalid scenario array for sync\n");
		return;
	}
	
	pthread_mutex_lock(&g_counter_mutex);
	
	// Build list of scenarios to keep
	int keep_count = 0;
	char* keep_list[10];
	
	cJSON* item = scenario_array->child;
	while (item && keep_count < 10) {
		if (cJSON_IsString(item) && item->valuestring) {
			keep_list[keep_count] = item->valuestring;
			keep_count++;
		}
		item = item->next;
	}
	
	// Check each counter - remove if not in keep list
	int removed = 0;
	for (int i = g_counter_count - 1; i >= 0; i--) {
		int should_keep = 0;
		for (int j = 0; j < keep_count; j++) {
			if (strcmp(g_counters[i].scenario, keep_list[j]) == 0) {
				should_keep = 1;
				break;
			}
		}
		
		if (!should_keep) {
			LOG("Sync: Removing counter '%s' (not in AOA)\n", g_counters[i].scenario);
			
			// Shift remaining counters down
			for (int j = i; j < g_counter_count - 1; j++) {
				memcpy(&g_counters[j], &g_counters[j + 1], sizeof(CounterState));
			}
			
			g_counter_count--;
			removed++;
		}
	}
	
	pthread_mutex_unlock(&g_counter_mutex);
	
	if (removed > 0) {
		LOG("Sync: Removed %d counter(s), saving to file...\n", removed);
		Save_Counters_To_File();
		LOG("Sync: File saved with %d counters remaining\n", g_counter_count);
	} else {
		LOG("Sync: All counters match AOA configuration (%d counters)\n", g_counter_count);
	}
}

// ==================================================================
// Event Callback
// ==================================================================

void
AOA_Event_Callback(cJSON *event, void* userdata) {

	// Only process CrosslineCounting events
	cJSON* scenarioType = cJSON_GetObjectItem(event, "scenarioType");
	if (!scenarioType || !scenarioType->valuestring || 
	    strcmp(scenarioType->valuestring, "CrosslineCounting") != 0) {
		return;  // Ignore non-counter events
	}
	
	// Extract scenario name
	cJSON* scenario_name = cJSON_GetObjectItem(event, "scenario");
	if (!scenario_name || !scenario_name->valuestring) {
		LOG_WARN("CrosslineCounting event missing scenario name\n");
		return;
	}
	
	// Get counter state
	CounterState* counter = Find_Or_Create_Counter(scenario_name->valuestring);
	if (!counter) {
		return;
	}
	
	pthread_mutex_lock(&g_counter_mutex);
	
	// Extract AOA counts
	int aoa_total = 0, aoa_human = 0, aoa_car = 0, aoa_bike = 0;
	int aoa_bus = 0, aoa_truck = 0, aoa_other = 0;
	
	cJSON* total = cJSON_GetObjectItem(event, "total");
	if (total) aoa_total = total->valueint;
	
	cJSON* totalHuman = cJSON_GetObjectItem(event, "totalHuman");
	if (totalHuman) aoa_human = totalHuman->valueint;
	
	cJSON* totalCar = cJSON_GetObjectItem(event, "totalCar");
	if (totalCar) aoa_car = totalCar->valueint;
	
	cJSON* totalBike = cJSON_GetObjectItem(event, "totalBike");
	if (totalBike) aoa_bike = totalBike->valueint;
	
	cJSON* totalBus = cJSON_GetObjectItem(event, "totalBus");
	if (totalBus) aoa_bus = totalBus->valueint;
	
	cJSON* totalTruck = cJSON_GetObjectItem(event, "totalTruck");
	if (totalTruck) aoa_truck = totalTruck->valueint;
	
	cJSON* totalOther = cJSON_GetObjectItem(event, "totalOtherVehicle");
	if (totalOther) aoa_other = totalOther->valueint;
	
	// Get reason (what triggered this count)
	cJSON* reason = cJSON_GetObjectItem(event, "reason");
	const char* reason_str = reason && reason->valuestring ? reason->valuestring : "unknown";
	
	if (!counter->has_reference) {
		// First event - set reference values
		counter->aoa_reference_total = aoa_total;
		counter->aoa_reference_human = aoa_human;
		counter->aoa_reference_car = aoa_car;
		counter->aoa_reference_bike = aoa_bike;
		counter->aoa_reference_bus = aoa_bus;
		counter->aoa_reference_truck = aoa_truck;
		counter->aoa_reference_other = aoa_other;
		counter->has_reference = 1;
		
		LOG("Set reference for %s: total=%d (reason: %s)\n", 
		    counter->scenario, aoa_total, reason_str);
	} else {
		// Calculate deltas
		int delta_total = aoa_total - counter->aoa_reference_total;
		int delta_human = aoa_human - counter->aoa_reference_human;
		int delta_car = aoa_car - counter->aoa_reference_car;
		int delta_bike = aoa_bike - counter->aoa_reference_bike;
		int delta_bus = aoa_bus - counter->aoa_reference_bus;
		int delta_truck = aoa_truck - counter->aoa_reference_truck;
		int delta_other = aoa_other - counter->aoa_reference_other;
		
		// Check if AOA counters were reset (negative delta)
		if (delta_total < 0 || delta_human < 0 || delta_car < 0 || 
		    delta_bike < 0 || delta_bus < 0 || delta_truck < 0 || delta_other < 0) {
			LOG_WARN("%s: AOA counter reset detected (delta=%d), updating reference\n",
			         counter->scenario, delta_total);
			
			// Update references without incrementing internal counters
			counter->aoa_reference_total = aoa_total;
			counter->aoa_reference_human = aoa_human;
			counter->aoa_reference_car = aoa_car;
			counter->aoa_reference_bike = aoa_bike;
			counter->aoa_reference_bus = aoa_bus;
			counter->aoa_reference_truck = aoa_truck;
			counter->aoa_reference_other = aoa_other;
		} else if (delta_total > 0) {
			// Normal case - update internal counters
			counter->internal_total += delta_total;
			counter->internal_human += delta_human;
			counter->internal_car += delta_car;
			counter->internal_bike += delta_bike;
			counter->internal_bus += delta_bus;
			counter->internal_truck += delta_truck;
			counter->internal_other += delta_other;
			
			// Update references
			counter->aoa_reference_total = aoa_total;
			counter->aoa_reference_human = aoa_human;
			counter->aoa_reference_car = aoa_car;
			counter->aoa_reference_bike = aoa_bike;
			counter->aoa_reference_bus = aoa_bus;
			counter->aoa_reference_truck = aoa_truck;
			counter->aoa_reference_other = aoa_other;
			
			LOG("%s: +%d %s (internal: %d total, %d human, %d car)\n",
			    counter->scenario, delta_total, reason_str,
			    counter->internal_total, counter->internal_human, counter->internal_car);
			
			// Auto-save if it's been more than 1 minute
			time_t now = time(NULL);
			if (now - g_last_save_time >= 60) {
				pthread_mutex_unlock(&g_counter_mutex);
				Save_Counters_To_File();
				return;
			}
		}
		// If delta is 0, ignore (no change)
	}
	
	pthread_mutex_unlock(&g_counter_mutex);
}

// ==================================================================
// Settings Callback
// ==================================================================

void
Settings_Updated_Callback( const char* service, cJSON* data) {
	char* _dbg = cJSON_PrintUnformatted(data);
	if (_dbg) { LOG_TRACE("Settings_Updated_Callback [%s]: %s\n", service, _dbg); free(_dbg); }

	// The ACAP framework calls this with the individual group name as service
	// (e.g. "b100", "lorawan", "transmission", "polling") and data is the
	// group object directly — NOT the top-level settings wrapper.

	if (strcmp(service, "b100") == 0) {
		cJSON* ip = cJSON_GetObjectItem(data, "ip");
		if (ip && ip->valuestring) {
			strncpy(g_b100_ip, ip->valuestring, sizeof(g_b100_ip) - 1);
			B100_Set_IP(g_b100_ip);
		}
		cJSON* port = cJSON_GetObjectItem(data, "port");
		if (port) {
			g_b100_port = port->valueint;
			B100_Set_Port(g_b100_port);
		}
		cJSON* timeout = cJSON_GetObjectItem(data, "timeout");
		if (timeout) {
			B100_Set_Timeout(timeout->valueint);
		}
		cJSON* cbip = cJSON_GetObjectItem(data, "callbackIP");
		if (cbip && cbip->valuestring) {
			strncpy(g_callback_ip, cbip->valuestring, sizeof(g_callback_ip) - 1);
			g_callback_ip[sizeof(g_callback_ip) - 1] = '\0';
		}
		cJSON* cbport = cJSON_GetObjectItem(data, "callbackPort");
		if (cbport && cbport->valueint > 0)
			g_callback_port = cbport->valueint;
		// Force callback re-configuration on next health cycle so the
		// new B100 IP / callback address takes effect immediately.
		g_callbacks_configured = 0;
	}

	if (strcmp(service, "lorawan") == 0) {
		cJSON* port = cJSON_GetObjectItem(data, "port");
		if (port) {
			g_lorawan_port = port->valueint;
			if (g_lorawan_port < 1) g_lorawan_port = 1;
			if (g_lorawan_port > 223) g_lorawan_port = 223;
			LOG("Settings: LoRaWAN port updated to %d\n", g_lorawan_port);
		}
	}

	if (strcmp(service, "transmission") == 0) {
		cJSON* interval = cJSON_GetObjectItem(data, "intervalMinutes");
		if (interval) {
			pthread_mutex_lock(&g_publish_mutex);
			g_publish_interval_minutes = interval->valueint;
			if (g_publish_interval_minutes < 1) g_publish_interval_minutes = 1;
			if (g_publish_interval_minutes > 60) g_publish_interval_minutes = 60;
			pthread_mutex_unlock(&g_publish_mutex);
		}
		cJSON* enabled = cJSON_GetObjectItem(data, "enabled");
		if (enabled) {
			pthread_mutex_lock(&g_publish_mutex);
			g_publish_enabled = cJSON_IsTrue(enabled);
			pthread_mutex_unlock(&g_publish_mutex);
		}
		cJSON* classes = cJSON_GetObjectItem(data, "classes");
		if (classes) {
			pthread_mutex_lock(&g_publish_mutex);
			cJSON* human = cJSON_GetObjectItem(classes, "human");
			if (human) g_publish_human = cJSON_IsTrue(human);
			cJSON* car = cJSON_GetObjectItem(classes, "car");
			if (car) g_publish_car = cJSON_IsTrue(car);
			cJSON* bike = cJSON_GetObjectItem(classes, "bike");
			if (bike) g_publish_bike = cJSON_IsTrue(bike);
			cJSON* bus = cJSON_GetObjectItem(classes, "bus");
			if (bus) g_publish_bus = cJSON_IsTrue(bus);
			cJSON* truck = cJSON_GetObjectItem(classes, "truck");
			if (truck) g_publish_truck = cJSON_IsTrue(truck);
			cJSON* other = cJSON_GetObjectItem(classes, "other");
			if (other) g_publish_other = cJSON_IsTrue(other);
			pthread_mutex_unlock(&g_publish_mutex);
		}
	}

	if (strcmp(service, "polling") == 0) {
		cJSON* health_interval = cJSON_GetObjectItem(data, "healthCheckIntervalSeconds");
		if (health_interval) {
			g_health_check_interval = health_interval->valueint;
		}
	}
	// downlinkCommands is read directly from settings at dispatch time; no caching needed.
}

// ==================================================================
// B100 Callbacks
// ==================================================================

// Forward declaration — defined further down in this file
static int Publish_Counters_To_LoRa(void);

void
B100_Downlink_Handler(B100_Downlink* downlink) {
	if (!downlink) return;

	syslog(LOG_WARNING, "LoRa Downlink: port=%d len=%d type=%s payload=%s RSSI=%.1f SNR=%.1f fcntDown=%d",
	    downlink->port, downlink->length, downlink->payload_type,
	    downlink->payload, downlink->rssi, downlink->snr, downlink->fcntDown);
	LOG("LoRa Downlink: port=%d len=%d type=%s payload=%s RSSI=%.1f SNR=%.1f fcntDown=%d\n",
	    downlink->port, downlink->length, downlink->payload_type,
	    downlink->payload, downlink->rssi, downlink->snr, downlink->fcntDown);

	// Update status — read by downlink.html via the 'status' endpoint
	ACAP_STATUS_SetString("lorawan", "lastDownlink", downlink->payload);
	ACAP_STATUS_SetString("lorawan", "lastDownlinkType", downlink->payload_type);
	ACAP_STATUS_SetNumber("lorawan", "lastDownlinkPort", downlink->port);
	ACAP_STATUS_SetNumber("lorawan", "lastDownlinkLength", downlink->length);
	ACAP_STATUS_SetNumber("lorawan", "lastDownlinkRSSI", downlink->rssi);
	ACAP_STATUS_SetNumber("lorawan", "lastDownlinkSNR", downlink->snr);
	ACAP_STATUS_SetNumber("lorawan", "lastDownlinkFcnt", downlink->fcntDown);

	// Maintain server-side ring buffer of last DOWNLINK_LOG_MAX downlinks
	{
		cJSON* entry = cJSON_CreateObject();
		if (entry) {
			cJSON_AddStringToObject(entry, "time",         ACAP_DEVICE_Local_Time());
			cJSON_AddNumberToObject(entry, "port",         downlink->port);
			cJSON_AddNumberToObject(entry, "length",       downlink->length);
			cJSON_AddStringToObject(entry, "payload",      downlink->payload);
			cJSON_AddStringToObject(entry, "payload_type", downlink->payload_type);
			cJSON_AddNumberToObject(entry, "rssi",         downlink->rssi);
			cJSON_AddNumberToObject(entry, "snr",          downlink->snr);
			cJSON_AddNumberToObject(entry, "fcntDown",     downlink->fcntDown);
			if (!g_downlink_log)
				g_downlink_log = cJSON_CreateArray();
			cJSON_AddItemToArray(g_downlink_log, entry);
			while (cJSON_GetArraySize(g_downlink_log) > DOWNLINK_LOG_MAX)
				cJSON_DeleteItemFromArray(g_downlink_log, 0);
			ACAP_STATUS_SetObject("lorawan", "downlinks", g_downlink_log);
		}
	}

	// -----------------------------------------------------------
	// Parse payload into a byte array for command dispatch
	// -----------------------------------------------------------
	unsigned char bytes[256] = {0};
	int byte_count = 0;

	if (strcmp(downlink->payload_type, "HEX") == 0) {
		const char* hex = downlink->payload;
		int hex_len = (int)strlen(hex);
		for (int i = 0; i + 1 < hex_len && byte_count < (int)sizeof(bytes); i += 2) {
			char pair[3] = { hex[i], hex[i+1], '\0' };
			bytes[byte_count++] = (unsigned char)strtol(pair, NULL, 16);
		}
	} else {
		byte_count = (int)strlen(downlink->payload);
		if (byte_count > (int)sizeof(bytes) - 1)
			byte_count = (int)sizeof(bytes) - 1;
		memcpy(bytes, downlink->payload, byte_count);
	}

	if (byte_count == 0) return;

	int port       = downlink->port;
	int first_byte = (int)bytes[0];

	// -----------------------------------------------------------
	// Check whether this command is enabled in settings
	// -----------------------------------------------------------
	int command_enabled = 1; // default: enabled if not listed
	cJSON* settings_obj = ACAP_Get_Config("settings");
	if (settings_obj) {
		cJSON* cmd_list = cJSON_GetObjectItem(settings_obj, "downlinkCommands");
		if (cmd_list && cJSON_IsArray(cmd_list)) {
			cJSON* cmd;
			int lookup_byte = first_byte;
			cJSON_ArrayForEach(cmd, cmd_list) {
				cJSON* p = cJSON_GetObjectItem(cmd, "port");
				cJSON* b = cJSON_GetObjectItem(cmd, "byte");
				if (p && b && p->valueint == port && b->valueint == lookup_byte) {
					cJSON* e = cJSON_GetObjectItem(cmd, "enabled");
					command_enabled = (!e || cJSON_IsTrue(e)) ? 1 : 0;
					break;
				}
			}
		}
	}

	if (!command_enabled) {
		LOG("Downlink command port=%d byte=0x%02X disabled in settings — ignored\n", port, first_byte);
		return;
	}

	// -----------------------------------------------------------
	// Dispatch
	// -----------------------------------------------------------
	if (port == 10) {
		// Port 10: Actions
		switch (first_byte) {
			case 0x01:
				LOG("Downlink: Restart Bridge\n");
				B100_Restart();
				break;
			case 0x02:
				LOG("Downlink: Join Network\n");
				B100_Join_Auto();
				break;
			case 0x03:
				LOG("Downlink: Reset Counters\n");
				pthread_mutex_lock(&g_counter_mutex);
				for (int i = 0; i < g_counter_count; i++) {
					g_counters[i].internal_total = 0;
					g_counters[i].internal_human = 0;
					g_counters[i].internal_car   = 0;
					g_counters[i].internal_bike  = 0;
					g_counters[i].internal_bus   = 0;
					g_counters[i].internal_truck = 0;
					g_counters[i].internal_other = 0;
					g_counters[i].has_reference  = 0;
				}
				pthread_mutex_unlock(&g_counter_mutex);
				Save_Counters_To_File();
				sleep(1);
				Publish_Counters_To_LoRa();
				break;
			default:
				LOG_WARN("Downlink: Unknown port 10 command 0x%02X\n", first_byte);
				break;
		}

	} else if (port == 11) {
		// Port 11: Configuration (2-byte: byte[0]=command, byte[1]=value)
		if (byte_count < 2) {
			LOG_WARN("Downlink: Port 11 requires 2 bytes, got %d\n", byte_count);
		} else {
			int value = (int)bytes[1];
			switch (first_byte) {
				case 0x01: {
					int minutes = value;
					if (minutes < 5)  minutes = 5;
					if (minutes > 60) minutes = 60;
					LOG("Downlink: Set publish interval to %d minutes\n", minutes);
					pthread_mutex_lock(&g_publish_mutex);
					g_publish_interval_minutes = minutes;
					pthread_mutex_unlock(&g_publish_mutex);
					if (settings_obj) {
						cJSON* trans = cJSON_GetObjectItem(settings_obj, "transmission");
						if (trans) {
							cJSON* iv = cJSON_GetObjectItem(trans, "intervalMinutes");
							if (iv) { iv->valueint = minutes; iv->valuedouble = minutes; }
							ACAP_FILE_Write("localdata/settings.json", settings_obj);
						}
					}
					break;
				}
				case 0x02: {
					int dr = value;
					if (dr >= 0 && dr <= 5) {
						LOG("Downlink: Set Data Rate DR%d\n", dr);
						cJSON* p = cJSON_CreateObject();
						cJSON_AddNumberToObject(p, "data_rate", dr);
						B100_Set_Params(p);
						cJSON_Delete(p);
					} else {
						LOG_WARN("Downlink: Set Data Rate bad value %d\n", dr);
					}
					break;
				}
				case 0x03: {
					int adr = value ? 1 : 0;
					LOG("Downlink: Set ADR %s\n", adr ? "on" : "off");
					cJSON* p = cJSON_CreateObject();
					cJSON_AddNumberToObject(p, "adr_enable", adr);
					B100_Set_Params(p);
					cJSON_Delete(p);
					break;
				}
				default:
					LOG_WARN("Downlink: Unknown port 11 command 0x%02X\n", first_byte);
					break;
			}
		}

	} else if (port == 12) {
		// Port 12: Information requests
		switch (first_byte) {
			case 0x01: {
				// Camera identity + health → reply on port 5
				const char* model    = ACAP_DEVICE_Prop("model");
				const char* serial   = ACAP_DEVICE_Prop("serial");
				const char* firmware = ACAP_DEVICE_Prop("firmware");
				int uptime_hours = (int)(ACAP_DEVICE_Uptime() / 3600.0);
				int cpu_pct = (int)(ACAP_DEVICE_CPU_Average() * 100.0);
				if (cpu_pct > 99) cpu_pct = 99;
				const char* app_ver = "?";
				cJSON* mf = ACAP_Get_Config("manifest");
				if (mf) {
					cJSON* setup = cJSON_GetObjectItem(cJSON_GetObjectItem(mf, "acapPackageConf"), "setup");
					if (setup) {
						cJSON* ver = cJSON_GetObjectItem(setup, "version");
						if (ver && ver->valuestring) app_ver = ver->valuestring;
					}
				}
				char info[192] = {0};
				snprintf(info, sizeof(info), "%s,%s,%s,%dh,%d%%,%s",
				         model    ? model    : "?",
				         serial   ? serial   : "?",
				         firmware ? firmware : "?",
				         uptime_hours, cpu_pct, app_ver);
				LOG("Downlink: Camera Info reply: %s\n", info);
				if (!B100_Send(info, 5, 0))
					LOG_WARN("Downlink: Camera Info send failed: %s\n", B100_Get_Last_Error());
				break;
			}
			case 0x02: {
				// Bridge identity + LoRaWAN state → reply on port 6
				B100_Status* s = B100_Get_Status();
				char info[256] = {0};
				snprintf(info, sizeof(info), "%s/%s,%s,%s,%.0fC,R%u,%s",
				         s->hardware[0]        ? s->hardware        : "?",
				         s->hardwareVersion[0] ? s->hardwareVersion : "?",
				         s->firmwareVersion[0] ? s->firmwareVersion : "?",
				         s->powerSource[0]     ? s->powerSource     : "?",
				         s->tempC,
				         s->restartCounter,
				         s->devAddrStr[0]      ? s->devAddrStr      : "0");
				LOG("Downlink: Bridge Info reply: %s\n", info);
				if (!B100_Send(info, 6, 0))
					LOG_WARN("Downlink: Bridge Info send failed: %s\n", B100_Get_Last_Error());
				ACAP_STATUS_SetString("lorawan", "bridgeInfo", info);
				break;
			}
			case 0x03: {
				// Signal quality snapshot → reply on port 7
				B100_Status* s = B100_Get_Status();
				char info[128] = {0};
				snprintf(info, sizeof(info), "DR%d,%dB,%.0fdBm,%.1fdB,%uup,%udn",
				         s->dataRate, s->maxPayload,
				         s->rssi, s->snr,
				         s->fcntUp, s->fcntDown);
				LOG("Downlink: Signal Quality reply: %s\n", info);
				if (!B100_Send(info, 7, 0))
					LOG_WARN("Downlink: Signal Quality send failed: %s\n", B100_Get_Last_Error());
				break;
			}
			default:
				LOG_WARN("Downlink: Unknown port 12 command 0x%02X\n", first_byte);
				break;
		}

	} else {
		LOG_WARN("Downlink: Unhandled port %d\n", port);
	}
}

// Push bridge device-level info to the "bridge" ACAP_STATUS group.
// Called after a successful /info fetch and on every status callback.
static void
Update_Bridge_ACAP_Status(B100_Status* status) {
	if (!status) return;
	ACAP_STATUS_SetBool("bridge", "connected", status->connected == B100_CONNECTED);
	if (status->hardware[0])
		ACAP_STATUS_SetString("bridge", "hardware", status->hardware);
	if (status->hardwareVersion[0])
		ACAP_STATUS_SetString("bridge", "hardwareVersion", status->hardwareVersion);
	if (status->firmwareVersion[0])
		ACAP_STATUS_SetString("bridge", "firmwareVersion", status->firmwareVersion);
	if (status->powerSource[0])
		ACAP_STATUS_SetString("bridge", "powerSource", status->powerSource);
	if (status->ipAddr[0])
		ACAP_STATUS_SetString("bridge", "ipAddr", status->ipAddr);
	ACAP_STATUS_SetBool("bridge", "dhcpEnabled", status->dhcpEnabled);
	ACAP_STATUS_SetBool("bridge", "mqttEnabled", status->mqttEnabled);
	ACAP_STATUS_SetBool("bridge", "httpApiEnabled", status->httpApiEnabled);
	if (status->devEUI[0])
		ACAP_STATUS_SetString("bridge", "devEUI", status->devEUI);
	if (status->devAddr != 0) {
		char addr_str[16];
		snprintf(addr_str, sizeof(addr_str), "%08X", status->devAddr);
		ACAP_STATUS_SetString("bridge", "devAddr", addr_str);
	} else if (status->devAddrStr[0]) {
		ACAP_STATUS_SetString("bridge", "devAddr", status->devAddrStr);
	}
	ACAP_STATUS_SetNumber("bridge", "restartCounter", status->restartCounter);
	if (status->tempC != 0)
		ACAP_STATUS_SetNumber("bridge", "tempC", status->tempC);
	if (status->tUnix > 0)
		ACAP_STATUS_SetNumber("bridge", "tUnix", (double)status->tUnix);
	ACAP_STATUS_SetNumber("bridge", "tamper", status->tamper);
	ACAP_STATUS_SetNumber("bridge", "gpsStatus", status->gpsStatus);
}

static void
Update_Lorawan_ACAP_Status(B100_Status* status) {
	if (!status) return;
	ACAP_STATUS_SetBool("lorawan", "connected", status->connected == B100_CONNECTED);
	ACAP_STATUS_SetBool("lorawan", "joined", status->joined);
	ACAP_STATUS_SetNumber("lorawan", "statusCode", status->statusCode);
	if (status->statusText[0])
		ACAP_STATUS_SetString("lorawan", "statusText", status->statusText);
	ACAP_STATUS_SetNumber("lorawan", "fcntUp", status->fcntUp);
	ACAP_STATUS_SetNumber("lorawan", "fcntDown", status->fcntDown);
	ACAP_STATUS_SetNumber("lorawan", "rssi", status->rssi);
	ACAP_STATUS_SetNumber("lorawan", "snr", status->snr);
	ACAP_STATUS_SetNumber("lorawan", "dataRate", status->dataRate);
	ACAP_STATUS_SetNumber("lorawan", "maxPayload", status->maxPayload);
	if (status->tUnix > 0)
		ACAP_STATUS_SetNumber("lorawan", "tUnix", (double)status->tUnix);
	ACAP_STATUS_SetNumber("lorawan", "nextUploadMs", (double)status->nextUploadMs);
	if (status->receiveTUnix > 0)
		ACAP_STATUS_SetNumber("lorawan", "downlinkTUnix", (double)status->receiveTUnix);
	ACAP_STATUS_SetNumber("lorawan", "margin", status->margin);
	ACAP_STATUS_SetNumber("lorawan", "gwCount", status->gwCount);
	if (status->devAddr != 0) {
		char addr_str[16];
		snprintf(addr_str, sizeof(addr_str), "%08X", status->devAddr);
		ACAP_STATUS_SetString("lorawan", "devAddr", addr_str);
	} else if (status->devAddrStr[0]) {
		ACAP_STATUS_SetString("lorawan", "devAddr", status->devAddrStr);
	}
}

void
B100_Status_Handler(B100_Status* status) {
	if (!status) return;

	Update_Lorawan_ACAP_Status(status);

	// Bridge device info (from /info) — separate group
	Update_Bridge_ACAP_Status(status);
	ACAP_STATUS_SetBool("bridge", "callbacksActive", g_callbacks_configured);
}

// ==================================================================
// Background Threads
// ==================================================================

void*
Health_Monitor_Thread(void* arg) {
	LOG("Health monitor thread started (interval: %ds)\n", g_health_check_interval);

	while (running) {
		// Refresh device info periodically via /info (always available)
		B100_Fetch_Device_Info();
		B100_Status* status = B100_Get_Status();

		if (status->connected != B100_CONNECTED) {
			LOG_WARN("Health check: B100 not reachable\n");
			ACAP_STATUS_SetBool("bridge", "connected", 0);
			ACAP_STATUS_SetBool("lorawan", "connected", 0);
			ACAP_STATUS_SetString("app", "status", "B100 connection error");
			goto sleep_and_continue;
		}

		// Push device info to bridge status group, and all available lorawan stats
		Update_Bridge_ACAP_Status(status);
		Update_Lorawan_ACAP_Status(status);
		ACAP_STATUS_SetBool("bridge", "callbacksActive", g_callbacks_configured);
		ACAP_STATUS_SetBool("lorawan", "connected", 1);

		// If callbacks are not yet configured, or if B100 reports http_api_enable=0
		// (e.g. after a B100 restart or external reconfiguration), reapply.
		if (!g_callbacks_configured || !status->httpApiEnabled) {
			if (g_callbacks_configured && !status->httpApiEnabled)
				LOG_WARN("B100 http_api_enable is 0 — reapplying callback configuration\n");
			g_callbacks_configured = 0;
			const char* cam_ip = g_callback_ip[0] ? g_callback_ip : ACAP_DEVICE_Prop("IPv4");
			char status_uri[64], receive_uri[64];
			snprintf(status_uri, sizeof(status_uri), "/local/%s/b100_status", APP_PACKAGE);
			snprintf(receive_uri, sizeof(receive_uri), "/local/%s/b100_receive", APP_PACKAGE);
			if (B100_Configure_Callbacks(cam_ip, g_callback_port, status_uri, receive_uri)) {
				g_callbacks_configured = 1;
				// Also re-apply GPS callback so GPS push stays active
				char gps_uri[64];
				snprintf(gps_uri, sizeof(gps_uri), "/local/%s/b100_gps", APP_PACKAGE);
				B100_Configure_GPS_Callback(gps_uri, 60);
				ACAP_STATUS_SetString("app", "status", "Running");
			} else {
				LOG_WARN("Failed to configure B100 callbacks\n");
			}
		}

		// Poll current GPS data and update ACAP status (also fires the GPS handler)
		{
			cJSON* gps_json = B100_Get_GPS();
			if (gps_json) cJSON_Delete(gps_json);
		}

		// Trigger async status request (response comes via callback)
		if (g_callbacks_configured) {
			if (!B100_Request_Status()) {
				LOG_WARN("Health check: status request failed\n");
			}
		}

		// Device restart detected → trigger immediate rejoin
		if (status->statusCode == B100_STATUS_RESTARTED ||
		    status->statusCode == B100_STATUS_AUTOJOIN_ENABLED) {
			LOG("Device restart detected (status %d), triggering auto-join...\n", status->statusCode);
			ACAP_STATUS_SetString("lorawan", "statusText", "Device restarted - rejoining");
			B100_Join_Auto();
			pthread_mutex_lock(&g_health_mutex);
			g_unconf_count = 0;
			g_awaiting_confirm = 0;
			g_conf_trial_count = 0;
			pthread_mutex_unlock(&g_health_mutex);
			goto sleep_and_continue;
		}

		// Confirmed delivery (status 10) → clear health-check state
		if (status->statusCode == B100_STATUS_SENT_CONFIRMED) {
			pthread_mutex_lock(&g_health_mutex);
			if (g_awaiting_confirm) {
				LOG("LoRa: Confirmed delivery received, link healthy\n");
				g_awaiting_confirm = 0;
				g_conf_trial_count = 0;
			}
			pthread_mutex_unlock(&g_health_mutex);
		}

		// Auto-join if not on network
		if (!status->joined) {
			cJSON* settings = ACAP_Get_Config("settings");
			if (settings) {
				cJSON* lorawan = cJSON_GetObjectItem(settings, "lorawan");
				if (lorawan) {
					cJSON* autoJoin = cJSON_GetObjectItem(lorawan, "autoJoin");
					if (autoJoin && cJSON_IsTrue(autoJoin)) {
						LOG("Auto-join: Device not joined, attempting join...\n");
						B100_Join_Auto();
					}
				}
			}
			goto sleep_and_continue;
		}

		// Confirmation timeout: if a confirmed uplink got no ACK within 4 minutes,
		// probe with "Hello" on port 7 (up to 3 retries), then force a rejoin.
		pthread_mutex_lock(&g_health_mutex);
		int waiting = g_awaiting_confirm;
		int trials  = g_conf_trial_count;
		time_t sent = g_conf_sent_time;
		pthread_mutex_unlock(&g_health_mutex);

		if (waiting && (time(NULL) - sent) > 240) {
			if (trials < 3) {
				LOG("Confirmation timeout - sending Hello probe %d/3 on port 7\n", trials + 1);
				B100_Send("Hello", 7, 1);
				pthread_mutex_lock(&g_health_mutex);
				g_conf_trial_count++;
				g_conf_sent_time = time(NULL);
				pthread_mutex_unlock(&g_health_mutex);
			} else {
				LOG("3 confirmation failures - forcing rejoin\n");
				ACAP_STATUS_SetString("lorawan", "statusText", "Link lost - rejoining");
				B100_Join_Auto();
				pthread_mutex_lock(&g_health_mutex);
				g_awaiting_confirm = 0;
				g_conf_trial_count = 0;
				g_unconf_count = 0;
				pthread_mutex_unlock(&g_health_mutex);
			}
		}

	sleep_and_continue:
		for (int i = 0; i < g_health_check_interval && running; i++) {
			sleep(1);
		}
	}

	LOG("Health monitor thread stopped\n");
	return NULL;
}

// Downlink poller removed — downlinks now arrive via B100 receive callback

// ==================================================================
// HTTP Endpoints
// ==================================================================

void
HTTP_Endpoint_join(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method || strcmp(method, "POST") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
        return;
    }
	
	LOG("Manual join requested\n");
	
	// Get optional parameters
	char* drJoin_str = ACAP_HTTP_Request_Param(request, "drJoin");
	char* adr_str = ACAP_HTTP_Request_Param(request, "adr");
	char* drUp_str = ACAP_HTTP_Request_Param(request, "drUp");
	
	int drJoin = drJoin_str ? atoi(drJoin_str) : 0;
	int adr = adr_str ? atoi(adr_str) : 1;
	int drUp = drUp_str ? atoi(drUp_str) : 4;
	
	free(drJoin_str);
	free(adr_str);
	free(drUp_str);
	
	// Attempt join
	if (B100_Join(drJoin, adr, drUp)) {
		ACAP_HTTP_Respond_Text(response, "Join request sent");
	} else {
		ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
	}
}

void
HTTP_Endpoint_test(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method || strcmp(method, "POST") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
        return;
    }
	
	LOG("Connection test requested\n");
	
	if (B100_Test_Connection()) {
		ACAP_HTTP_Respond_Text(response, "Connection successful");
	} else {
		ACAP_HTTP_Respond_Error(response, 503, "Connection failed");
	}
}

void
HTTP_Endpoint_restart(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method || strcmp(method, "POST") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
        return;
    }
	
	LOG("B100 restart requested\n");
	
	if (B100_Restart()) {
		ACAP_HTTP_Respond_Text(response, "Restart initiated");
	} else {
		ACAP_HTTP_Respond_Error(response, 500, "Restart failed");
	}
}

void
HTTP_Endpoint_send(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method || strcmp(method, "POST") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
        return;
    }
	
	// Get parameters
	char* payload = ACAP_HTTP_Request_Param(request, "payload");
	char* port_str = ACAP_HTTP_Request_Param(request, "port");
	char* confirmed_str = ACAP_HTTP_Request_Param(request, "confirmed");
	
	if (!payload) {
		free(port_str);
		free(confirmed_str);
		ACAP_HTTP_Respond_Error(response, 400, "Missing payload parameter");
		return;
	}
	
	int port = port_str ? atoi(port_str) : 10;
	int confirmed = confirmed_str ? atoi(confirmed_str) : 0;
	
	free(port_str);
	free(confirmed_str);
	
	LOG("Sending test message on port %d: %s\n", port, payload);
	
	// Send message
	if (B100_Send(payload, port, confirmed)) {
		ACAP_HTTP_Respond_Text(response, "Message sent");
	} else {
		ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
	}
	
	free(payload);
}

void
HTTP_Endpoint_counters(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	pthread_mutex_lock(&g_counter_mutex);
	
	cJSON* root = cJSON_CreateObject();
	cJSON* counters_array = cJSON_CreateArray();
	
	for (int i = 0; i < g_counter_count; i++) {
		CounterState* counter = &g_counters[i];
		cJSON* item = cJSON_CreateObject();
		
		cJSON_AddStringToObject(item, "scenario", counter->scenario);
		cJSON_AddNumberToObject(item, "total", counter->internal_total);
		cJSON_AddNumberToObject(item, "human", counter->internal_human);
		cJSON_AddNumberToObject(item, "car", counter->internal_car);
		cJSON_AddNumberToObject(item, "bike", counter->internal_bike);
		cJSON_AddNumberToObject(item, "bus", counter->internal_bus);
		cJSON_AddNumberToObject(item, "truck", counter->internal_truck);
		cJSON_AddNumberToObject(item, "other", counter->internal_other);
		
		cJSON_AddItemToArray(counters_array, item);
	}
	
	pthread_mutex_unlock(&g_counter_mutex);
	
	// Add publishing info
	pthread_mutex_lock(&g_publish_mutex);
	cJSON_AddItemToObject(root, "counters", counters_array);
	cJSON_AddBoolToObject(root, "publishEnabled", g_publish_enabled);
	cJSON_AddNumberToObject(root, "publishInterval", g_publish_interval_minutes);
	cJSON_AddNumberToObject(root, "nextPublishTime", (double)g_next_publish_time);
	time_t now = time(NULL);
	int seconds_until_publish = g_next_publish_time > now ? (int)(g_next_publish_time - now) : 0;
	cJSON_AddNumberToObject(root, "secondsUntilPublish", seconds_until_publish);
	pthread_mutex_unlock(&g_publish_mutex);
	
	ACAP_HTTP_Respond_JSON(response, root);
	cJSON_Delete(root);
}

// ==================================================================
// LoRa Publishing — sends binary counter data directly via byte array
// ==================================================================

int Publish_Counters_To_LoRa() {
	pthread_mutex_lock(&g_counter_mutex);
	
	// Check if we have counters
	if (g_counter_count == 0) {
		pthread_mutex_unlock(&g_counter_mutex);
		LOG_WARN("No counters to publish\n");
		return 0;
	}
	
	// Count how many classes are enabled
	pthread_mutex_lock(&g_publish_mutex);
	int class_count = g_publish_human + g_publish_car + g_publish_bike + 
	                  g_publish_bus + g_publish_truck + g_publish_other;
	pthread_mutex_unlock(&g_publish_mutex);
	
	if (class_count == 0) {
		pthread_mutex_unlock(&g_counter_mutex);
		LOG_WARN("No classes selected for publishing\n");
		return 0;
	}
	
	// Calculate buffer size: counter_count * class_count * 2 bytes
	size_t buffer_size = g_counter_count * class_count * 2;
	unsigned char* buffer = malloc(buffer_size);
	if (!buffer) {
		pthread_mutex_unlock(&g_counter_mutex);
		LOG_WARN("Failed to allocate buffer\n");
		return 0;
	}
	
	// Fill buffer with counter values (16-bit unsigned, little-endian)
	size_t offset = 0;
	for (int i = 0; i < g_counter_count; i++) {
		CounterState* counter = &g_counters[i];
		
		pthread_mutex_lock(&g_publish_mutex);
		if (g_publish_human) {
			uint16_t val = counter->internal_human & 0xFFFF;
			buffer[offset++] = val & 0xFF;
			buffer[offset++] = (val >> 8) & 0xFF;
		}
		if (g_publish_car) {
			uint16_t val = counter->internal_car & 0xFFFF;
			buffer[offset++] = val & 0xFF;
			buffer[offset++] = (val >> 8) & 0xFF;
		}
		if (g_publish_bike) {
			uint16_t val = counter->internal_bike & 0xFFFF;
			buffer[offset++] = val & 0xFF;
			buffer[offset++] = (val >> 8) & 0xFF;
		}
		if (g_publish_bus) {
			uint16_t val = counter->internal_bus & 0xFFFF;
			buffer[offset++] = val & 0xFF;
			buffer[offset++] = (val >> 8) & 0xFF;
		}
		if (g_publish_truck) {
			uint16_t val = counter->internal_truck & 0xFFFF;
			buffer[offset++] = val & 0xFF;
			buffer[offset++] = (val >> 8) & 0xFF;
		}
		if (g_publish_other) {
			uint16_t val = counter->internal_other & 0xFFFF;
			buffer[offset++] = val & 0xFF;
			buffer[offset++] = (val >> 8) & 0xFF;
		}
		pthread_mutex_unlock(&g_publish_mutex);
	}
	
	pthread_mutex_unlock(&g_counter_mutex);
	
	// Decide confirmed/unconfirmed: every 10th uplink is confirmed for link health.
	pthread_mutex_lock(&g_health_mutex);
	g_unconf_count++;
	int use_confirmed = (g_unconf_count >= 10) ? 1 : 0;
	if (use_confirmed) {
		g_unconf_count = 0;
		g_awaiting_confirm = 1;
		g_conf_trial_count = 0;
		g_conf_sent_time = time(NULL);
		LOG("LoRa: Sending health-check confirmed uplink\n");
	}
	pthread_mutex_unlock(&g_health_mutex);

	// Send raw bytes directly via the B100 POST API (no base64 needed)
	LOG("LoRa: Publishing %zu bytes (%d counters, %d classes) on port %d%s\n", 
	    buffer_size, g_counter_count, class_count, g_lorawan_port,
	    use_confirmed ? " [confirmed]" : "");

	int success = B100_Send_Bytes(buffer, (int)buffer_size, g_lorawan_port, use_confirmed);
	
	if (success) {
		LOG("LoRa: Publish accepted\n");
		pthread_mutex_lock(&g_publish_mutex);
		g_next_publish_time = time(NULL) + (g_publish_interval_minutes * 60);
		pthread_mutex_unlock(&g_publish_mutex);
	} else {
		LOG_WARN("LoRa: Publish failed - %s\n", B100_Get_Last_Error());
	}
	
	free(buffer);
	return success;
}

void*
Publish_Thread(void* arg) {
	LOG("Publish thread started\n");
	
	// Set initial next publish time
	pthread_mutex_lock(&g_publish_mutex);
	g_next_publish_time = time(NULL) + (g_publish_interval_minutes * 60);
	pthread_mutex_unlock(&g_publish_mutex);
	
	while (running) {
		pthread_mutex_lock(&g_publish_mutex);
		int enabled = g_publish_enabled;
		time_t next_time = g_next_publish_time;
		pthread_mutex_unlock(&g_publish_mutex);
		
		if (enabled) {
			time_t now = time(NULL);
			if (now >= next_time) {
				// Time to publish
				Publish_Counters_To_LoRa();
			}
		}
		
		// Sleep for 10 seconds before checking again
		for (int i = 0; i < 10 && running; i++) {
			sleep(1);
		}
	}
	
	LOG("Publish thread stopped\n");
	return NULL;
}

void
HTTP_Endpoint_publish(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}
	
	LOG("Manual publish requested\n");
	
	if (Publish_Counters_To_LoRa()) {
		ACAP_HTTP_Respond_Text(response, "Counters published");
	} else {
		ACAP_HTTP_Respond_Error(response, 500, "Failed to publish counters");
	}
}

void
HTTP_Endpoint_receive(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	// Manual linkcheck trigger
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}
	
	LOG("Manual linkcheck requested\n");
	
	if (B100_Link_Check()) {
		ACAP_HTTP_Respond_Text(response, "Linkcheck request sent");
	} else {
		ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
	}
}

void
HTTP_Endpoint_b100_request_status(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}

	// Ensure B100 has http_api_enable=1 and correct callback URIs configured
	const char* cam_ip = g_callback_ip[0] ? g_callback_ip : ACAP_DEVICE_Prop("IPv4");
	char status_uri[64], receive_uri[64];
	snprintf(status_uri, sizeof(status_uri), "/local/%s/b100_status", APP_PACKAGE);
	snprintf(receive_uri, sizeof(receive_uri), "/local/%s/b100_receive", APP_PACKAGE);
	if (!B100_Configure_Callbacks(cam_ip, g_callback_port, status_uri, receive_uri)) {
		ACAP_HTTP_Respond_Error(response, 500, "Failed to configure B100 callbacks");
		return;
	}
	g_callbacks_configured = 1;

	if (B100_Request_Status()) {
		ACAP_HTTP_Respond_Text(response, "Status request sent");
	} else {
		ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
	}
}

// ==================================================================
// B100 Callback Endpoints — the B100 POSTs status/downlink data here
// ==================================================================

void
HTTP_Endpoint_B100_Status_Callback(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}
	
	cJSON* body = ACAP_HTTP_Request_JSON(request, NULL);
	if (!body) {
		ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON body");
		return;
	}

	char *json_str = cJSON_PrintUnformatted(body);
	if( json_str ) {
		LOG_TRACE("B100 status callback received JSON: %s\n", json_str);
		free(json_str);
	}

	B100_Process_Status_Callback(body);
	cJSON_Delete(body);

	ACAP_HTTP_Respond_Text(response, "OK");
}

void
HTTP_Endpoint_B100_Receive_Callback(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}

	cJSON* body = ACAP_HTTP_Request_JSON(request, NULL);
	if (!body) {
		LOG_WARN("B100 receive callback: failed to parse JSON body (Content-Type may be wrong)\n");
		ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON body");
		return;
	}

	char *json_str = cJSON_PrintUnformatted(body);
	if (json_str) {
		syslog(LOG_WARNING, "B100 receive callback: %s", json_str);
		LOG("B100 receive callback: %s\n", json_str);
		free(json_str);
	}

	B100_Process_Receive_Callback(body);
	cJSON_Delete(body);

	ACAP_HTTP_Respond_Text(response, "OK");
}

void
B100_GPS_Handler(B100_GPS* gps) {
	if (!gps) return;

	ACAP_STATUS_SetNumber("gps", "gps_status", gps->gps_status);
	ACAP_STATUS_SetString("gps", "ns",          gps->ns);
	ACAP_STATUS_SetNumber("gps", "lat",          gps->lat);
	ACAP_STATUS_SetString("gps", "ew",          gps->ew);
	ACAP_STATUS_SetNumber("gps", "lon",          gps->lon);
	ACAP_STATUS_SetNumber("gps", "alt",          gps->alt);
	ACAP_STATUS_SetNumber("gps", "nosv",         gps->nosv);
	ACAP_STATUS_SetNumber("gps", "pdop",         gps->pdop);
	ACAP_STATUS_SetNumber("gps", "hdop",         gps->hdop);
	ACAP_STATUS_SetNumber("gps", "vdop",         gps->vdop);
	ACAP_STATUS_SetString("gps", "utc",          gps->utc);
	ACAP_STATUS_SetString("gps", "date",         gps->date);
	ACAP_STATUS_SetNumber("gps", "sog",          gps->sog);
	ACAP_STATUS_SetNumber("gps", "cog",          gps->cog);
}

void
HTTP_Endpoint_B100_GPS_Callback(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}

	cJSON* body = ACAP_HTTP_Request_JSON(request, NULL);
	if (!body) {
		ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON body");
		return;
	}

	char* json_str = cJSON_PrintUnformatted(body);
	if (json_str) {
		LOG_TRACE("B100 GPS callback received JSON: %s\n", json_str);
		free(json_str);
	}

	B100_Process_GPS_Callback(body);
	cJSON_Delete(body);

	ACAP_HTTP_Respond_Text(response, "OK");
}

void
HTTP_Endpoint_gps(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	cJSON* gps_json = B100_Get_GPS();
	if (gps_json) {
		ACAP_HTTP_Respond_JSON(response, gps_json);
		cJSON_Delete(gps_json);
	} else {
		ACAP_HTTP_Respond_Error(response, 503, "B100 GPS not available");
	}
}

// ==================================================================
// B100 Info/Config Endpoint — expose B100 device info and params
// ==================================================================

void
HTTP_Endpoint_b100_info(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	cJSON* info = B100_Get_Info();
	if (info) {
		// Add current callback config
		cJSON* cb_params = B100_Get_Params("callback_addr");
		if (cb_params) {
			cJSON* addr = cJSON_GetObjectItem(cb_params, "callback_addr");
			if (addr && addr->valuestring)
				cJSON_AddStringToObject(info, "callback_configured_addr", addr->valuestring);
			cJSON_Delete(cb_params);
		}
		cJSON_AddBoolToObject(info, "callbacks_active", g_callbacks_configured);
		
		ACAP_HTTP_Respond_JSON(response, info);
		cJSON_Delete(info);
	} else {
		ACAP_HTTP_Respond_Error(response, 503, "B100 not reachable");
	}
}

void
HTTP_Endpoint_b100_params(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	
	if (method && strcmp(method, "GET") == 0) {
		// Read all parameters
		cJSON* params = B100_Get_Params(NULL);
		if (params) {
			ACAP_HTTP_Respond_JSON(response, params);
			cJSON_Delete(params);
		} else {
			ACAP_HTTP_Respond_Error(response, 503, "B100 not reachable");
		}
	} else if (method && strcmp(method, "POST") == 0) {
		// Set parameters
		cJSON* body = ACAP_HTTP_Request_JSON(request, NULL);
		if (!body) {
			ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON body");
			return;
		}
		if (B100_Set_Params(body)) {
			cJSON_Delete(body);
			ACAP_HTTP_Respond_Text(response, "Parameters updated");
		} else {
			cJSON_Delete(body);
			ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
		}
	} else {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be GET or POST");
	}
}

void
HTTP_Endpoint_linkcheck(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}
	
	if (B100_Link_Check()) {
		ACAP_HTTP_Respond_Text(response, "Linkcheck request sent");
	} else {
		ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
	}
}

void
HTTP_Endpoint_delete_counter(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}
	
	// Get scenario parameter
	char* scenario = ACAP_HTTP_Request_Param(request, "scenario");
	if (!scenario) {
		ACAP_HTTP_Respond_Error(response, 400, "Missing scenario parameter");
		return;
	}
	
	LOG("Delete counter requested: %s\n", scenario);
	Delete_Counter_By_Scenario(scenario);
	free(scenario);
	
	ACAP_HTTP_Respond_Text(response, "Counter deleted");
}

void
HTTP_Endpoint_sync_counters(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}
	
	// Get JSON body with scenario list
	cJSON* body = ACAP_HTTP_Request_JSON(request, NULL);
	if (!body) {
		ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON body");
		return;
	}
	
	cJSON* scenarios = cJSON_GetObjectItem(body, "scenarios");
	if (!scenarios || !cJSON_IsArray(scenarios)) {
		cJSON_Delete(body);
		ACAP_HTTP_Respond_Error(response, 400, "Missing scenarios array");
		return;
	}
	
	int scenario_count = cJSON_GetArraySize(scenarios);
	LOG("Sync counters requested with %d AOA scenarios\n", scenario_count);
	LOG("Current backend has %d counters before sync\n", g_counter_count);
	
	Sync_Counters_With_AOA_List(scenarios);
	
	cJSON_Delete(body);
	
	// Return current counter count
	cJSON* result = cJSON_CreateObject();
	cJSON_AddNumberToObject(result, "removed", -1);  // Will be calculated in sync
	cJSON_AddNumberToObject(result, "remaining", g_counter_count);
	cJSON_AddStringToObject(result, "status", "synchronized");
	
	ACAP_HTTP_Respond_JSON(response, result);
	cJSON_Delete(result);
}

void
HTTP_Endpoint_translator(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	pthread_mutex_lock(&g_counter_mutex);
	pthread_mutex_lock(&g_publish_mutex);
	
	// Build class list string
	char classes[256] = "";
	int first = 1;
	if (g_publish_human) {
		strcat(classes, first ? "human" : ", human");
		first = 0;
	}
	if (g_publish_car) {
		strcat(classes, first ? "car" : ", car");
		first = 0;
	}
	if (g_publish_bike) {
		strcat(classes, first ? "bike" : ", bike");
		first = 0;
	}
	if (g_publish_bus) {
		strcat(classes, first ? "bus" : ", bus");
		first = 0;
	}
	if (g_publish_truck) {
		strcat(classes, first ? "truck" : ", truck");
		first = 0;
	}
	if (g_publish_other) {
		strcat(classes, first ? "other" : ", other");
		first = 0;
	}
	
	// Build JavaScript decoder function
	char js[8192];
	int pos = 0;
	
	// Build counter names list for documentation
	char counter_names[256] = "";
	for (int i = 0; i < g_counter_count; i++) {
		if (i > 0) strcat(counter_names, ", ");
		strcat(counter_names, g_counters[i].scenario);
	}
	
	const char* dev_model  = ACAP_DEVICE_Prop("model")  ? ACAP_DEVICE_Prop("model")  : "unknown";
	const char* dev_serial = ACAP_DEVICE_Prop("serial") ? ACAP_DEVICE_Prop("serial") : "000000";
	const char* dev_date   = ACAP_DEVICE_Date();

	pos += snprintf(js + pos, sizeof(js) - pos,
		"/**\n"
		" * AI-B100 Counter Decoder\n"
		" * Device  : %s  (serial %s)\n"
		" * Generated: %s\n"
		" * Counters: %s\n"
		" * Enabled classes: %s\n"
		" *\n"
		" * Payload format\n"
		" * --------------\n"
		" * The AI-B100 sends raw binary bytes over LoRaWAN.\n"
		" * Each counter contributes one 16-bit little-endian unsigned integer\n"
		" * per enabled class, in the order: human, car, bike, bus, truck, other.\n"
		" * Total payload size = (number of counters) x (number of classes) x 2 bytes.\n"
		" *\n"
		" * Usage\n"
		" * -----\n"
		" * Call decodeCounters(bytes) where 'bytes' is the raw LoRaWAN payload\n"
		" * as a Buffer or Uint8Array. Returns an object with one key per counter,\n"
		" * each containing the enabled class counts.\n"
		" */\n\n"
		"function decodeCounters(bytes) {\n"
		"  var result = {};\n"
		"  var offset = 0;\n\n",
		dev_model, dev_serial, dev_date, counter_names, classes
	);
	
	// Generate decoding for each counter
	for (int i = 0; i < g_counter_count; i++) {
		pos += snprintf(js + pos, sizeof(js) - pos,
			"  // %s\n"
			"  result['%s'] = {};\n",
			g_counters[i].scenario, g_counters[i].scenario
		);
		
		if (g_publish_human) {
			pos += snprintf(js + pos, sizeof(js) - pos,
				"  result['%s'].human = bytes[offset] | (bytes[offset + 1] << 8);\n"
				"  offset += 2;\n",
				g_counters[i].scenario
			);
		}
		if (g_publish_car) {
			pos += snprintf(js + pos, sizeof(js) - pos,
				"  result['%s'].car = bytes[offset] | (bytes[offset + 1] << 8);\n"
				"  offset += 2;\n",
				g_counters[i].scenario
			);
		}
		if (g_publish_bike) {
			pos += snprintf(js + pos, sizeof(js) - pos,
				"  result['%s'].bike = bytes[offset] | (bytes[offset + 1] << 8);\n"
				"  offset += 2;\n",
				g_counters[i].scenario
			);
		}
		if (g_publish_bus) {
			pos += snprintf(js + pos, sizeof(js) - pos,
				"  result['%s'].bus = bytes[offset] | (bytes[offset + 1] << 8);\n"
				"  offset += 2;\n",
				g_counters[i].scenario
			);
		}
		if (g_publish_truck) {
			pos += snprintf(js + pos, sizeof(js) - pos,
				"  result['%s'].truck = bytes[offset] | (bytes[offset + 1] << 8);\n"
				"  offset += 2;\n",
				g_counters[i].scenario
			);
		}
		if (g_publish_other) {
			pos += snprintf(js + pos, sizeof(js) - pos,
				"  result['%s'].other = bytes[offset] | (bytes[offset + 1] << 8);\n"
				"  offset += 2;\n",
				g_counters[i].scenario
			);
		}
		
		pos += snprintf(js + pos, sizeof(js) - pos, "\n");
	}
	
	// Close the decodeCounters function
	pos += snprintf(js + pos, sizeof(js) - pos,
		"  return result;\n"
		"}\n"
	);
	
	pthread_mutex_unlock(&g_publish_mutex);
	pthread_mutex_unlock(&g_counter_mutex);
	
	// Build filename: aib100-decoder-{model}-{serial}-{date}.js
	char filename[128];
	snprintf(filename, sizeof(filename), "aib100-decoder-%s-%s-%s.js",
	         dev_model, dev_serial, dev_date);

	// Send as downloadable JavaScript file
	ACAP_HTTP_Header_FILE(response, filename, "application/javascript", strlen(js));
	ACAP_HTTP_Respond_String(response, "%s", js);
}

// ==================================================================
// Signal Handler
// ==================================================================

static gboolean
signal_handler(gpointer user_data) {
    LOG("Received SIGTERM, initiating shutdown\n");
    running = 0;
    
    if (main_loop && g_main_loop_is_running(main_loop)) {
        g_main_loop_quit(main_loop);
    }
    return G_SOURCE_REMOVE;
}

// ==================================================================
// Main
// ==================================================================

int main(void) {
    openlog(APP_PACKAGE, LOG_PID|LOG_CONS, LOG_USER);
    LOG("========================================\n");
    LOG("Starting %s\n", APP_PACKAGE);
    LOG("========================================\n");

    g_app_start_time = time(NULL);

    // Initialize ACAP
    ACAP_Init(APP_PACKAGE, Settings_Updated_Callback);
    
    // Load saved counters
    LOG("Loading saved counters...\n");
    Load_Counters_From_File();
    
    // Setup AOA event subscriptions
    LOG("Setting up AOA event subscriptions...\n");
    ACAP_EVENTS_SetCallback(AOA_Event_Callback);
    eventSubscriptions = ACAP_FILE_Read("settings/subscriptions.json");
    if (eventSubscriptions) {
        cJSON* subscription = eventSubscriptions->child;
        int count = 0;
        while (subscription) {
            ACAP_EVENTS_Subscribe(subscription, NULL);
            count++;
            subscription = subscription->next;
        }
        LOG("Subscribed to %d event topic(s)\n", count);
    } else {
        LOG_WARN("No event subscriptions found\n");
    }
    
    // Load settings
    cJSON* settings = ACAP_Get_Config("settings");
    if (settings) {
        cJSON* b100 = cJSON_GetObjectItem(settings, "b100");
        if (b100) {
            cJSON* ip = cJSON_GetObjectItem(b100, "ip");
            if (ip && ip->valuestring) {
                strncpy(g_b100_ip, ip->valuestring, sizeof(g_b100_ip) - 1);
            }
            cJSON* cbip = cJSON_GetObjectItem(b100, "callbackIP");
            if (cbip && cbip->valuestring && cbip->valuestring[0]) {
                strncpy(g_callback_ip, cbip->valuestring, sizeof(g_callback_ip) - 1);
            }
            cJSON* cbport = cJSON_GetObjectItem(b100, "callbackPort");
            if (cbport && cbport->valueint > 0)
                g_callback_port = cbport->valueint;
            cJSON* port = cJSON_GetObjectItem(b100, "port");
            if (port) {
                g_b100_port = port->valueint;
            }
        }
        
        cJSON* lorawan = cJSON_GetObjectItem(settings, "lorawan");
        if (lorawan) {
            cJSON* port = cJSON_GetObjectItem(lorawan, "port");
            if (port) {
                g_lorawan_port = port->valueint;
            }
            
            cJSON* confirmed = cJSON_GetObjectItem(lorawan, "confirmed");
            if (confirmed) {
                // confirmed mode is managed automatically; ignore the stored setting
            }
        }
        
        cJSON* polling = cJSON_GetObjectItem(settings, "polling");
        if (polling) {
            cJSON* health = cJSON_GetObjectItem(polling, "healthCheckIntervalSeconds");
            if (health) {
                g_health_check_interval = health->valueint;
            }
        }
        
        // Load transmission settings
        cJSON* transmission = cJSON_GetObjectItem(settings, "transmission");
        if (transmission) {
            cJSON* interval = cJSON_GetObjectItem(transmission, "intervalMinutes");
            if (interval) {
                g_publish_interval_minutes = interval->valueint;
                if (g_publish_interval_minutes < 1) g_publish_interval_minutes = 1;
                if (g_publish_interval_minutes > 60) g_publish_interval_minutes = 60;
            }
            
            cJSON* enabled = cJSON_GetObjectItem(transmission, "enabled");
            if (enabled) {
                g_publish_enabled = cJSON_IsTrue(enabled);
            }
            
            cJSON* classes = cJSON_GetObjectItem(transmission, "classes");
            if (classes) {
                cJSON* human = cJSON_GetObjectItem(classes, "human");
                if (human) g_publish_human = cJSON_IsTrue(human);
                cJSON* car = cJSON_GetObjectItem(classes, "car");
                if (car) g_publish_car = cJSON_IsTrue(car);
                cJSON* bike = cJSON_GetObjectItem(classes, "bike");
                if (bike) g_publish_bike = cJSON_IsTrue(bike);
                cJSON* bus = cJSON_GetObjectItem(classes, "bus");
                if (bus) g_publish_bus = cJSON_IsTrue(bus);
                cJSON* truck = cJSON_GetObjectItem(classes, "truck");
                if (truck) g_publish_truck = cJSON_IsTrue(truck);
                cJSON* other = cJSON_GetObjectItem(classes, "other");
                if (other) g_publish_other = cJSON_IsTrue(other);
            }
        }
    }
    
    // Initialize B100 client
    LOG("Initializing B100 client: %s:%d\n", g_b100_ip, g_b100_port);
    B100_Init(g_b100_ip, g_b100_port, 30);
    B100_Set_Downlink_Callback(B100_Downlink_Handler);
    B100_Set_Status_Callback(B100_Status_Handler);
    B100_Set_GPS_Callback(B100_GPS_Handler);
    
    // Initial connection test using /info endpoint
    ACAP_STATUS_SetString("app", "status", "Testing B100 connection...");
    if (B100_Test_Connection()) {
        LOG("B100 connection successful\n");
        
        // Configure B100 API enable and callback URIs
        {
            const char* cam_ip = g_callback_ip[0] ? g_callback_ip : ACAP_DEVICE_Prop("IPv4");
            char status_uri[64], receive_uri[64];
            snprintf(status_uri, sizeof(status_uri), "/local/%s/b100_status", APP_PACKAGE);
            snprintf(receive_uri, sizeof(receive_uri), "/local/%s/b100_receive", APP_PACKAGE);
            if (B100_Configure_Callbacks(cam_ip, g_callback_port, status_uri, receive_uri)) {
                g_callbacks_configured = 1;
                // Configure GPS callback — POST every 60 s to the b100_gps endpoint
                char gps_uri[64];
                snprintf(gps_uri, sizeof(gps_uri), "/local/%s/b100_gps", APP_PACKAGE);
                B100_Configure_GPS_Callback(gps_uri, 60);
                ACAP_STATUS_SetString("app", "status", "Running");
            } else {
                LOG_WARN("Failed to configure B100 callbacks\n");
                ACAP_STATUS_SetString("app", "status", "Running (callbacks failed)");
            }
        }

        // Trigger initial status request
        if (g_callbacks_configured)
            B100_Request_Status();
    } else {
        LOG_WARN("B100 connection failed - will retry in background\n");
        ACAP_STATUS_SetString("app", "status", "B100 connection failed - retrying...");
    }
    
    // Register HTTP endpoints
    ACAP_HTTP_Node("join", HTTP_Endpoint_join);
    ACAP_HTTP_Node("test", HTTP_Endpoint_test);
    ACAP_HTTP_Node("restart", HTTP_Endpoint_restart);
    ACAP_HTTP_Node("send", HTTP_Endpoint_send);
    ACAP_HTTP_Node("counters", HTTP_Endpoint_counters);
    ACAP_HTTP_Node("publish", HTTP_Endpoint_publish);
    ACAP_HTTP_Node("translator", HTTP_Endpoint_translator);
    ACAP_HTTP_Node("delete_counter", HTTP_Endpoint_delete_counter);
    ACAP_HTTP_Node("sync_counters", HTTP_Endpoint_sync_counters);
    ACAP_HTTP_Node("receive", HTTP_Endpoint_receive);
    ACAP_HTTP_Node("b100_status", HTTP_Endpoint_B100_Status_Callback);
    ACAP_HTTP_Node("b100_receive", HTTP_Endpoint_B100_Receive_Callback);
    ACAP_HTTP_Node("b100_gps", HTTP_Endpoint_B100_GPS_Callback);
    ACAP_HTTP_Node("gps", HTTP_Endpoint_gps);
    ACAP_HTTP_Node("b100_info", HTTP_Endpoint_b100_info);
    ACAP_HTTP_Node("b100_params", HTTP_Endpoint_b100_params);
    ACAP_HTTP_Node("b100_request_status", HTTP_Endpoint_b100_request_status);
    ACAP_HTTP_Node("linkcheck", HTTP_Endpoint_linkcheck);
    
    // Initialize next publish time
    pthread_mutex_lock(&g_publish_mutex);
    g_next_publish_time = time(NULL) + (g_publish_interval_minutes * 60);
    pthread_mutex_unlock(&g_publish_mutex);
    
    // Start background threads
    LOG("Starting background threads...\n");
    pthread_create(&health_thread, NULL, Health_Monitor_Thread, NULL);
    pthread_create(&publish_thread, NULL, Publish_Thread, NULL);
    
    // Setup signal handler
    LOG("Entering main loop\n");
	main_loop = g_main_loop_new(NULL, FALSE);
    GSource *signal_source = g_unix_signal_source_new(SIGTERM);
    if (signal_source) {
		g_source_set_callback(signal_source, signal_handler, NULL, NULL);
		g_source_attach(signal_source, NULL);
	} else {
		LOG_WARN("Signal detection failed\n");
	}
	
    g_main_loop_run(main_loop);
	
	// Cleanup
	LOG("Shutting down...\n");
	running = 0;
	
	pthread_join(health_thread, NULL);
	pthread_join(publish_thread, NULL);
	
	// Save counters before exit
	LOG("Saving counters before shutdown...\n");
	Save_Counters_To_File();
	
	// Cleanup event subscriptions
	if (eventSubscriptions) {
		cJSON_Delete(eventSubscriptions);
		eventSubscriptions = NULL;
	}
	
	B100_Cleanup();
    ACAP_Cleanup();
    
    LOG("Shutdown complete\n");
    closelog();
    return 0;
}
