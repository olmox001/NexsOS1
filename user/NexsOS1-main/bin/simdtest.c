/* Real cross-process FP/vector preservation regression, both ISA providers. */
#include <os1.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>

long os1_simd_context_probe(uint32_t seed);

static int run_pair(void) {
  long child = OS1low_process_spawn("/bin/simdchild", 0, NULL);
  if (child < 0) return 1;
  long own = os1_simd_context_probe(UINT32_C(0x5371ac29));
  int status = 0;
  long waited = waitpid((int)child, &status, 0);
  if (own != 0 || waited != child || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0) return 2;
  return 0;
}

int main(void) {
  int rc = run_pair();
  if (printf("[simdtest] FP/vector context, syscall, yield, sleep: %s (%d)\n",
             rc == 0 ? "PASS" : "FAIL", rc) < 0) return 1;
  if (printf("[simdtest] done: %d/1 passed, %d failure(s)\n",
             rc == 0 ? 1 : 0, rc == 0 ? 0 : 1) < 0) return 1;
  return rc;
}
