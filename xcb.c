#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include <sys/epoll.h>

#include <xcb/shape.h>
#include <xcb/bigreq.h>

#include "xcb.h"

static xcb_visualid_t get_rgba_visual(xcb_connection_t* c,
                                      xcb_screen_t* screen);
static int on_xcb_read(struct app_state* state, uint32_t events);

static my_epoll_cb xcb_cb = &on_xcb_read;

int setup_xcb(struct app_state* state) {
  int i, screen_no = -1;
  xcb_screen_t* screen;
  xcb_screen_iterator_t iter;
  uint32_t valmask;
  uint32_t vals[4];
  xcb_visualid_t rgba_visual;
  struct epoll_event event;
  const xcb_query_extension_reply_t* ext_query;

  state->window = state->gc = state->cm = XCB_NONE;
  state->xcb = xcb_connect(NULL, &screen_no);
  if (!state->xcb) {
    fputs("Cannot open display\n", stderr);
    return -1;
  }

  xcb_big_requests_enable(state->xcb);

  ext_query = xcb_get_extension_data(state->xcb, &xcb_shape_id);
  if (!ext_query->present) {
    fputs("xshape extension not present\n", stderr);
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

  rgba_visual = get_rgba_visual(state->xcb, screen);

  state->cm = xcb_generate_id(state->xcb);
  xcb_create_colormap(state->xcb, XCB_COLORMAP_ALLOC_NONE,
      state->cm, screen->root, rgba_visual);

  valmask = XCB_CW_BORDER_PIXEL
            | XCB_CW_OVERRIDE_REDIRECT
            | XCB_CW_EVENT_MASK
            | XCB_CW_COLORMAP;
  vals[0] = 0;
  vals[1] = 1;
  vals[2] = XCB_EVENT_MASK_EXPOSURE;
  vals[3] = state->cm;

  state->window = xcb_generate_id(state->xcb);
  xcb_create_window(
      state->xcb,
      32,
      state->window,
      screen->root,
      (int16_t) (screen->width_in_pixels / 2 - 150),
      (int16_t) (screen->height_in_pixels / 2 - 75),
      150, 150,
      0,
      XCB_WINDOW_CLASS_INPUT_OUTPUT,
      rgba_visual,
      valmask, vals);

  state->gc = xcb_generate_id(state->xcb);
  xcb_create_gc(state->xcb, state->gc, state->window, 0, NULL);

  xcb_shape_rectangles(state->xcb, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
      XCB_CLIP_ORDERING_UNSORTED, state->window, 0, 0, 0, NULL);

  xcb_flush(state->xcb);
  event.events = EPOLLIN | EPOLLRDHUP;
  event.data.ptr = &xcb_cb;
  if (epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD,
                xcb_get_file_descriptor(state->xcb), &event) == -1) {
    perror("epoll_ctl (xcb)");
    return -1;
  }

  return 0;
}


void cleanup_xcb(struct app_state* state) {
  if (state->xcb) {
    if (state->window != XCB_NONE)
      xcb_destroy_window(state->xcb, state->window);
    if (state->gc != XCB_NONE)
      xcb_free_gc(state->xcb, state->gc);
    if (state->cm != XCB_NONE)
      xcb_free_colormap(state->xcb, state->cm);
    xcb_disconnect(state->xcb);
    state->xcb = NULL;
  }
}

void move_resize(struct app_state* state) {
  uint32_t values[4];
  if (state->mumble_active_w * state->mumble_active_h > 0) {
    xcb_map_window(state->xcb, state->window);

    values[0] = state->mumble_active_x;
    values[1] = state->mumble_active_y;
    values[2] = state->mumble_active_w;
    values[3] = state->mumble_active_h;

    xcb_configure_window(state->xcb, state->window,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
        | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
        values);
  } else {
    xcb_unmap_window(state->xcb, state->window);
    xcb_flush(state->xcb);
  }
}

void blit(struct app_state* state) {
  size_t offset, size, y;
  void* ptr;
  void* buf;
  if (!state->mumble_shm_ptr
      || state->mumble_active_w * state->mumble_active_h == 0)
    return;
  offset = state->mumble_active_x +
    (uint32_t) state->mumble_active_y * state->screen_res_width;
  ptr = offset + (uint32_t*) state->mumble_shm_ptr;
  size = (size_t) state->mumble_active_w * state->mumble_active_h * 4;

  buf = malloc(size);
  for (y = 0; y < size / 4; ++y)
    ((uint32_t*)buf)[y] = 0xffff0000;
  for (y = 0; y < state->mumble_active_h; ++y) {
    memcpy((uint32_t*) buf + y * state->mumble_active_w,
           (uint32_t*) ptr + y * state->screen_res_width,
           (size_t) state->mumble_active_w * 4);
  }

  xcb_put_image(state->xcb, XCB_IMAGE_FORMAT_Z_PIXMAP,
      state->window, state->gc,
      state->mumble_active_w, state->mumble_active_h,
      0, 0, 0, 32,
      (uint32_t) size, buf);
  free(buf);

  xcb_flush(state->xcb);
}

static xcb_visualid_t get_rgba_visual(xcb_connection_t* c,
                                      xcb_screen_t* screen) {
  xcb_depth_iterator_t depth_iter;

  for (depth_iter = xcb_screen_allowed_depths_iterator(screen);
       depth_iter.rem;
       xcb_depth_next(&depth_iter)) {
    xcb_depth_t* d = depth_iter.data;
    xcb_visualtype_iterator_t visual_iter;
    if (d->depth != 32)
      continue;
    for (visual_iter = xcb_depth_visuals_iterator(d);
         visual_iter.rem;
         xcb_visualtype_next(&visual_iter)) {
      xcb_visualtype_t* v = visual_iter.data;
      if (v->red_mask != 0xff0000
          || v->green_mask != 0x00ff00
          || v->blue_mask != 0x0000ff)
        continue;
      return v->visual_id;
    }
  }
  return XCB_NONE;
}

static int on_xcb_read(struct app_state* state, uint32_t events) {
  xcb_generic_event_t* event;
  int needs_blit = 0;

  while ((event = xcb_poll_for_event(state->xcb))) {
    printf("XCB: %d\n", (int) event->response_type);
    switch (event->response_type & ~0x80) {
    case 0: {
      /* xcb_request_error_t* error = (xcb_request_error_t*) event;
       printf("error: code=%d seq=%d bad=%d min=%d maj=%d\n",
          (int) error->error_code,
          (int) error->sequence,
          (int) error->bad_value,
          (int) error->minor_opcode,
          (int) error->major_opcode); */
      break;
    }
    case XCB_EXPOSE: {
      /* xcb_expose_event_t* e = (xcb_expose_event_t*) event; */
      break;
    }
    default:
      break;
    }
    free(event);
  }

  if (xcb_connection_has_error(state->xcb))
    return -1;

  if (needs_blit)
    blit(state);

  return 0;
}
