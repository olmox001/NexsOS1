#include <hal/provider.h>
#include <hal/uaccess.h>
#include <stddef.h>
#include <stdint.h>

void hal_uaccess_enter(uint64_t user_root_phys,
                       struct nxhal_uaccess_hw_state *state) {
  if (state == NULL)
    return;
  nxhal_provider_uaccess_enter(user_root_phys, state);
}

void hal_uaccess_exit(const struct nxhal_uaccess_hw_state *state) {
  if (state == NULL)
    return;
  nxhal_provider_uaccess_exit(state);
}

void hal_uaccess_fault_abort(void) {
  nxhal_provider_uaccess_fault_abort();
}
