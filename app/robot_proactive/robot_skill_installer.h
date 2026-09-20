/*
 * Contest-local installer for the official Agent skill directory.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CONTEST_ROBOT_SKILL_INSTALLER_H
#define __CONTEST_ROBOT_SKILL_INSTALLER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

enum robot_skill_install_result_e
{
  ROBOT_SKILL_INSTALL_ALREADY_CURRENT = 0,
  ROBOT_SKILL_INSTALL_CREATED = 1,
  ROBOT_SKILL_INSTALL_UPDATED = 2,
};

int robot_skill_installer_ensure(void);
size_t robot_skill_installer_count(void);
size_t robot_skill_installer_current_count(void);
size_t robot_skill_installer_created_count(void);
size_t robot_skill_installer_updated_count(void);
size_t robot_skill_installer_failed_count(void);
const char *robot_skill_installer_path(void);
bool robot_skill_installer_exists(void);
bool robot_skill_installer_matches(void);
int robot_skill_installer_last_result(void);
const char *robot_skill_installer_result_name(int result);

#ifdef __cplusplus
}
#endif

#endif /* __CONTEST_ROBOT_SKILL_INSTALLER_H */
