#include <cstdlib>
#include <iostream>

#if defined(AIMEDIA_HAS_GTEST)
#include <gtest/gtest.h>
#endif

#if defined(AIMEDIA_HAS_GTEST)
TEST(BuildContract, UsesCxx17) {
  EXPECT_GE(__cplusplus, 201703L);
}
#else
int main() {
  if (__cplusplus < 201703L) {
    std::cerr << "__cplusplus=" << __cplusplus << " (need >= 201703L)\n";
    return 1;
  }
  return 0;
}
#endif

