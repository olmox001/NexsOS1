/*
 * kernel/include/drivers/virtio_gpu.h
 * VirtIO GPU Device Definitions
 */
#ifndef _DRIVERS_VIRTIO_GPU_H
#define _DRIVERS_VIRTIO_GPU_H

#include <kernel/types.h>

#define VIRTIO_GPU_EVENT_DISPLAY (1 << 0)

struct virtio_gpu_config {
  uint32_t events_read;
  uint32_t events_clear;
  uint32_t num_scanouts;
  uint32_t num_capsets;
} __attribute__((packed));

/* Control Queue Command Headers */
enum virtio_gpu_ctrl_type {
  VIRTIO_GPU_UNDEFINED = 0,

  /* 2D Commands */
  VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
  VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
  VIRTIO_GPU_CMD_RESOURCE_UNREF,
  VIRTIO_GPU_CMD_SET_SCANOUT,
  VIRTIO_GPU_CMD_RESOURCE_FLUSH,
  VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
  VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
  VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING,

  /* Success Responses */
  VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
  VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
  VIRTIO_GPU_RESP_OK_CAPSET_INFO,
  VIRTIO_GPU_RESP_OK_CAPSET,
  VIRTIO_GPU_RESP_OK_EDID,

  /* Error Responses */
  VIRTIO_GPU_RESP_ERR_UNSPEC = 0x1200,
  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY,
  VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID,
  VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID,
  VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID,
  VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER,
};

struct virtio_gpu_ctrl_hdr {
  uint32_t type;
  uint32_t flags;
  uint64_t fence_id;
  uint32_t ctx_id;
  uint32_t padding;
} __attribute__((packed));

/* Display Info */
#define VIRTIO_GPU_MAX_SCANOUTS 16

struct virtio_gpu_rect {
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
} __attribute__((packed));

struct virtio_gpu_display_one {
  struct virtio_gpu_rect r;
  uint32_t enabled;
  uint32_t flags;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

/* Resource Create 2D */
enum virtio_gpu_formats {
  VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM = 1,
  VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2,
  VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM = 3,
  VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM = 4,
  VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM = 67,
  VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM = 68,
};

struct virtio_gpu_resource_create_2d {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t format;
  uint32_t width;
  uint32_t height;
} __attribute__((packed));

/* Resource Attach Backing */
struct virtio_gpu_mem_entry {
  uint64_t addr;
  uint32_t length;
  uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t nr_entries;
} __attribute__((packed));

/* Set Scanout */
struct virtio_gpu_set_scanout {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect r;
  uint32_t scanout_id;
  uint32_t resource_id;
} __attribute__((packed));

/* Transfer To Host 2D */
struct virtio_gpu_transfer_to_host_2d {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect r;
  uint64_t offset;
  uint32_t resource_id;
  uint32_t padding;
} __attribute__((packed));

/* Resource Flush */
struct virtio_gpu_resource_flush {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect r;
  uint32_t resource_id;
  uint32_t padding;
} __attribute__((packed));

/* Resource Unref (release a host resource on mode change) */
struct virtio_gpu_resource_unref {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t padding;
} __attribute__((packed));

/* Driver API */
void virtio_gpu_init(void);

/* Public Framebuffer Info */
struct gpu_framebuffer {
  uint32_t width;
  uint32_t height;
  uint32_t stride;      /* Bytes per line */
  uint32_t bpp;         /* Bits per pixel (32) */
  void *base_addr;      /* Virtual Address */
  uint32_t resource_id; /* VirtIO resource ID */
};

/* Global instance */
extern struct gpu_framebuffer g_fb;

/* Flush region to screen */
void virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

#endif /* _DRIVERS_VIRTIO_GPU_H */
