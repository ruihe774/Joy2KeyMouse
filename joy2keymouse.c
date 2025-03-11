#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <linux/limits.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>

const char *evdev_dir = "/dev/input";

[[nodiscard]] static int64_t get_time(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return ts.tv_sec * 1'000'000'000ll + ts.tv_nsec;
}

[[nodiscard]] static struct libevdev *find_gamepad(void) {
  for (int i = 0; i < 32; ++i) {
    char path[32];
    sprintf(path, "%s/event%d", evdev_dir, i);

    const int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
      break;

    struct libevdev *dev;
    if (libevdev_new_from_fd(fd, &dev) < 0)
      goto close_fd;

    if (libevdev_has_event_type(dev, EV_ABS) &&
        libevdev_has_event_code(dev, EV_ABS, ABS_X) &&
        libevdev_has_event_code(dev, EV_ABS, ABS_Y) &&
        libevdev_has_event_code(dev, EV_ABS, ABS_RX) &&
        libevdev_has_event_code(dev, EV_ABS, ABS_RY) &&
        libevdev_has_event_code(dev, EV_KEY, BTN_A))
      return dev;

    libevdev_free(dev);
  close_fd:
    close(fd);
  }

  return nullptr;
}

[[nodiscard]] static struct libevdev_uinput *create_virtual_device() {
  struct libevdev *dev = libevdev_new();
  libevdev_set_name(dev, "Joy2KeyMouse Virtual Input");

  libevdev_enable_event_type(dev, EV_REL);
  libevdev_enable_event_code(dev, EV_REL, REL_X, nullptr);
  libevdev_enable_event_code(dev, EV_REL, REL_Y, nullptr);
  libevdev_enable_event_code(dev, EV_REL, REL_WHEEL_HI_RES, nullptr);
  libevdev_enable_event_code(dev, EV_REL, REL_HWHEEL_HI_RES, nullptr);

  libevdev_enable_event_type(dev, EV_KEY);
  libevdev_enable_event_code(dev, EV_KEY, BTN_LEFT, nullptr);
  libevdev_enable_event_code(dev, EV_KEY, BTN_RIGHT, nullptr);
  libevdev_enable_event_code(dev, EV_KEY, BTN_SIDE, nullptr);
  libevdev_enable_event_code(dev, EV_KEY, BTN_EXTRA, nullptr);
  libevdev_enable_event_code(dev, EV_KEY, KEY_TAB, nullptr);
  libevdev_enable_event_code(dev, EV_KEY, KEY_A, nullptr);
  libevdev_enable_event_code(dev, EV_KEY, KEY_LEFTMETA, nullptr);
  libevdev_enable_event_code(dev, EV_KEY, KEY_LEFTSHIFT, nullptr);
  libevdev_enable_event_code(dev, EV_KEY, KEY_LEFTCTRL, nullptr);
  libevdev_enable_event_code(dev, EV_KEY, KEY_LEFTALT, nullptr);
  libevdev_enable_event_code(dev, EV_KEY, KEY_ENTER, nullptr);
  libevdev_enable_event_code(dev, EV_KEY, KEY_LEFT, nullptr);
  libevdev_enable_event_code(dev, EV_KEY, KEY_RIGHT, nullptr);
  libevdev_enable_event_code(dev, EV_KEY, KEY_UP, nullptr);
  libevdev_enable_event_code(dev, EV_KEY, KEY_DOWN, nullptr);

  struct libevdev_uinput *uinput;
  const int rc = libevdev_uinput_create_from_device(
      dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &uinput);
  libevdev_free(dev);

  if (rc < 0) {
    errno = -rc;
    return nullptr;
  }
  return uinput;
}

int main(void) {
  sigset_t sigmask;
  sigemptyset(&sigmask);
  sigaddset(&sigmask, SIGINT);
  sigaddset(&sigmask, SIGTERM);
  sigprocmask(SIG_BLOCK, &sigmask, nullptr);
  const int signal_fd = signalfd(-1, &sigmask, SFD_NONBLOCK);
  if (signal_fd < 0)
    err(EXIT_FAILURE, "failed to create signalfd");

  struct libevdev_uinput *uinput = create_virtual_device();
  if (uinput == nullptr)
    err(EXIT_FAILURE, "failed to create virtual device");
  warnx("created virtual device: %s", libevdev_uinput_get_devnode(uinput));

  const int ino_fd = inotify_init1(IN_NONBLOCK);
  if (ino_fd < 0)
    err(EXIT_FAILURE, "failed to init inotify");
  if (inotify_add_watch(ino_fd, evdev_dir, IN_CREATE) < 0)
    err(EXIT_FAILURE, "failed to monitor evdev");

  for (bool quit = false;;) {
    struct pollfd poll_fds[] = {
        (struct pollfd){.fd = -1, .events = POLLIN},
        (struct pollfd){.fd = signal_fd, .events = POLLIN}};

    struct libevdev *gamepad = find_gamepad();
    if (gamepad == nullptr)
      goto wait_hotplug;
    const int gamepad_fd = poll_fds[0].fd = libevdev_get_fd(gamepad);
    warnx("gamepad found: %s", libevdev_get_name(gamepad));

    int lx = 0, ly = 0, rx = 0, ry = 0, slx = 0, sly = 0, srx = 0, sry = 0;
    int hat0x = 0, hat0y = 0;
    bool lz = false, rz = false;
    const double lbase = 1.01;
    const int64_t ldiv1 = 1ll << 9, lsub = 1ll << 13, ldiv2 = 1ll << 36,
                  lmul = 1;
    const double rbase = 1.01;
    const int64_t rdiv1 = 1ll << 9, rsub = 1ll << 13, rdiv2 = 1ll << 36,
                  rmul = 2;
    const int z_down_threshold = 512, z_up_threshold = 256;
    int64_t last_time = get_time();

    while (true) {
      const int rc = poll(poll_fds, 2, slx || sly || srx || sry ? 20 : -1);
      const int64_t current_time = get_time();
      const int64_t interval = current_time - last_time;
      last_time = current_time;

      if (rc < 0)
        err(EXIT_FAILURE, "poll failed");

      if (poll_fds[1].revents) {
        warnx("exiting");
        quit = true;
        break;
      }

      if (poll_fds[0].revents) {
        struct input_event ev;
        const int rc =
            libevdev_next_event(gamepad, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        if (rc < 0) {
          if (rc == -ENODEV) {
            warnx("gamepad disconnected");
            break;
          }
          errno = -rc;
          err(EXIT_FAILURE, "failed to read event");
        }
        switch (rc) {
        case LIBEVDEV_READ_STATUS_SUCCESS:
          break;
        case LIBEVDEV_READ_STATUS_SYNC:
          warnx("some events have been dropped by kernel");
          break;
        default:
          warnx("libevdev_next_event returned unknown error");
          break;
        }

        switch (ev.type) {
        case EV_ABS:
          switch (ev.code) {
          case ABS_X:
            lx = ev.value;
            break;
          case ABS_Y:
            ly = ev.value;
            break;
          case ABS_RX:
            rx = ev.value;
            break;
          case ABS_RY:
            ry = ev.value;
            break;
          case ABS_HAT0X:
            if (hat0x && hat0x != ev.value) {
              libevdev_uinput_write_event(uinput, EV_KEY,
                                          hat0x < 0 ? KEY_LEFT : KEY_RIGHT, 0);
              libevdev_uinput_write_event(uinput, EV_SYN, SYN_REPORT, 0);
            }
            hat0x = ev.value;
            if (hat0x) {
              libevdev_uinput_write_event(uinput, EV_KEY,
                                          hat0x < 0 ? KEY_LEFT : KEY_RIGHT, 1);
              libevdev_uinput_write_event(uinput, EV_SYN, SYN_REPORT, 0);
            }
            break;
          case ABS_HAT0Y:
            if (hat0y && hat0y != ev.value) {
              libevdev_uinput_write_event(uinput, EV_KEY,
                                          hat0y < 0 ? KEY_UP : KEY_DOWN, 0);
              libevdev_uinput_write_event(uinput, EV_SYN, SYN_REPORT, 0);
            }
            hat0y = ev.value;
            if (hat0y) {
              libevdev_uinput_write_event(uinput, EV_KEY,
                                          hat0y < 0 ? KEY_UP : KEY_DOWN, 1);
              libevdev_uinput_write_event(uinput, EV_SYN, SYN_REPORT, 0);
            }
            break;
          case ABS_Z:
            if (ev.value > z_down_threshold && !lz) {
              libevdev_uinput_write_event(uinput, EV_KEY, KEY_LEFTCTRL,
                                          lz = true);
              libevdev_uinput_write_event(uinput, EV_SYN, SYN_REPORT, 0);
            }
            if (ev.value < z_up_threshold && lz) {
              libevdev_uinput_write_event(uinput, EV_KEY, KEY_LEFTCTRL,
                                          lz = false);
              libevdev_uinput_write_event(uinput, EV_SYN, SYN_REPORT, 0);
            }
            break;
          case ABS_RZ:
            if (ev.value > z_down_threshold && !rz) {
              libevdev_uinput_write_event(uinput, EV_KEY, KEY_LEFTSHIFT,
                                          rz = true);
              libevdev_uinput_write_event(uinput, EV_SYN, SYN_REPORT, 0);
            }
            if (ev.value < z_up_threshold && rz) {
              libevdev_uinput_write_event(uinput, EV_KEY, KEY_LEFTSHIFT,
                                          rz = false);
              libevdev_uinput_write_event(uinput, EV_SYN, SYN_REPORT, 0);
            }
            break;
          }
          break;

        case EV_KEY:
          switch (ev.code) {
          case BTN_SOUTH:
            libevdev_uinput_write_event(uinput, EV_KEY, BTN_LEFT, ev.value);
            break;
          case BTN_EAST:
            libevdev_uinput_write_event(uinput, EV_KEY, BTN_RIGHT, ev.value);
            break;
          case BTN_SELECT:
            libevdev_uinput_write_event(uinput, EV_KEY, KEY_LEFTMETA, ev.value);
            break;
          case BTN_START:
            libevdev_uinput_write_event(uinput, EV_KEY, KEY_LEFTMETA, ev.value);
            libevdev_uinput_write_event(uinput, EV_KEY, KEY_A, ev.value);
            break;
          case BTN_TR:
            libevdev_uinput_write_event(uinput, EV_KEY, KEY_TAB, ev.value);
            break;
          case BTN_TL:
            libevdev_uinput_write_event(uinput, EV_KEY, KEY_LEFTSHIFT,
                                        ev.value);
            libevdev_uinput_write_event(uinput, EV_KEY, KEY_TAB, ev.value);
            break;
          case BTN_DPAD_UP:
            libevdev_uinput_write_event(uinput, EV_KEY, KEY_UP, ev.value);
            break;
          case BTN_DPAD_DOWN:
            libevdev_uinput_write_event(uinput, EV_KEY, KEY_DOWN, ev.value);
            break;
          case BTN_DPAD_LEFT:
            libevdev_uinput_write_event(uinput, EV_KEY, KEY_LEFT, ev.value);
            break;
          case BTN_DPAD_RIGHT:
            libevdev_uinput_write_event(uinput, EV_KEY, KEY_RIGHT, ev.value);
            break;
          case BTN_WEST:
            libevdev_uinput_write_event(uinput, EV_KEY, BTN_EXTRA, ev.value);
            break;
          case BTN_NORTH:
            libevdev_uinput_write_event(uinput, EV_KEY, BTN_SIDE, ev.value);
            break;
          case BTN_THUMBL:
            libevdev_uinput_write_event(uinput, EV_KEY, KEY_LEFTALT, ev.value);
            break;
          case BTN_THUMBR:
            libevdev_uinput_write_event(uinput, EV_KEY, KEY_ENTER, ev.value);
            break;
          }
          libevdev_uinput_write_event(uinput, EV_SYN, SYN_REPORT, 0);
          break;
        }
      }

      slx =
          (int)((int64_t)(pow(lbase, (double)((abs(lx) - lsub) / ldiv1)) * lx) *
                interval / ldiv2 * lmul);
      sly =
          (int)((int64_t)(pow(lbase, (double)((abs(ly) - lsub) / ldiv1)) * ly) *
                interval / ldiv2 * lmul);
      if (slx || sly) {
        libevdev_uinput_write_event(uinput, EV_REL, REL_X, slx);
        libevdev_uinput_write_event(uinput, EV_REL, REL_Y, sly);
        libevdev_uinput_write_event(uinput, EV_SYN, SYN_REPORT, 0);
      }

      srx =
          (int)((int64_t)(pow(rbase, (double)((abs(rx) - rsub) / rdiv1)) * rx) *
                interval / rdiv2 * rmul);
      sry =
          (int)((int64_t)(pow(rbase, (double)((abs(ry) - rsub) / rdiv1)) * ry) *
                interval / rdiv2 * rmul);
      if (srx || sry) {
        if (abs(srx) > abs(sry))
          libevdev_uinput_write_event(uinput, EV_REL, REL_HWHEEL_HI_RES, -srx);
        else
          libevdev_uinput_write_event(uinput, EV_REL, REL_WHEEL_HI_RES, sry);
        libevdev_uinput_write_event(uinput, EV_SYN, SYN_REPORT, 0);
      }
    }

    libevdev_free(gamepad);
    close(gamepad_fd);
    if (quit)
      break;
  wait_hotplug:
    poll_fds[0].fd = ino_fd;
    while (true) {
      const int rc = poll(poll_fds, 2, -1);

      if (rc < 0)
        err(EXIT_FAILURE, "poll failed");

      if (poll_fds[1].revents) {
        warnx("exiting");
        goto outer;
      }

      if (poll_fds[0].revents) {
        while (true) {
          char buf[sizeof(struct inotify_event) + NAME_MAX + 1];
          if (read(ino_fd, buf, sizeof(buf)) < 0) {
            if (errno == EAGAIN)
              break;
            err(EXIT_FAILURE, "failed to read inotify");
          }
        }
        break;
      }
    }
  }

outer:
  close(ino_fd);
  libevdev_uinput_destroy(uinput);
  return EXIT_SUCCESS;
}
