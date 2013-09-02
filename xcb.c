#include <stdlib.h>
#include <stdio.h>

#include <sys/epoll.h>

#include "xcb.h"

static int on_xcb_read(struct app_state* state, uint32_t events);

static my_epoll_cb xcb_cb = &on_xcb_read;

int setup_xcb(struct app_state* state) {
  int i, screen_no = -1;
  xcb_screen_t* screen;
  xcb_screen_iterator_t iter;
  uint32_t valmask;
  uint32_t vals[1];
  struct epoll_event event;

  state->xcb = xcb_connect(NULL, &screen_no);
  if (!state->xcb) {
    fputs("Cannot open display\n", stderr);
    return -1;
  }

  iter = xcb_setup_roots_iterator(xcb_get_setup(state->xcb));
  for (i = 0; i < screen_no; ++i) {
    if (!iter.rem) {
      fputs("Screen not found\n", stderr);
      return -1;
    }

    xcb_screen_next(&iter);
  }
  screen = iter.data;

  state->screen_res_width  = screen->width_in_pixels;
  state->screen_res_height = screen->height_in_pixels;

  valmask = XCB_CW_EVENT_MASK;
  vals[0] = XCB_EVENT_MASK_EXPOSURE;

  state->window = xcb_generate_id(state->xcb);
  xcb_create_window(
      state->xcb,
      XCB_COPY_FROM_PARENT,
      state->window,
      screen->root,
      (int16_t) (screen->width_in_pixels / 2 - 75),
      (int16_t) (screen->height_in_pixels / 2 - 75),
      150, 150,
      10,
      XCB_WINDOW_CLASS_INPUT_OUTPUT,
      screen->root_visual,
      valmask, vals);

  xcb_map_window(state->xcb, state->window);
  xcb_flush(state->xcb);

  event.events = EPOLLIN | EPOLLRDHUP;
  event.data.ptr = &xcb_cb;
  if (epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD,
                xcb_get_file_descriptor(state->xcb), &event) == -1) {
    perror("epoll_ctl (xcb)");
    return -1;
  }

  return 0;

/*
  ext_query = xcb_get_extension_data(c, &xcb_render_id);
  if (!ext_query->present) {
    fprintf(stderr, "no xrender\n");
    return -1;
  }
  */
}


void cleanup_xcb(struct app_state* state) {
  if (state->xcb) {
    xcb_disconnect(state->xcb);
    state->xcb = NULL;
  }
}

static int on_xcb_read(struct app_state* state, uint32_t events) {
  xcb_generic_event_t* event;

  while ((event = xcb_poll_for_event(state->xcb))) {
    printf("XCB: %d\n", (int) event->response_type);
    switch (event->response_type & ~0x80) {
    case XCB_EXPOSE: {
      /* xcb_expose_event_t* e = (xcb_expose_event_t*) event; */
    }
    default:
      break;
    }
    free(event);
  }

  if (xcb_connection_has_error(state->xcb))
    return -1;

  return 0;
}
