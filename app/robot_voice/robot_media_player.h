#ifndef ROBOT_MEDIA_PLAYER_H
#define ROBOT_MEDIA_PLAYER_H

#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

typedef void (*robot_media_event_callback)(void *cookie, int event,
                                           int result, const char *extra);

void *robot_legacy_media_open(const char *stream);
int robot_legacy_media_close(void *handle, int pending_stop);
int robot_legacy_media_prepare(void *handle, const char *url, const char *options);
int robot_legacy_media_set_event_callback(void *handle, void *cookie,
                                    robot_media_event_callback callback);
int robot_legacy_media_start(void *handle);
int robot_legacy_media_stop(void *handle);
int robot_legacy_media_pause(void *handle);
int robot_legacy_media_suspend(void *handle);
int robot_legacy_media_resume_active(void);
bool robot_legacy_media_start_pending(void *handle);
bool robot_legacy_media_is_running(void *handle);
bool robot_legacy_media_is_paused(void *handle);
int robot_legacy_media_seek(void *handle, unsigned int position);
int robot_legacy_media_get_position(void *handle, unsigned int *position);
int robot_legacy_media_get_duration(void *handle, unsigned int *duration);
ssize_t robot_legacy_media_write_data(void *handle, const void *data, size_t len);
int media_policy_set_stream_volume(const char *stream, int volume);

#endif
