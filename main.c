#define _POSIX_SOURCE /* for sigsetops */
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>

#include <unistd.h>

#include <sys/epoll.h>
#include <sys/signalfd.h>

#include "main.h"
#include "mumble.h"
#include "xcb.h"

static int on_sig_read(struct app_state* state, uint32_t events);

int main(void) {
  struct app_state state;
  sigset_t sigs;
  struct epoll_event event;
  my_epoll_cb sig_cb;

  state.sig_fd = state.mumble_pipe_fd = state.mumble_wait_fd = -1;
  state.mumble_shm_ptr = state.xcb = NULL;

  state.home = getenv("XDG_RUNTIME_DIR");
  if (!state.home) {
    fputs("XDG_RUNTIME_DIR not set, exiting", stderr);
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

  if (setup_mumble(&state) == -1) {
    cleanup(&state);
    return -1;
  }

  sigemptyset(&sigs);
  sigaddset(&sigs, SIGINT);
  state.sig_fd = signalfd(-1, &sigs, SFD_NONBLOCK | SFD_CLOEXEC);
  if (state.sig_fd == -1) {
    perror("signalfd");
    cleanup(&state);
    return -1;
  }
  sigprocmask(SIG_BLOCK, &sigs, NULL);
  sig_cb = &on_sig_read;
  event.events = EPOLLIN;
  event.data.ptr = &sig_cb;
  if (epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.sig_fd, &event) == -1) {
    perror("epoll_ctl (signalfd)");
    cleanup(&state);
    return -1;
  }

  for (;;) {
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
  sigprocmask(SIG_UNBLOCK, &sigs, NULL);
  cleanup(&state);

  return 0;
}

void cleanup(struct app_state* state) {
  if (state->epoll_fd != -1)
    close(state->epoll_fd);
  if (state->sig_fd != -1)
    close(state->sig_fd);
  cleanup_mumble(state);
  cleanup_xcb(state);
}

static int on_sig_read(struct app_state* state, uint32_t events) {
  struct signalfd_siginfo info;
  ssize_t ret;
  sigset_t sigs;

  ret = read(state->sig_fd, &info, sizeof info);
  if (ret == -1) {
    perror("read (signalfd)");
  }

  sigemptyset(&sigs);
  sigaddset(&sigs, SIGINT);
  sigprocmask(SIG_UNBLOCK, &sigs, NULL);

  return -1;
}
