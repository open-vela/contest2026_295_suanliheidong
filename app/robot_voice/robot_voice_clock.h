#ifndef ROBOT_VOICE_CLOCK_H
#define ROBOT_VOICE_CLOCK_H
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
/* Call confirm only AFTER an independent NTP/RTC/manual synchronization.
 * Reading a plausible epoch alone does not prove synchronization. */
int robot_voice_clock_confirm(void);
void robot_voice_clock_clear(void);
bool robot_voice_clock_reply(const char *text, char *reply, size_t cap);
/* Pure formatting/matching seam, also used for exhaustive host checks. */
bool robot_voice_clock_reply_at(const char *text, time_t utc, bool trusted,
                                char *reply, size_t cap);
void robot_voice_clock_diag(void);
void robot_voice_clock_context(char *out, size_t cap);
#endif
