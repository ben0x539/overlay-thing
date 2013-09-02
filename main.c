#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <unistd.h>

#include <sys/epoll.h>

#include "main.h"
#include "mumble.h"
#include "xcb.h"

int main(void) {
  struct app_state state;

  state.mumble_pipe_fd = state.mumble_wait_fd = -1;
  state.mumble_shm_ptr = state.xcb = NULL;

  state.home = getenv("HOME");
  if (!state.home) {
    fputs("HOME not set, exiting", stderr);
    return -1;
  }

  if ((state.epoll_fd = epoll_create1(EPOLL_CLOEXEC)) == -1) {
    perror("epoll_create1");
    return -1;
  }

  if (setup_xcb(&state) == -1) {
    /* TODO: how does x error checking work again */
    cleanup(&state);
    return -1;
  }

  /*
  if (setup_mumble(&state) == -1) {
    cleanup(&state);
    return -1;
  }
  */

  for (;;) {
    struct epoll_event event;
    int ready = epoll_wait(state.epoll_fd, &event, 1, -1);
    my_epoll_cb cb;
    if (ready == -1) {
      if (errno == EINTR) {
        continue;
      } else {
        perror("epoll_wait");
        cleanup(&state);
        return -1;
      }
    } else if (ready != 1) {
      break;
    }

    cb = *(my_epoll_cb*) event.data.ptr;
    if ((*cb)(&state, event.events) == -1)
      break;
  }
  cleanup(&state);

  return 0;
}

void cleanup(struct app_state* state) {
  close(state->epoll_fd);
  cleanup_mumble(state);
  cleanup_xcb(state);
}
