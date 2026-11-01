#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <rfb/rfbclient.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "virtio-gpu-metadata-v1-client-protocol.h"

#define BUFFER_COUNT 4

struct buffer {
  struct wl_buffer* handle;
  uint32_t* pixels;
  int busy;
};

struct bridge {
  struct wl_display* display;
  struct wl_compositor* compositor;
  struct wl_shm* shm;
  struct wp_virtio_gpu_metadata_v1* metadata;
  struct wl_surface* surface;
  struct wp_virtio_gpu_surface_metadata_v1* surface_metadata;
  struct buffer buffers[BUFFER_COUNT];
  int width;
  int height;
  int frame_dirty;
  int resize_pending;
};

static struct bridge* current_bridge;

static void registry_global(void* data, struct wl_registry* registry,
                            uint32_t name, const char* interface,
                            uint32_t version) {
  struct bridge* bridge = data;
  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    bridge->compositor = wl_registry_bind(
        registry, name, &wl_compositor_interface, version < 3 ? version : 3);
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    bridge->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  } else if (strcmp(interface, wp_virtio_gpu_metadata_v1_interface.name) == 0) {
    bridge->metadata = wl_registry_bind(
        registry, name, &wp_virtio_gpu_metadata_v1_interface, 1);
  }
}

static void registry_global_remove(void* data, struct wl_registry* registry,
                                   uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void buffer_release(void* data, struct wl_buffer* handle) {
  struct buffer* buffer = data;
  (void)handle;
  buffer->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void frame_buffer_update(rfbClient* client, int x, int y, int w, int h) {
  (void)client;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  current_bridge->frame_dirty = 1;
}

static rfbBool allocate_frame_buffer(rfbClient* client) {
  size_t size = (size_t)client->width * client->height * 4;
  void* pixels = realloc(client->frameBuffer, size);
  if (!pixels) return FALSE;
  memset(pixels, 0, size);
  client->frameBuffer = pixels;
  current_bridge->resize_pending = 1;
  return TRUE;
}

static int connect_wayland(struct bridge* bridge, const char* socket_path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket(AF_UNIX)");
    return -1;
  }
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  if (strlen(socket_path) >= sizeof(address.sun_path)) {
    fprintf(stderr, "Wayland socket path is too long\n");
    close(fd);
    return -1;
  }
  strcpy(address.sun_path, socket_path);
  if (connect(fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
    perror("connect(frames.sock)");
    close(fd);
    return -1;
  }
  bridge->display = wl_display_connect_to_fd(fd);
  if (!bridge->display) {
    perror("wl_display_connect_to_fd");
    close(fd);
    return -1;
  }
  struct wl_registry* registry = wl_display_get_registry(bridge->display);
  wl_registry_add_listener(registry, &registry_listener, bridge);
  if (wl_display_roundtrip(bridge->display) < 0) {
    fprintf(stderr, "Wayland registry roundtrip failed\n");
    return -1;
  }
  wl_registry_destroy(registry);
  if (!bridge->compositor || !bridge->shm || !bridge->metadata) {
    fprintf(stderr, "Required Wayland globals are unavailable\n");
    return -1;
  }
  return 0;
}

static int create_buffers(struct bridge* bridge) {
  size_t size = (size_t)bridge->width * bridge->height * 4;
  for (int index = 0; index < BUFFER_COUNT; index++) {
    struct buffer* buffer = &bridge->buffers[index];
    int fd = memfd_create("cf-vnc-frame", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) {
      perror("memfd_create/ftruncate");
      return -1;
    }
    buffer->pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buffer->pixels == MAP_FAILED) {
      perror("mmap");
      close(fd);
      return -1;
    }
    struct wl_shm_pool* pool = wl_shm_create_pool(bridge->shm, fd, size);
    // webRTC reinterprets these RGBA bytes as DRM_FORMAT_ABGR8888.
    buffer->handle = wl_shm_pool_create_buffer(
        pool, 0, bridge->width, bridge->height, bridge->width * 4,
        WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!buffer->handle) {
      fprintf(stderr, "wl_shm_pool_create_buffer failed\n");
      return -1;
    }
    wl_buffer_add_listener(buffer->handle, &buffer_listener, buffer);
  }
  return 0;
}

static void destroy_surface(struct bridge* bridge) {
  size_t size = (size_t)bridge->width * bridge->height * 4;
  for (int index = 0; index < BUFFER_COUNT; index++) {
    struct buffer* buffer = &bridge->buffers[index];
    if (buffer->handle) wl_buffer_destroy(buffer->handle);
    if (buffer->pixels) munmap(buffer->pixels, size);
    memset(buffer, 0, sizeof(*buffer));
  }
  if (bridge->surface_metadata)
    wl_proxy_destroy((struct wl_proxy*)bridge->surface_metadata);
  if (bridge->surface) wl_surface_destroy(bridge->surface);
  bridge->surface_metadata = NULL;
  bridge->surface = NULL;
}

static int create_surface(struct bridge* bridge) {
  bridge->surface = wl_compositor_create_surface(bridge->compositor);
  struct wl_region* region = wl_compositor_create_region(bridge->compositor);
  wl_region_add(region, 0, 0, bridge->width, bridge->height);
  wl_surface_set_opaque_region(bridge->surface, region);
  wl_region_destroy(region);
  bridge->surface_metadata = wp_virtio_gpu_metadata_v1_get_surface_metadata(
      bridge->metadata, bridge->surface);
  wp_virtio_gpu_surface_metadata_v1_set_scanout_id(bridge->surface_metadata, 0);
  if (create_buffers(bridge) < 0) return -1;
  if (wl_display_roundtrip(bridge->display) < 0) {
    fprintf(stderr, "Wayland surface setup failed\n");
    return -1;
  }
  return 0;
}

static int64_t milliseconds(void) {
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static int submit_frame(struct bridge* bridge, rfbClient* client) {
  struct buffer* buffer = NULL;
  for (int index = 0; index < BUFFER_COUNT; index++) {
    if (!bridge->buffers[index].busy) {
      buffer = &bridge->buffers[index];
      break;
    }
  }
  if (!buffer) return -1;

  const size_t pixels = (size_t)bridge->width * bridge->height;
  const uint32_t* source = (const uint32_t*)client->frameBuffer;
  for (size_t index = 0; index < pixels; index++) {
    buffer->pixels[index] = source[index] | 0xff000000u;
  }
  wl_surface_attach(bridge->surface, buffer->handle, 0, 0);
  wl_surface_damage(bridge->surface, 0, 0, bridge->width, bridge->height);
  wl_surface_commit(bridge->surface);
  buffer->busy = 1;
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 5) {
    fprintf(stderr, "Usage: %s FRAMES_SOCK VNC_DISPLAY WIDTH HEIGHT\n", argv[0]);
    return 2;
  }
  const int expected_width = atoi(argv[3]);
  const int expected_height = atoi(argv[4]);
  struct bridge bridge = {0};
  current_bridge = &bridge;

  rfbClient* client = rfbGetClient(8, 3, 4);
  client->programName = argv[0];
  client->format.bitsPerPixel = 32;
  client->format.depth = 24;
  client->format.bigEndian = 0;
  client->format.trueColour = 1;
  client->format.redMax = 255;
  client->format.greenMax = 255;
  client->format.blueMax = 255;
  client->format.redShift = 0;
  client->format.greenShift = 8;
  client->format.blueShift = 16;
  client->GotFrameBufferUpdate = frame_buffer_update;
  client->MallocFrameBuffer = allocate_frame_buffer;
  client->canHandleNewFBSize = TRUE;

  char* vnc_argv[] = {argv[0], argv[2], NULL};
  int vnc_argc = 2;
  if (!rfbInitClient(client, &vnc_argc, vnc_argv)) {
    fprintf(stderr, "Unable to connect to QEMU VNC at %s\n", argv[2]);
    return 1;
  }
  bridge.width = client->width;
  bridge.height = client->height;
  if (bridge.width <= 0 || bridge.height <= 0) {
    fprintf(stderr, "Invalid VNC framebuffer dimensions\n");
    rfbClientCleanup(client);
    return 1;
  }
  fprintf(stderr, "Connected to QEMU VNC: %dx%d\n", bridge.width, bridge.height);
  if (bridge.width != expected_width || bridge.height != expected_height) {
    fprintf(stderr, "Waiting for Android display %dx%d\n", expected_width,
            expected_height);
    rfbClientCleanup(client);
    return 1;
  }

  if (connect_wayland(&bridge, argv[1]) < 0) return 1;
  if (create_surface(&bridge) < 0) return 1;
  bridge.resize_pending = 0;
  fprintf(stderr, "Publishing VNC frames as WebRTC display 0\n");

  int64_t last_frame = 0;
  while (1) {
    while (wl_display_prepare_read(bridge.display) != 0) {
      if (wl_display_dispatch_pending(bridge.display) < 0) goto fail;
    }
    if (wl_display_flush(bridge.display) < 0 && errno != EAGAIN) {
      wl_display_cancel_read(bridge.display);
      perror("wl_display_flush");
      goto fail;
    }
    struct pollfd fds[] = {
        {.fd = wl_display_get_fd(bridge.display), .events = POLLIN},
        {.fd = client->sock, .events = POLLIN},
    };
    int ready = poll(fds, 2, 33);
    if (ready < 0) {
      if (errno == EINTR) {
        wl_display_cancel_read(bridge.display);
        continue;
      }
      wl_display_cancel_read(bridge.display);
      perror("poll");
      goto fail;
    }
    if (fds[0].revents & POLLIN) {
      if (wl_display_read_events(bridge.display) < 0) goto fail;
    } else {
      wl_display_cancel_read(bridge.display);
    }
    if (wl_display_dispatch_pending(bridge.display) < 0) goto fail;
    if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      fprintf(stderr, "QEMU VNC connection closed\n");
      goto fail;
    }
    if ((fds[1].revents & POLLIN) && !HandleRFBServerMessage(client)) {
      fprintf(stderr, "QEMU VNC message handling failed\n");
      goto fail;
    }
    if (bridge.resize_pending) {
      bridge.resize_pending = 0;
      if (bridge.width != client->width || bridge.height != client->height) {
        destroy_surface(&bridge);
        bridge.width = client->width;
        bridge.height = client->height;
        if (create_surface(&bridge) < 0) goto fail;
        fprintf(stderr, "Resized WebRTC display 0 to %dx%d\n", bridge.width,
                bridge.height);
      }
    }
    int64_t now = milliseconds();
    if (bridge.frame_dirty && now - last_frame >= 33 &&
        submit_frame(&bridge, client) == 0) {
      bridge.frame_dirty = 0;
      last_frame = now;
    }
  }

fail:
  destroy_surface(&bridge);
  wl_display_roundtrip(bridge.display);
  wl_display_disconnect(bridge.display);
  rfbClientCleanup(client);
  return 1;
}
