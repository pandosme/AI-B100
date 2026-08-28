#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "alert.h"
#include "counting.h"
#include "occupancy.h"

static void test_counting_transition_balance(void) {
	Counting_Reset();
	int counted_inside = 0;
	int counted_bucket = 0;

	Counting_Process_Transition(1, 1, 0, 1, &counted_inside, &counted_bucket);
	RadarAreaBalance balance = Counting_Area_Balance();
	assert(balance.entering == 1);
	assert(balance.exiting == 0);
	assert(counted_inside == 1);

	Counting_Process_Transition(1, 0, 1, 1, &counted_inside, &counted_bucket);
	balance = Counting_Area_Balance();
	assert(balance.entering == 1);
	assert(balance.exiting == 1);

	Counting_Mark_Published(balance);
	balance = Counting_Area_Balance();
	assert(balance.entering == 0);
	assert(balance.exiting == 0);
}

static CountingSceneConfig counting_scene(uint16_t id, const char* name,
	CountingDirection direction, uint8_t classes) {
	CountingSceneConfig scene = {
		.id = id,
		.enabled = 1,
		.direction = direction,
		.class_mask = classes,
		.x1 = 500,
		.y1 = 100,
		.x2 = 500,
		.y2 = 900
	};
	strncpy(scene.name, name, sizeof(scene.name) - 1);
	return scene;
}

static void test_completed_track_line_counting(void) {
	CountingSceneConfig scenes[] = {
		counting_scene(1, "Right", COUNTING_DIRECTION_LEFT_TO_RIGHT,
			COUNTING_CLASS_HUMAN | COUNTING_CLASS_VEHICLE),
		counting_scene(2, "Left", COUNTING_DIRECTION_RIGHT_TO_LEFT,
			COUNTING_CLASS_HUMAN)
	};
	assert(Counting_Configure(scenes, 2));
	Counting_Reset_All();

	assert(Counting_Process_Completed_Track(1, 1, 90, 30, 5, 0, 100, 500, 900, 500) == 0);
	assert(Counting_Process_Completed_Track(0, 1, 20, 30, 5, 0, 100, 500, 900, 500) == 0);
	assert(Counting_Process_Completed_Track(0, 1, 90, 30, 1, 2, 100, 500, 900, 500) == 0);
	assert(Counting_Process_Completed_Track(0, 1, 90, 30, 5, 0, 100, 500, 900, 500) == 1);
	assert(Counting_Process_Completed_Track(0, 2, 90, 30, 5, 0, 100, 500, 900, 500) == 1);
	assert(Counting_Process_Completed_Track(0, 1, 90, 30, 5, 0, 900, 500, 100, 500) == 1);
	assert(Counting_Process_Completed_Track(0, 1, 90, 30, 5, 0, 100, 950, 900, 950) == 0);
	assert(Counting_Process_Completed_Track(0, 1, 90, 30, 5, 0, 100, 100, 900, 100) == 0);

	CountingSceneSnapshot snapshot[2];
	assert(Counting_Get_Scenes(snapshot, 2) == 2);
	assert(snapshot[0].human == 1 && snapshot[0].vehicle == 1);
	assert(snapshot[1].human == 1 && snapshot[1].vehicle == 0);

	uint8_t payload[6] = {0};
	assert(Counting_Build_Cumulative_Payload(payload, sizeof(payload)) == sizeof(payload));
	const uint8_t expected[] = {0, 1, 0, 1, 0, 1};
	assert(memcmp(payload, expected, sizeof(expected)) == 0);

	CountingSceneConfig reordered[] = {
		counting_scene(2, "Left renamed", COUNTING_DIRECTION_RIGHT_TO_LEFT,
			COUNTING_CLASS_HUMAN),
		counting_scene(1, "Right", COUNTING_DIRECTION_LEFT_TO_RIGHT,
			COUNTING_CLASS_HUMAN | COUNTING_CLASS_VEHICLE)
	};
	assert(Counting_Configure(reordered, 2));
	assert(Counting_Get_Scenes(snapshot, 2) == 2);
	assert(snapshot[0].config.id == 2 && snapshot[0].human == 1);
	assert(snapshot[1].config.id == 1 && snapshot[1].human == 1 && snapshot[1].vehicle == 1);
}

static void test_occupancy_peak(void) {
	Occupancy_Reset();
	Occupancy_Update_Current(RadarCounts_Make(2, 2, 0, 0));
	Occupancy_Update_Current(RadarCounts_Make(5, 3, 2, 0));
	Occupancy_Update_Current(RadarCounts_Make(1, 1, 0, 0));

	RadarCounts current = Occupancy_Current_Counts();
	RadarCounts peak = Occupancy_Peak_Counts();
	assert(current.total == 1);
	assert(peak.total == 5);
	assert(peak.human == 3);
	assert(peak.vehicle == 2);

	Occupancy_Mark_Published();
	peak = Occupancy_Peak_Counts();
	assert(peak.total == 0);
}

static void test_presence_schedule(void) {
	time_t now = time(NULL);
	struct tm local_time;
	localtime_r(&now, &local_time);
	int current_minutes = local_time.tm_hour * 60 + local_time.tm_min;

	Alert_Reset();
	Alert_Set_Schedule(1, 0, 0);
	Alert_Update_Current(RadarCounts_Make(1, 1, 0, 0), now, 2);
	Alert_Update_Current(RadarCounts_Make(1, 1, 0, 0), now + 2, 2);
	assert(Alert_Active());

	int future_start = (current_minutes + 10) % 1440;
	int future_end = (current_minutes + 20) % 1440;
	Alert_Set_Schedule(1, future_start, future_end);
	Alert_Update_Current(RadarCounts_Make(1, 1, 0, 0), now, 2);
	assert(!Alert_Active());
}

int main(void) {
	test_counting_transition_balance();
	test_completed_track_line_counting();
	test_occupancy_peak();
	test_presence_schedule();
	return 0;
}
