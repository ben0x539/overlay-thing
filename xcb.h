#ifndef OVERLAY_APP_XCB_H
#define OVERLAY_APP_XCB_H

#include "main.h"

int setup_xcb(struct app_state* state);
void cleanup_xcb(struct app_state* state);

void move_resize(struct app_state* state);
void blit(struct app_state* state);

#endif
