/*
 * Contest-local generated Skill resource interface.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CONTEST_SKILL_RESOURCE_H
#define __CONTEST_SKILL_RESOURCE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

struct contest_skill_resource_s
{
  const char *filename;
  const unsigned char *content;
  size_t size;
};

extern const struct contest_skill_resource_s contest_skill_resources[];
extern const size_t contest_skill_resource_count;

#ifdef __cplusplus
}
#endif

#endif /* __CONTEST_SKILL_RESOURCE_H */
