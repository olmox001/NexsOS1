/*
 * user/bin/nxres.c
 * NEXS display-resolution tool (GFX-DYN-01).
 *
 * Usage:
 *   nxres                 print the current desktop resolution
 *   nxres <width> <height>  set the desktop resolution at runtime
 *
 * Exercises the full dynamic-display chain from userland: SYS_SET_DISPLAY_MODE
 * -> gpu_set_mode() (virtio-gpu recreates the scanout) -> compositor_resize()
 * (the desktop/backbuffer retargets and every window is kept on-screen).  This
 * is the guaranteed resize path when the host cannot drive a window resize
 * (e.g. headless / fixed display); the QEMU window resize path is handled
 * automatically by init's display poll.
 */
#include <os1.h>

int main(int argc, char **argv) {
  if (argc < 3) {
    long info = _sys_display_info();
    int w = (int)((info >> 16) & 0xFFFF);
    int h = (int)(info & 0xFFFF);
    printf("nxres: current desktop %dx%d\n", w, h);
    printf("usage: nxres <width> <height>   (e.g. nxres 1280 720)\n");
    return 0;
  }

  int w = atoi(argv[1]);
  int h = atoi(argv[2]);
  if (w <= 0 || h <= 0) {
    printf("nxres: invalid size '%s x %s'\n", argv[1], argv[2]);
    return 1;
  }

  int r = _sys_set_display_mode(w, h);
  if (r == 0) {
    printf("nxres: display set to %dx%d\n", w, h);
    return 0;
  }
  printf("nxres: failed to set %dx%d (err %d)\n", w, h, r);
  return 1;
}
