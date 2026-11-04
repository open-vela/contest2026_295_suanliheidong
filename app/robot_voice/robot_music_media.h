#ifndef ROBOT_MUSIC_MEDIA_H
#define ROBOT_MUSIC_MEDIA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef void (*robot_music_event_cb)(void *, int, int, const char *);
void robot_music_media_link_anchor(void);
void *robot_music_media_open(const char *);
int robot_music_media_close(void *, int);
int robot_music_media_set_track_info(void *, const char *, const char *);
int robot_music_media_prepare(void *, const char *, const char *);
int robot_music_media_set_event_callback(void *, void *, robot_music_event_cb);
int robot_music_media_start(void *);
int robot_music_media_stop(void *);
int robot_music_media_pause(void *);
int robot_music_media_seek(void *, unsigned int);
int robot_music_media_get_position(void *, unsigned int *);
int robot_music_media_get_duration(void *, unsigned int *);
ssize_t robot_music_media_write_data(void *, const void *, size_t);
int robot_music_media_set_volume(const char *, int);
bool robot_music_media_owns_reply(unsigned long);
bool robot_music_media_start_pending(void *);
bool robot_music_media_is_active(void);
bool robot_music_media_has_task(void);
/* Auto preemption is resumable; user pause/stop never auto-resumes. */
int robot_music_media_suspend(void);
int robot_music_media_resume(void);
#define ROBOT_MUSIC_MOTION_READY_BYTES (64U * 1024U)
bool robot_music_media_session_ready(uint32_t session_id);
bool robot_music_media_motion_allowed(uint32_t session_id);
int robot_music_media_react(uint32_t session_id, uint32_t event_id, bool calm);
int robot_music_media_claim_tail(uint32_t session_id, uint32_t event_id);
/* The Skill arms a local policy once; ticks require no LLM/network requests. */
int robot_music_media_companion_start(uint32_t session_id, uint32_t event_id,
                                      bool calm, bool tail_enabled);
int robot_music_media_claim_periodic_tail(uint32_t session_id, uint32_t event_id);
int robot_music_media_stop_for_voice(unsigned long run);
#endif
