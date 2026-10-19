#define CHICKENBLOCKER_TEST
#define SE_SCHEDULE_FILE "/tmp/kilo_cb_se_schedule_test"
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

	struct MeetingLedger ledger = {0};
	ledger.n = 2;
	ledger.entries[0].start = 100;
	ledger.entries[0].end = 200;
	ledger.entries[1].start = 100;
	ledger.entries[1].end = 200;
	assert(meeting_ledger_find_unseen_time(&ledger, 100, 200) == 0);
	ledger.entries[0].seen = 1;
	assert(meeting_ledger_find_unseen_time(&ledger, 100, 200) == 1);
	assert(meeting_ledger_find_unseen_time(&ledger, 101, 200) == -1);

	assert(camera_confirms_meeting(1, 1));
	assert(!camera_confirms_meeting(1, 0));
	assert(!camera_confirms_meeting(0, 1));
	assert(!camera_confirms_meeting(-1, 1));
	assert(vt_mask_status("masked"));
	assert(vt_mask_status("masked-runtime"));
	assert(!vt_mask_status("enabled"));
	assert(!vt_mask_status("disabled"));

	const char cmd_normal[] = "/opt/vivaldi/vivaldi-bin\0--no-first-run\0";
	const char cmd_temp_eq[] = "/opt/vivaldi/vivaldi-bin\0--user-data-dir=/tmp/test\0";
	const char cmd_temp_arg[] = "/opt/vivaldi/vivaldi-bin\0--user-data-dir\0/tmp/test\0";
	const char cmd_temp_flat[] =
		"/opt/vivaldi/vivaldi-bin --user-data-dir=/tmp/test --no-first-run\0";
	const char cmd_profile[] = "/opt/vivaldi/vivaldi-bin\0--profile-directory=Profile 1\0";
	assert(!nul_list_has_switch(cmd_normal, sizeof cmd_normal - 1, "--user-data-dir"));
	assert(nul_list_has_switch(cmd_temp_eq, sizeof cmd_temp_eq - 1, "--user-data-dir"));
	assert(nul_list_has_switch(cmd_temp_arg, sizeof cmd_temp_arg - 1, "--user-data-dir"));
	assert(nul_list_has_switch(cmd_temp_flat, sizeof cmd_temp_flat - 1, "--user-data-dir"));
	assert(nul_list_has_switch(cmd_profile, sizeof cmd_profile - 1, "--profile-directory"));

	const char env_normal[] = "HOME=/home/test\0XDG_CONFIG_HOME=/home/test/.config\0";
	const char env_temp[] = "HOME=/home/test\0XDG_CONFIG_HOME=/tmp/config\0";
	assert(!nul_env_path_differs(env_normal, sizeof env_normal - 1,
		"XDG_CONFIG_HOME", "/home/test/.config"));
	assert(nul_env_path_differs(env_temp, sizeof env_temp - 1,
		"XDG_CONFIG_HOME", "/home/test/.config"));
	assert(!nul_env_path_differs(env_normal, sizeof env_normal - 1,
		"CHROME_USER_DATA_DIR", "/home/test/.config/vivaldi"));

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

	// Work-deadline adjustment on resume: suspend is NOT work time. A short
	// sleep pauses the countdown (deadline pushed by the sleep duration,
	// remaining work fully owed); a sleep >= the work time left at sleep
	// start skips the pending break and starts a fresh interval at wake.
	// Numbers are anchored at sleep_start = wake - slept.
	int skip = -1;
	time_t wake = 1000000;
	// 20 min owed at sleep start, 5-min sleep: still owe 20 min after wake
	time_t work_end = (wake - 300) + 1200;
	assert(se_work_resume_adjust(work_end, wake, 300, 1800, &skip) == wake + 1200);
	assert(skip == 0);
	// sleep exactly as long as the time left -> skip + fresh interval
	time_t exact = (wake - 1200) + 1200;
	assert(se_work_resume_adjust(exact, wake, 1200, 1800, &skip) == wake + 1800);
	assert(skip == 1);
	skip = 0;
	// the Oct-17 incident shape: 76s of work left, 39-min sleep -> skip
	time_t short_left = (wake - 2352) + 76;
	assert(se_work_resume_adjust(short_left, wake, 2352, 1800, &skip) == wake + 1800);
	assert(skip == 1);
	skip = 0;
	// 1s more owed than slept -> still a pause, not a skip
	time_t owed_301 = (wake - 300) + 301;
	assert(se_work_resume_adjust(owed_301, wake, 300, 1800, &skip) == owed_301 + 300);
	assert(skip == 0);
	// deadline already past pre-sleep (pathological) -> skip
	time_t past = (wake - 60) - 10;
	assert(se_work_resume_adjust(past, wake, 60, 1800, &skip) == wake + 1800);
	assert(skip == 1);

	// Tray schedule publisher: work armed + cycle 3 -> next break IS the
	// long one, interval line 4/4.
	{
		struct SafeEyesState sched_st = {0};
		sched_st.se_cycle = 3;
		sched_st.se_work_end = 2000000;
		se_write_schedule(&sched_st, 0, 1500000);
		size_t n = 0;
		char *buf = read_file(SE_SCHEDULE_FILE, &n);
		assert(buf != NULL);
		assert(strstr(buf, "night=0"));
		assert(strstr(buf, "next_break=2000000"));
		assert(strstr(buf, "next_long=2000000"));
		assert(strstr(buf, "interval=4"));
		assert(strstr(buf, "total=4"));
		free(buf);
		struct SafeEyesState night_st = {0};
		se_write_schedule(&night_st, 1, 1500000);
		buf = read_file(SE_SCHEDULE_FILE, &n);
		assert(buf != NULL);
		assert(strstr(buf, "night=1"));
		assert(strstr(buf, "interval=0"));
		free(buf);
		unlink(SE_SCHEDULE_FILE);
	}

	// Cycle counter after a completed break: shorts advance, a long (imposed
	// OR manual) ends the set.
	assert(se_cycle_after_break(0, 0) == 1);
	assert(se_cycle_after_break(2, 0) == 3);
	assert(se_cycle_after_break(3, 0) == 0);   // short at cycle 3 wraps the set
	assert(se_cycle_after_break(1, 1) == 0);   // long break resets the count
	assert(se_cycle_after_break(3, 1) == 0);

	// The main poll loop normally runs for CYCLE_BUDGET, but a work deadline
	// inside that window must wake the next enforcement pass at the deadline.
	time_t cycle_start = 3000000;
	assert(pause_cycle_deadline(cycle_start, 0)
		== cycle_start + CYCLE_BUDGET);
	assert(pause_cycle_deadline(cycle_start, cycle_start + 10)
		== cycle_start + 10);
	assert(pause_cycle_deadline(cycle_start, cycle_start + CYCLE_BUDGET + 5)
		== cycle_start + CYCLE_BUDGET);
	assert(pause_cycle_deadline(cycle_start, cycle_start - 1)
		== cycle_start - 1);

	puts("meeting state regressions: ok");
	return 0;
}
