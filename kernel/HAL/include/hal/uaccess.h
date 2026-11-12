#ifndef NXHAL_UACCESS_H
#define NXHAL_UACCESS_H

#include <stdint.h>

struct nxhal_uaccess_hw_state {
  uint64_t saved_root;
};

void hal_uaccess_enter(uint64_t user_root_phys,
                       struct nxhal_uaccess_hw_state *state);
void hal_uaccess_exit(const struct nxhal_uaccess_hw_state *state);
void hal_uaccess_fault_abort(void);

#endif /* NXHAL_UACCESS_H */
