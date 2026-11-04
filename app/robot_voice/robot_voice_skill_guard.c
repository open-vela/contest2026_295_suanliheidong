#include "robot_voice_diag.h"
/* Contest safety shim around the unchanged official Skill parser.
 * The uploaded loader returns snprintf's requested size and writes buf[off],
 * which can exceed its caller's 1024-byte buffer (log: 1054 bytes). Compile its
 * original implementation under private names, then invoke it with a bounded
 * overflow margin. No replacement Markdown parser or loader is introduced.
 * The included official source retains its original license and copyrights. */
#include <nuttx/config.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ROBOT_VOICE_AGENT_INTERPOSE
#define skill_loader_init rv_original_skill_init_unused
#define skill_loader_build_summary rv_original_skill_summary
#define skill_loader_check_changed rv_original_skill_changed_unused
#define skill_loader_refresh rv_original_skill_refresh_unused
#include <tools/skill_loader.c>
#undef skill_loader_init
#undef skill_loader_build_summary
#undef skill_loader_check_changed
#undef skill_loader_refresh

size_t skill_loader_build_summary(char *buf, size_t cap)
{
  char *temporary;
  size_t requested, used;
  if (!buf || !cap) return 0;
  buf[0] = 0;
  if (cap > 128 * 1024) return 0;
  /* One formatted entry is bounded by upstream's 64/256/256 byte local
   * title/description/path arrays. Its final overrun is below 2048 bytes. */
  temporary = calloc(1, cap + 2048);
  if (!temporary) return 0;
  requested = rv_original_skill_summary(temporary, cap);
  used = strnlen(temporary, cap - 1);
  /* Preserve complete UTF-8 at the original summary limit. */
  if (requested >= cap && used) {
    size_t start = used - 1;
    while (start && ((unsigned char)temporary[start] & 0xc0) == 0x80) start--;
    unsigned char lead = (unsigned char)temporary[start];
    size_t width = lead < 0x80 ? 1 : (lead < 0xe0 ? 2 : (lead < 0xf0 ? 3 : 4));
    if (start + width > used) used = start;
  }
  memcpy(buf, temporary, used);
  buf[used] = 0;
  free(temporary);
  if (requested >= cap)
    RV_VERBOSE("[RV-SKILL] summary guard requested=%zu written=%zu cap=%zu\n", requested, used, cap);
  return used;
}
#endif

void robot_voice_skill_guard_link_anchor(void)
{
#ifdef ROBOT_VOICE_AGENT_INTERPOSE
  RV_VERBOSE("[RV-SKILL] official parser retained; summary boundary guard=enabled\n");
#endif
}
