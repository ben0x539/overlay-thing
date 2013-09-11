#ifndef OVERLAY_APP_MAIN_H
#define OVERLAY_APP_MAIN_H

#include <xcb/xcb.h>

#include "overlay.h"

struct app_state;
typedef int (*my_epoll_cb)(struct app_state*, uint32_t);

struct app_state {
  xcb_connection_t* xcb;
  size_t mumble_msg_read;
  void* mumble_shm_ptr;
  const char* home;
  xcb_gcontext_t gc;
  int epoll_fd;
  int mumble_pipe_fd;
  int mumble_wait_fd;
  xcb_window_t window;
  uint16_t mumble_active_x, mumble_active_y, mumble_active_w, mumble_active_h;
  uint16_t screen_res_width;
  uint16_t screen_res_height;
  struct OverlayMsg mumble_msg;
};

void cleanup(struct app_state* state);

#endif
