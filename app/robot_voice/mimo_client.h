#ifndef __APPS_ROBOT_VOICE_MIMO_CLIENT_H
#define __APPS_ROBOT_VOICE_MIMO_CLIENT_H

#include <stddef.h>

typedef struct
{
  const char *name;
  const char *value;
} robot_mimo_header_t;

int robot_mimo_post(const char *api_key, const char *body, size_t body_len,
                    char *response, size_t response_cap, size_t *response_len);

#endif
