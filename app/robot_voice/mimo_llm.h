#ifndef __APPS_ROBOT_VOICE_MIMO_LLM_H
#define __APPS_ROBOT_VOICE_MIMO_LLM_H

#include <stddef.h>

int mimo_llm_chat(const char *api_key, const char *prompt, char *reply,
                  size_t reply_cap);

#endif
