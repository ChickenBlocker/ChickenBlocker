#define CHICKENBLOCKER_TEST
#include "../src/ChickenBlocker.c"

#include <assert.h>

static time_t local_time_at(int year, int month, int day, int hour, int minute) {
	struct tm tm = {0};
	tm.tm_year = year - 1900;
	tm.tm_mon = month - 1;
	tm.tm_mday = day;
	tm.tm_hour = hour;
	tm.tm_min = minute;
	tm.tm_isdst = -1;
	return mktime(&tm);
}

int main(void) {
	const char *id =
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	char line[256];
	struct MeetingOccurrence event;

	snprintf(line, sizeof line, "event=%s,100,200,0\n", id);
	assert(meeting_occurrence_parse(line, &event) == 0);
	assert(strcmp(event.id, id) == 0);
	assert(event.start == 100);
	assert(event.end == 200);
	assert(event.attempted == 0);

	snprintf(line, sizeof line, "event=%s,100,200,1", id);
	assert(meeting_occurrence_parse(line, &event) == 0);
	assert(event.attempted == 1);

	snprintf(line, sizeof line, "event=%s,100,200,0 junk\n", id);
	assert(meeting_occurrence_parse(line, &event) == -1);
	snprintf(line, sizeof line, "event=%s,200,100,0\n", id);
	assert(meeting_occurrence_parse(line, &event) == -1);

	assert(camera_confirms_meeting(1, 1));
	assert(!camera_confirms_meeting(1, 0));
	assert(!camera_confirms_meeting(0, 1));
	assert(!camera_confirms_meeting(-1, 1));

	struct SafeEyesState state = {0};
	time_t before_day = local_time_at(2026, 9, 21, 7, 59);
	time_t hour_8 = local_time_at(2026, 9, 21, 8, 0);
	time_t hour_9 = local_time_at(2026, 9, 21, 9, 45);
	time_t next_day = local_time_at(2026, 9, 22, 8, 0);
	assert(!calendar_sync_due(&state, before_day));
	assert(calendar_sync_due(&state, hour_8));
	local_hour_for(hour_8, state.se_cal_hour);
	assert(!calendar_sync_due(&state, hour_8 + 30 * 60));
	assert(calendar_sync_due(&state, hour_9));
	local_hour_for(hour_9, state.se_cal_hour);
	assert(!calendar_sync_due(&state, hour_9));
	assert(calendar_sync_due(&state, next_day));

	puts("meeting state regressions: ok");
	return 0;
}
