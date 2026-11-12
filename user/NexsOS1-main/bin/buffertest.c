/* Cross-process shared-memory regression. No pixel copy or address sharing. */
#include <os1.h>
#include <os1_buffer.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

/* Cross a catalog work quantum so allocation/map/unmap must all continue. */
#define TEST_BYTES ((4096U / sizeof(uint64_t) + 1U) * 4096U)
#define TEST_PATTERN UINT32_C(0x6e785347)
#define CHILD_PATTERN UINT32_C(0x4753786e)

static int child_test(void) {
  struct ipc_message message;
  if (OS1low_ipc_recv(-1, &message)) return 1;
  int handle = (int)message.data1;
  struct os1_buffer_view view;
  if (OS1_buffer_map(handle, 1, &view)) return 2;
  volatile uint32_t *words = view.address;
  if (words[0] != TEST_PATTERN || words[TEST_BYTES / 4U - 1U] != TEST_PATTERN)
    return 3;
  words[1] = CHILD_PATTERN;
  /* Closing the last handle in this process must not invalidate its mapping. */
  if (OS1low_handle_close(handle)) return 4;
  words[2] = CHILD_PATTERN;
  message.type = 0x411;
  if (OS1low_ipc_send(message.from, &message)) return 5;
  /* Intentionally leave the mapping: process teardown must detach it safely. */
  return 0;
}

static int rights_test(int handle) {
  long read_only = OS1low_handle_duplicate(handle, OS1_RIGHT_READ);
  if (read_only < 0) return 1;
  struct os1_buffer_view view;
  int denied = OS1_buffer_map((int)read_only, 1, &view) == -EPERM;
  int mapped = OS1_buffer_map((int)read_only, 0, &view) == 0;
  int contents = mapped && ((const uint32_t *)view.address)[0] == TEST_PATTERN;
  int unmapped = mapped && OS1_buffer_unmap((int)read_only, &view) == 0;
  int closed = OS1low_handle_close((int)read_only) == 0;
  return !(denied && contents && unmapped && closed);
}

static int descriptor_test(int handle) {
  struct os1_buffer_request_v1 request = {0};
  request.abi_version = OS1_SURFACE_ABI_VERSION;
  request.struct_size = sizeof(request);
  request.operation = OS1_BUFFER_MAP;
  request.address = UINT64_C(0xffff000000001000);
  if (OS1_object_read(handle, &request, sizeof(request)) != -EINVAL) return 1;
  request.address = 0;
  request.page_offset = 1;
  if (OS1_object_read(handle, &request, sizeof(request)) != -EINVAL) return 2;
  request.page_offset = 0;
  request.abi_version = UINT32_MAX;
  if (OS1_object_read(handle, &request, sizeof(request)) != -EINVAL) return 3;
  return OS1_buffer_create(SIZE_MAX) >= 0 ? 4 : 0;
}

static int parent_test(void) {
  long handle = OS1_buffer_create(TEST_BYTES);
  if (handle < 0) { printf("[BUFFERTEST] create=%ld\n", handle); return 1; }
  struct os1_buffer_view view;
  int result = OS1_buffer_map((int)handle, 1, &view);
  if (result) { printf("[BUFFERTEST] map=%d\n", result); return 2; }
  volatile uint32_t *words = view.address;
  for (unsigned i = 0; i < TEST_BYTES / 4U; ++i) words[i] = TEST_PATTERN;
  if (rights_test((int)handle)) return 3;
  if (descriptor_test((int)handle)) return 12;
  char *args[] = {(char *)"/bin/buffertest", (char *)"--child"};
  long child = OS1low_process_spawn(args[0], 2, args);
  if (child < 0) return 4;
  long granted = OS1low_cap_grant((int)child, (int)handle,
                                  OS1_RIGHT_READ | OS1_RIGHT_WRITE);
  if (granted < 0) return 5;
  struct ipc_message message = {0};
  message.type = 0x410;
  message.data1 = (uint64_t)granted;
  if (OS1low_ipc_send((int)child, &message)) return 6;
  if (OS1low_ipc_recv((int)child, &message)) return 7;
  int status = 0;
  if (waitpid((int)child, &status, 0) != child ||
      !WIFEXITED(status) || WEXITSTATUS(status)) return 8;
  if (words[1] != CHILD_PATTERN || words[2] != CHILD_PATTERN) return 9;
  words[3] = TEST_PATTERN; /* still valid after the peer's address space died */
  if (OS1_buffer_unmap((int)handle, &view)) return 10;
  if (OS1low_handle_close((int)handle)) return 11;
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--child") == 0) return child_test();
  int result = parent_test();
  printf("[BUFFERTEST] zero-copy sharing, read-only rights, close and exit: %s (%d)\n",
         result ? "FAIL" : "PASS", result);
  printf("[buffertest] done: %d/1 passed, %d failure(s)\n", result == 0, result != 0);
  return result;
}
