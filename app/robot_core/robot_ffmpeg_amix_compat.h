/****************************************************************************
 * Contest-local FFmpeg amix compatibility link anchor
 ****************************************************************************/

#ifndef __CONTEST_ROBOT_FFMPEG_AMIX_COMPAT_H
#define __CONTEST_ROBOT_FFMPEG_AMIX_COMPAT_H

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * This function has no runtime behavior.  robot_voice references it so the
 * archive linker must pull robot_ffmpeg_amix_compat.o into the final image.
 */
void robot_ffmpeg_amix_compat_link_anchor(void);

#ifdef __cplusplus
}
#endif

#endif
