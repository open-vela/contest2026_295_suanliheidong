/*
 * AI Agent completed-response OLED output.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __AI_AGENT_UI_AGENT_OLED_H
#define __AI_AGENT_UI_AGENT_OLED_H

#ifdef __cplusplus
extern "C" {
#endif

int agent_oled_start(void);
void agent_oled_enqueue(const char *content);

#ifdef __cplusplus
}
#endif

#endif /* __AI_AGENT_UI_AGENT_OLED_H */
