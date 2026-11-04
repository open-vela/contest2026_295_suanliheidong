/*
 * Contest-local synchronous Wi-Fi recovery helpers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CONTEST_ROBOT_VOICE_WIFI_H
#define __CONTEST_ROBOT_VOICE_WIFI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Wait for a usable IPv4 link and restore saved Wi-Fi credentials if needed. */
int robot_voice_wifi_ensure_connected(uint32_t timeout_ms);

/* Force one saved-credential Wi-Fi transaction, then wait for IPv4. */
int robot_voice_wifi_reconnect_and_ensure(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* __CONTEST_ROBOT_VOICE_WIFI_H */
