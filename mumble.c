#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <linux/limits.h>
#include <unistd.h>
#include <fcntl.h>

#define UNIX_PATH_MAX (sizeof(((struct sockaddr_un*) 0)->sun_path))

#include "mumble.h"
#include "xcb.h"

#define MUMBLE_PIPE_FILENAME ".MumbleOverlayPipe"

enum read_status {
  READ_DONE,
  READ_EOF,
  READ_ERROR,
  READ_AGAIN
};

static int inotify_init_watch_creates(const char* dir);
static int on_mumble_wait_read(struct app_state* state, uint32_t events);
static int open_unix_socket(const char* path);
static int send_mumble_init_msg(int sock, uint16_t width, uint16_t height);
static int get_mumble_pipe_path(char* buf, const char* home);
static void inspect_msg(struct OverlayMsg* msg);
static enum read_status read_n(int fd, size_t* filled, void* buf, size_t size);
static void* open_mumble_shm(size_t mmap_size, const char* name);
static int handle_mumble_msg(struct app_state* state);
static int reopen_mumble(struct app_state* state);
static int on_mumble_read(struct app_state* state, uint32_t events);

static my_epoll_cb mumble_cb = &on_mumble_read;
static my_epoll_cb mumble_wait_cb = &on_mumble_wait_read;

int setup_mumble(struct app_state* state) {
  struct epoll_event event;

  if ((state->mumble_pipe_fd = open_unix_socket(state->home)) != -1) {
    if (send_mumble_init_msg(state->mumble_pipe_fd,
                             state->screen_res_width,
                             state->screen_res_height) == -1) {
      perror("sending init msg");
      return -1;
    }

    if (fcntl(state->mumble_pipe_fd, F_SETFL, O_NONBLOCK, 1) == -1) {
      perror("fcntl");
      return -1;
    }

    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.ptr = &mumble_cb;
    if (epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD,
                  state->mumble_pipe_fd, &event) == -1) {
      perror("epoll_ctl (mumble)");
      return -1;
    }

    state->mumble_msg_read = 0;
    state->mumble_active_x =
      state->mumble_active_y =
      state->mumble_active_w =
      state->mumble_active_h = 0;
    return 0;
  } else {
    if (errno == ECONNREFUSED || errno == ENOENT) {
      puts("can't find mumble socket, waiting until mumble starts...");

      if ((state->mumble_wait_fd =
            inotify_init_watch_creates(state->home)) == -1)
        return -1;

      event.events = EPOLLIN | EPOLLRDHUP;
      event.data.ptr = &mumble_wait_cb;
      if (epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD,
                    state->mumble_wait_fd, &event) == -1) {
        perror("epoll_ctl (inotify)");
        return -1;
      }

      return 0;
    } else {
      perror("couldn't open mumble socket");
      return -1;
    }
  }
}

static int inotify_init_watch_creates(const char* dir) {
  int inotify;

  if ((inotify = inotify_init()) == -1) {
    perror("inotify_init");
    return -1;
  }

  if (inotify_add_watch(inotify, dir, IN_CREATE) == -1) {
    perror("inotify_add_watch");
    close(inotify);
    return -1;
  }

  return inotify;
}

static int on_mumble_wait_read(struct app_state* state, uint32_t events) {
  ssize_t ret;
  struct inotify_event* event;
  /* NAME_MAX is like 255. inotify manpage suggests this size is enough to
     receive at least one inotify event, and a smaller size would possibly
     result in errors. of course we might still get multiple events squeezed
     within this size of buffer...*/
  union {
    char buf[sizeof *event + NAME_MAX + 1];
    struct inotify_event first_event;
  } u;

  for (;;) {
    event = &u.first_event;
    ret = read(state->mumble_wait_fd, event, sizeof u.buf);
    if (ret < (ssize_t) sizeof *event) {
      if (errno == EINTR)
        continue;
      close(state->mumble_wait_fd);
      state->mumble_wait_fd = -1;
      return -1;
    }

    for (;;) {
      size_t chunk_size;
      if (strcmp(MUMBLE_PIPE_FILENAME, event->name) == 0) {
        close(state->mumble_wait_fd);
        state->mumble_wait_fd = -1;

        return setup_mumble(state);
      }

      chunk_size = sizeof *event + event->len;
      if ((size_t) ret <= chunk_size)
        break;

      ret -= (ssize_t) chunk_size;
      event += chunk_size;
    }
  }
  assert(0);
}

static int open_unix_socket(const char* path) {
  int sock;
  struct sockaddr_un addr;

  addr.sun_family = AF_UNIX;
  if (get_mumble_pipe_path(addr.sun_path, path) == -1)
    return -1;

  sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock == -1)
    return -1;

  if (connect(sock, (struct sockaddr*) &addr, sizeof addr) == -1) {
    close(sock);
    return -1;
  }

  return sock;
}

static int send_mumble_init_msg(int sock, uint16_t width, uint16_t height) {
  size_t msg_written, msgsize;
  struct OverlayMsg msg;
  ssize_t ret;

  msg.omh.uiMagic = OVERLAY_MAGIC_NUMBER;
  msg.omh.uiType = OVERLAY_MSGTYPE_INIT;
  msg.omh.iLength = sizeof(struct OverlayMsgInit);
  msg.body.omi.uiWidth = width;
  msg.body.omi.uiHeight = height;

  msgsize = sizeof(struct OverlayMsgHeader) + sizeof(struct OverlayMsgInit);
  msg_written = 0;
  while (msg_written < msgsize) {
    ret = write(sock, &msg, msgsize - msg_written);
    if ((ret == -1 && (errno != EAGAIN && errno != EINTR)) || ret == 0) {
      close(sock);
      return -1;
    }
    msg_written += (size_t) ret;
  }
  return 0;
}

static int get_mumble_pipe_path(char* buf, const char* home) {
  static const char FILENAME_BIT[] = "/" MUMBLE_PIPE_FILENAME;
  size_t len;

  strcpy(buf, home);
  len = strlen(buf);
  if (len + sizeof(FILENAME_BIT) > UNIX_PATH_MAX) {
    errno = ENAMETOOLONG;
    return -1;
  }

  strcpy(buf + len, FILENAME_BIT);

  return 0;
}

static void inspect_msg(struct OverlayMsg* msg) {
  fputs("Mumble: ", stdout);
  switch (msg->omh.uiType) {
    case OVERLAY_MSGTYPE_INIT:
      printf("Init(%u, %u)\n", msg->body.omi.uiWidth, msg->body.omi.uiHeight);
      break;
    case OVERLAY_MSGTYPE_SHMEM:
      printf("Shmem(%s)\n", msg->body.oms.a_cName);
      break;
    case OVERLAY_MSGTYPE_BLIT:
      printf("Blit(%u, %u, %u, %u)\n", msg->body.omb.x, msg->body.omb.y, msg->body.omb.w, msg->body.omb.h);
      break;
    case OVERLAY_MSGTYPE_ACTIVE:
      printf("Active(%u, %u, %u, %u)\n", msg->body.oma.x, msg->body.oma.y, msg->body.oma.w, msg->body.oma.h);
      break;
    case OVERLAY_MSGTYPE_PID:
      printf("Pid(%u)\n", msg->body.omp.pid);
      break;
    case OVERLAY_MSGTYPE_FPS:
      printf("Fps(%f)\n", (double) msg->body.omf.fps);
      break;
    case OVERLAY_MSGTYPE_INTERACTIVE:
      printf("Interactive(%s)\n", msg->body.omin.state ? "true" : "false");
      break;
    default:
      printf("??? %u %d\n", msg->omh.uiType, msg->omh.iLength);
      break;
  }
}

static enum read_status read_n(int fd, size_t* filled, void* buf, size_t size) {
  ssize_t ret;
  if (*filled > size)
    return READ_DONE;

  ret = read(fd, (char*) buf + *filled, size - *filled);
  if (ret == -1) {
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
      return READ_AGAIN;

    return READ_ERROR;
  } else if (ret == 0) {
    errno = 0;
    return READ_EOF;
  }

  *filled += (size_t) ret;
  if (*filled == size)
    return READ_DONE;

  return READ_AGAIN;
}

static void* open_mumble_shm(size_t mmap_size, const char* name) {
  int fd;
  void* ptr;
  int my_errno;

  fd = shm_open(name, O_RDONLY, 0600);
  if (fd == -1) {
    perror("shm_open");
    return NULL;
  }

  ptr = mmap(NULL, mmap_size, PROT_READ, MAP_SHARED, fd, 0);
  my_errno = errno;
  close(fd);
  errno = my_errno;

  if (ptr == 0) {
    perror("mmap");
    return NULL;
  }

  return ptr;
}

static int handle_mumble_msg(struct app_state* state) {
  switch (state->mumble_msg.omh.uiType) {
    case OVERLAY_MSGTYPE_INIT:
      break;
    case OVERLAY_MSGTYPE_SHMEM: {
      size_t mmap_size =
          (size_t) 4 * state->screen_res_width * state->screen_res_height;
      if (state->mumble_shm_ptr)
        munmap(state->mumble_shm_ptr, mmap_size);
      state->mumble_shm_ptr =
          open_mumble_shm(mmap_size, state->mumble_msg.body.oms.a_cName);
      if (state->mumble_shm_ptr == NULL) {
        return -1;
      } else {
        return 0;
      }
    }
    case OVERLAY_MSGTYPE_BLIT:
      blit(state);
      break;
    case OVERLAY_MSGTYPE_ACTIVE:
      state->mumble_active_x = (uint16_t) state->mumble_msg.body.oma.x;
      state->mumble_active_y = (uint16_t) state->mumble_msg.body.oma.y;
      state->mumble_active_w = (uint16_t) state->mumble_msg.body.oma.w;
      state->mumble_active_h = (uint16_t) state->mumble_msg.body.oma.h;
      move_resize(state);
      blit(state);
      break;
    case OVERLAY_MSGTYPE_PID:
      break;
    case OVERLAY_MSGTYPE_FPS:
      break;
    case OVERLAY_MSGTYPE_INTERACTIVE:
      break;
    default:
      break;
  }
  return 0;
}

void cleanup_mumble(struct app_state* state) {
  if (state->mumble_pipe_fd != -1) {
    close(state->mumble_pipe_fd);
    state->mumble_pipe_fd = -1;
  }
  if (state->mumble_wait_fd != -1) {
    close(state->mumble_wait_fd);
    state->mumble_wait_fd = -1;
  }
  if (state->mumble_shm_ptr) {
    size_t mmap_size =
      (size_t) 4 * state->screen_res_width * state->screen_res_height;
    munmap(state->mumble_shm_ptr, mmap_size);
    state->mumble_shm_ptr = NULL;
  }
}

static int reopen_mumble(struct app_state* state) {
  state->mumble_active_w = state->mumble_active_h = 0;
  if (state->xcb)
    move_resize(state);

  cleanup_mumble(state);
  return setup_mumble(state);
}

static int on_mumble_read(struct app_state* state, uint32_t events) {
  for (;;) {
    size_t msgsize;
    switch (read_n(state->mumble_pipe_fd, &state->mumble_msg_read,
                   &state->mumble_msg, sizeof(struct OverlayMsgHeader))) {
      case READ_ERROR:
        perror("can't read mumble msg header");
        /* fall through */
      case READ_EOF:
        fputs("mumble socket closed, reopening...\n", stderr);
        return reopen_mumble(state);
      case READ_AGAIN:
        return 0;
      case READ_DONE:
        break;
    }
    msgsize = sizeof(struct OverlayMsgHeader)
              + (size_t) state->mumble_msg.omh.iLength;
    switch (read_n(state->mumble_pipe_fd, &state->mumble_msg_read,
                   &state->mumble_msg, msgsize)) {
      case READ_ERROR:
        perror("can't read mumble msg body");
        /* fall through */
      case READ_EOF:
        fputs("mumble socket closed, reopening...\n", stderr);
        return reopen_mumble(state);
      case READ_AGAIN:
        return 0;
      case READ_DONE:
        break;
    }

    state->mumble_msg_read = 0;
    inspect_msg(&state->mumble_msg);
    if (handle_mumble_msg(state) == -1)
      return reopen_mumble(state);
  }
}
