#include "concore2full/async_oper_state.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <semaphore>

using namespace std::chrono_literals;

TEST_CASE("spawn can execute work", "[spawn]") {
  // Arrange
  bool called{false};
  std::binary_semaphore done{0};

  // Act
  concore2full::async_oper_state<int> op;
  op.spawn([&]() -> int {
    called = true;
    done.release();
    return 13;
  });
  done.acquire();
  std::this_thread::sleep_for(1ms);
  concore2full::global_thread_pool().clear();
  auto res = op.await();

  // Assert
  REQUIRE(called);
  REQUIRE(res == 13);
}