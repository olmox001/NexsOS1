/*
 * NX-CORE object bootstrap: resolve one typed registry entry and instantiate
 * one initial process. No VFS API, filesystem type or service name is visible
 * here; actual ELF loading uses the existing process loader contract.
 */
#include <kernel/object_boot.h>
#include <kernel/printk.h>
#include <kernel/registry.h>
#include <kernel/sched.h>
#include <kernel/string.h>

#define NX_BOOT_EXEC_PREFIX "/krnl/extension/"
#define NX_BOOT_EXEC_PREFIX_LEN 16U

static int nx_object_boot_entry(char out[NX_BOOT_EXEC_PATH_MAX]) {
  if (registry_get(NX_BOOT_REGISTRY_KEY, out, NX_BOOT_EXEC_PATH_MAX) != 0)
    return -1;
  unsigned size = 0U;
  for (; size < NX_BOOT_EXEC_PATH_MAX; ++size) {
    if (out[size] == '\0')
      break;
  }
  if (size <= NX_BOOT_EXEC_PREFIX_LEN || size >= NX_BOOT_EXEC_PATH_MAX ||
      strncmp(out, NX_BOOT_EXEC_PREFIX, NX_BOOT_EXEC_PREFIX_LEN) != 0)
    return -1;
  /* The only initial executable is an extension basename, without
   * additional '/' or relative path components. Prevent privilege/path
   * preset mismatch at the very first machine-authorized load. */
  for (unsigned i = NX_BOOT_EXEC_PREFIX_LEN; i < NX_BOOT_EXEC_PATH_MAX; ++i) {
    if (i >= size)
      break;
    const char ch = out[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
      return -1;
  }
  return 0;
}

int nx_object_bootstrap_launch(void) {
  char entry[NX_BOOT_EXEC_PATH_MAX];
  if (nx_object_boot_entry(entry) != 0) {
    pr_err("%s", "K3: missing/invalid initial object registry entry\n");
    return -1;
  }
  struct process *first = process_create_caps("boot-extension", PROC_PRIO_USER,
                                              PLVL_MACHINE, CAP_ALL);
  if (first == NULL)
    return -1;
  if (process_load_elf(first, entry) != 0) {
    process_abort_spawn(first);
    pr_err("%s", "K3: could not load initial registry-selected extension\n");
    return -1;
  }
  pr_info("K3: Initialized PID %d (boot-extension %s)\n", first->pid, entry);
  enqueue_task(first);
  return 0;
}
