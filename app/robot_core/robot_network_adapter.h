#ifndef __ROBOT_NETWORK_ADAPTER_H
#define __ROBOT_NETWORK_ADAPTER_H

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * Start the contest-local Wi-Fi association guard.
 *
 * The guard does not replace the OpenVela network manager.  It only fixes
 * board-side bring-up semantics that are important on this ESP32-S3 target:
 *
 *   - an old IPv4 address is not treated as proof of Wi-Fi association;
 *   - DHCP is attempted only after a real AP BSSID is present;
 *   - failed association is retried in the background.
 *
 * Returns 0 when the guard thread is running/already running, or a negative
 * errno-style value when it cannot be started.
 */
int robot_network_adapter_start(void);

#ifdef __cplusplus
}
#endif

#endif /* __ROBOT_NETWORK_ADAPTER_H */
