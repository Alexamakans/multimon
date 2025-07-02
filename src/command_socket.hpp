#pragma once

#include <cstdio>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>

#define SOCKET_PATH "/tmp/viture_ar.sock"

// After creating socket `sockfd` as before:
static int make_socket_non_blocking(int sockfd) {
  int flags = fcntl(sockfd, F_GETFL, 0);
  if (flags == -1)
    return -1;
  return fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
}

static int setup_command_socket() {
  int sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return -1;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
  unlink(SOCKET_PATH);

  if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    perror("bind");
    close(sockfd);
    return -1;
  }

  return sockfd;
}

static void (*on_align_command)(void) = nullptr;
static void (*on_push_command)(void) = nullptr;
static void (*on_pop_command)(void) = nullptr;
static void (*on_zoom_in_command)(void) = nullptr;
static void (*on_zoom_out_command)(void) = nullptr;
static void (*on_shift_left_command)(void) = nullptr;
static void (*on_shift_right_command)(void) = nullptr;
static void (*on_toggle_center_dot_command)(void) = nullptr;

static void poll_commands(int sockfd) {
  char buf[256];

  ssize_t len = recv(sockfd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
  if (len > 0) {
    buf[len] = '\0';
    std::string cmd(buf);

    if (cmd == "align") {
      if (on_align_command != nullptr) {
        on_align_command();
      }
    } else if (cmd == "push") {
      if (on_push_command != nullptr) {
        on_push_command();
      }
    } else if (cmd == "pop") {
      if (on_pop_command != nullptr) {
        on_pop_command();
      }
    } else if (cmd == "zoom_in") {
      if (on_zoom_in_command != nullptr) {
        on_zoom_in_command();
      }
    } else if (cmd == "zoom_out") {
      if (on_zoom_out_command != nullptr) {
        on_zoom_out_command();
      }
    } else if (cmd == "shift_left") {
      if (on_shift_left_command != nullptr) {
        on_shift_left_command();
      }
    } else if (cmd == "shift_right") {
      if (on_shift_right_command != nullptr) {
        on_shift_right_command();
      }
    } else if (cmd == "center_dot_toggle") {
      if (on_toggle_center_dot_command != nullptr) {
        on_toggle_center_dot_command();
      }
    }
  }
}

static void destroy_command_socket(int sockfd) {
  close(sockfd);
  unlink(SOCKET_PATH);
}
