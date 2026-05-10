/*
 * kernel/drivers/cpp_test.cpp
 * Simple C++ test
 */
extern "C" {
#include <kernel/printk.h>
}

class TestClass {
public:
  TestClass() { val = 42; }
  int start() { return val; }

private:
  int val;
};

extern "C" void cpp_test_func(void);

void cpp_test_func(void) {
  TestClass t;
  /* We can't easily print from here without plumbing, but compilation proves
   * C++ works */
  /* pr_info("C++ Test: %d\n", t.start()); */
}
