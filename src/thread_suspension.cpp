#include "concore2full/detail/thread_suspension.h"
#include "concore2full/this_thread.h"

#include <utility>

namespace concore2full::detail {

void thread_suspension::store_relaxed(context_core_api_fcontext_t c) {
  continuation_.store(c, std::memory_order_relaxed);
}
void thread_suspension::store_release(context_core_api_fcontext_t c) {
  continuation_.store(c, std::memory_order_release);
}

context_core_api_fcontext_t thread_suspension::use_thread_suspension_acquire() {
  return continuation_.load(std::memory_order_acquire);
}
context_core_api_fcontext_t thread_suspension::use_thread_suspension_relaxed() {
  return continuation_.load(std::memory_order_relaxed);
}

} // namespace concore2full::detail