#include <hal/nxpax.h>
#include <stdint.h>
#include <stddef.h>

struct nxhal_nxpax_slot {
  uint64_t sequence;
  uint64_t pc;
  uint64_t addr;
  uint64_t syndrome;
  int32_t pid;
  uint32_t kind;
  uint32_t flags;
  int32_t cpu_id;
  uint32_t busy;
};

static struct nxhal_nxpax_slot records[NXHAL_NXPAX_RING_CAPACITY];
static uint64_t next_sequence;
static uint64_t dropped_records;

uint64_t nxhal_nxpax_latest(void) {
  return __atomic_load_n(&next_sequence, __ATOMIC_ACQUIRE);
}

uint64_t nxhal_nxpax_dropped(void) {
  return __atomic_load_n(&dropped_records, __ATOMIC_ACQUIRE);
}

void nxhal_nxpax_note(const struct nxhal_nxpax_event *event) {
  if (event == NULL)
    return;
  uint64_t sequence = __atomic_add_fetch(&next_sequence, 1U,
                                          __ATOMIC_ACQ_REL);
  struct nxhal_nxpax_slot *slot =
      &records[(sequence - 1U) % NXHAL_NXPAX_RING_CAPACITY];
  uint32_t expected = 0U;
  if (!__atomic_compare_exchange_n(&slot->busy, &expected, 1U, 0,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    __atomic_add_fetch(&dropped_records, 1U, __ATOMIC_RELAXED);
    return;
  }
  __atomic_store_n(&slot->pc, event->fault_pc, __ATOMIC_RELAXED);
  __atomic_store_n(&slot->addr, event->fault_address, __ATOMIC_RELAXED);
  __atomic_store_n(&slot->syndrome, event->syndrome, __ATOMIC_RELAXED);
  __atomic_store_n(&slot->pid, event->pid, __ATOMIC_RELAXED);
  __atomic_store_n(&slot->kind, event->kind, __ATOMIC_RELAXED);
  __atomic_store_n(&slot->flags, event->flags, __ATOMIC_RELAXED);
  __atomic_store_n(&slot->cpu_id, event->cpu_id, __ATOMIC_RELAXED);
  __atomic_store_n(&slot->sequence, sequence, __ATOMIC_RELEASE);
  __atomic_store_n(&slot->busy, 0U, __ATOMIC_RELEASE);
}

void nxhal_nxpax_fault(uint64_t pc, uint64_t addr, uint64_t syndrome,
                       int user_mode, int32_t cpu_id) {
  const struct nxhal_nxpax_event event = {
      .fault_pc = pc, .fault_address = addr, .syndrome = syndrome,
      .pid = -1, .kind = NXHAL_NXPAX_KIND_HARDWARE,
      .flags = user_mode ? NXHAL_NXPAX_FLAG_USER : 0U,
      .cpu_id = cpu_id};
  nxhal_nxpax_note(&event);
}

int nxhal_nxpax_read(uint64_t sequence, struct nxhal_nxpax_event *event) {
  if (event == NULL || sequence == 0U)
    return -1;
  uint64_t newest = nxhal_nxpax_latest();
  if (sequence > newest ||
      newest - sequence >= NXHAL_NXPAX_RING_CAPACITY)
    return 0;
  struct nxhal_nxpax_slot *slot =
      &records[(sequence - 1U) % NXHAL_NXPAX_RING_CAPACITY];
  /* A single nonwaiting ownership CAS prevents torn snapshots during
   * simultaneous fault writes. Readers never spin or hold a kernel lock. */
  uint32_t expected = 0U;
  if (!__atomic_compare_exchange_n(&slot->busy, &expected, 1U, 0,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return -1;
  event->fault_pc = __atomic_load_n(&slot->pc, __ATOMIC_RELAXED);
  event->fault_address = __atomic_load_n(&slot->addr, __ATOMIC_RELAXED);
  event->syndrome = __atomic_load_n(&slot->syndrome, __ATOMIC_RELAXED);
  event->pid = __atomic_load_n(&slot->pid, __ATOMIC_RELAXED);
  event->kind = __atomic_load_n(&slot->kind, __ATOMIC_RELAXED);
  event->flags = __atomic_load_n(&slot->flags, __ATOMIC_RELAXED);
  event->cpu_id = __atomic_load_n(&slot->cpu_id, __ATOMIC_RELAXED);
  event->sequence = __atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE);
  __atomic_store_n(&slot->busy, 0U, __ATOMIC_RELEASE);
  return event->sequence == sequence ? 1 : 0;
}
