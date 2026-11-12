/*
 * NXPAX SMP fault/read stress: links the PRODUCTION HAL ring, not a mock.
 * Two independent test scenarios: 1M events and 2M concurrent events.
 */
#include <hal/nxpax.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define NXPAX_STRESS_EVENTS 1000000U
#define NXPAX_STRESS_READ_BUDGET (4U * NXPAX_STRESS_EVENTS)
#define NXPAX_STRESS_CHECKSUM UINT64_C(0x735F902F51D8BABA)

struct nxpax_stress_case {
  atomic_uint completed;
  atomic_uint valid_samples;
  atomic_uint failures;
  uint32_t writers;
};

struct nxpax_writer {
  struct nxpax_stress_case *test;
  uint32_t cpu_id;
};

static void *nxpax_stress_write(void *context) {
  struct nxpax_writer *writer = (struct nxpax_writer *)context;
  for (uint32_t i = 0U; i < NXPAX_STRESS_EVENTS; ++i) {
    uint64_t marker = ((uint64_t)writer->cpu_id << 56) | i;
    nxhal_nxpax_fault(marker, marker ^ NXPAX_STRESS_CHECKSUM,
                      marker + 17U, 1, (int32_t)writer->cpu_id);
  }
  atomic_fetch_add_explicit(&writer->test->completed, 1U,
                            memory_order_release);
  return NULL;
}

static int nxpax_stress_consistent(const struct nxhal_nxpax_event *event) {
  if (event->fault_address != (event->fault_pc ^ NXPAX_STRESS_CHECKSUM))
    return 0;
  if (event->syndrome != event->fault_pc + 17U)
    return 0;
  if (event->cpu_id != (int32_t)(event->fault_pc >> 56))
    return 0;
  if (event->kind != NXHAL_NXPAX_KIND_HARDWARE ||
      event->flags != NXHAL_NXPAX_FLAG_USER)
    return 0;
  return 1;
}

static void *nxpax_stress_read(void *context) {
  struct nxpax_stress_case *test = (struct nxpax_stress_case *)context;
  for (uint32_t i = 0U; i < NXPAX_STRESS_READ_BUDGET; ++i) {
    uint64_t newest = nxhal_nxpax_latest();
    struct nxhal_nxpax_event event = {0};
    if (newest != 0U) {
      int result = nxhal_nxpax_read(newest, &event);
      if (result == 1) {
        if (!nxpax_stress_consistent(&event))
          atomic_fetch_add_explicit(&test->failures, 1U,
                                    memory_order_relaxed);
        atomic_fetch_add_explicit(&test->valid_samples, 1U,
                                  memory_order_relaxed);
      }
    }
    if (i >= NXPAX_STRESS_EVENTS / 8U &&
        atomic_load_explicit(&test->completed, memory_order_acquire) ==
            test->writers)
      return NULL;
  }
  if (atomic_load_explicit(&test->completed, memory_order_acquire) !=
      test->writers)
    atomic_fetch_add_explicit(&test->failures, 1U, memory_order_relaxed);
  return NULL;
}

static int nxpax_stress_run(uint32_t writer_count) {
  struct nxpax_stress_case test = {.writers = writer_count};
  atomic_init(&test.completed, 0U);
  atomic_init(&test.valid_samples, 0U);
  atomic_init(&test.failures, 0U);
  struct nxpax_writer arguments[2] = {{&test, 11U}, {&test, 22U}};
  pthread_t writers[2];
  pthread_t reader;
  uint64_t start = nxhal_nxpax_latest();
  if (pthread_create(&reader, NULL, nxpax_stress_read, &test) != 0)
    return 1;
  for (uint32_t i = 0U; i < writer_count; ++i) {
    if (pthread_create(&writers[i], NULL, nxpax_stress_write,
                       &arguments[i]) != 0)
      return 2;
  }
  for (uint32_t i = 0U; i < writer_count; ++i) {
    if (pthread_join(writers[i], NULL) != 0)
      return 3;
  }
  if (pthread_join(reader, NULL) != 0)
    return 4;
  if (nxhal_nxpax_latest() - start !=
      (uint64_t)writer_count * NXPAX_STRESS_EVENTS)
    return 5;
  if (atomic_load_explicit(&test.failures, memory_order_relaxed) != 0U)
    return 6;
  struct nxhal_nxpax_event final = {0};
  if (nxhal_nxpax_read(nxhal_nxpax_latest(), &final) != 1 ||
      !nxpax_stress_consistent(&final))
    return 7;
  if (atomic_load_explicit(&test.valid_samples, memory_order_relaxed) == 0U)
    return 8;
  printf("PASS: NXPAX concurrent writers=%u events=%u samples=%u dropped=%llu\n",
         writer_count, writer_count * NXPAX_STRESS_EVENTS,
         atomic_load_explicit(&test.valid_samples, memory_order_relaxed),
         (unsigned long long)nxhal_nxpax_dropped());
  return 0;
}

int main(void) {
  int status = nxpax_stress_run(1U);
  if (status != 0) {
    fprintf(stderr, "NXPAX 1M stress failed: %d\n", status);
    return status;
  }
  status = nxpax_stress_run(2U);
  if (status != 0) {
    fprintf(stderr, "NXPAX 2M stress failed: %d\n", status);
    return status;
  }
  return 0;
}
