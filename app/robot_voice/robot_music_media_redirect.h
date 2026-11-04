#ifndef ROBOT_MUSIC_MEDIA_REDIRECT_H
#define ROBOT_MUSIC_MEDIA_REDIRECT_H
/* Applied ONLY to Agent tool_media.c by voice_cc.py / deferred CMake.
 * The contest keeps the Agent ABI but routes it to the local HTTP/MP3
 * compatibility backend instead of the native Media daemon. */
void robot_voice_music_route_tool_anchor(void) {}
int robot_music_media_set_track_info(void *, const char *, const char *);
#define media_player_open robot_music_media_open
#define media_player_close robot_music_media_close
#define media_player_set_track_info robot_music_media_set_track_info
#define media_player_prepare robot_music_media_prepare
#define media_player_set_event_callback robot_music_media_set_event_callback
#define media_player_start robot_music_media_start
#define media_player_stop robot_music_media_stop
#define media_player_pause robot_music_media_pause
#define media_player_seek robot_music_media_seek
#define media_player_get_position robot_music_media_get_position
#define media_player_get_duration robot_music_media_get_duration
#define media_player_write_data robot_music_media_write_data
#define media_policy_set_stream_volume robot_music_media_set_volume
#endif
