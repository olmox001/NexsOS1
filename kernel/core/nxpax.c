#include <kernel/nxpax.h>
#include <hal/nxpax.h>
#include <hal/core.h>

void nxpax_kernel_record(int32_t pid, uint32_t kind, uint64_t pc,
                         uint64_t address, uint64_t syndrome,
                         uint32_t flags) {
  const struct nxhal_nxpax_event event = {
      .fault_pc = pc, .fault_address = address, .syndrome = syndrome,
      .pid = pid, .kind = kind, .flags = flags,
      .cpu_id = hal_cpu_id_fault_safe()};
  nxhal_nxpax_note(&event);
}
