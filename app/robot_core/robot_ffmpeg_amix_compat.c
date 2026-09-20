/****************************************************************************
 * Contest-local FFmpeg amix compatibility unit
 *
 * OpenVela's af_asubgraph.c references ff_amix_* helpers implemented by
 * external/ffmpeg/ffmpeg/libavfilter/amix.c, but the current FFmpeg Makefile
 * does not add amix.o for CONFIG_ASUBGRAPH_FILTER / CONFIG_AMIX_FILTER.
 *
 * Keep the official source read-only.  Compile that implementation through
 * this contest-local translation unit instead.
 ****************************************************************************/

#include <nuttx/config.h>

#include "robot_ffmpeg_amix_compat.h"

void robot_ffmpeg_amix_compat_link_anchor(void)
{
  /* Link anchor only. */
}

#ifdef CONFIG_LIB_FFMPEG

#ifndef HAVE_AV_CONFIG_H
#  define HAVE_AV_CONFIG_H 1
#endif

/*
 * This is intentionally an include of the official read-only implementation,
 * not a modified copy.  Nested quoted includes such as "amix.h" resolve from
 * the official libavfilter directory; the FFmpeg root include path is added
 * by the contest-local build files for libavutil/... headers.
 */
#include "../../../external/ffmpeg/ffmpeg/libavfilter/amix.c"

#endif /* CONFIG_LIB_FFMPEG */
