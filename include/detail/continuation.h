#pragma once

#include "context_function.h"
#include "core_types.h"
#include "profiling.h"
#include "simple_stack_allocator.h"
#include "stack_allocator.h"
#include <context_core_api.h>

#include <cassert>
#include <functional>

namespace concore2full {
namespace context {

using continuation_t = detail::continuation_t;
using concore2full::detail::context_function;
using concore2full::detail::simple_stack_allocator;
using concore2full::detail::stack_allocator;

namespace detail {
using stack_t = concore2full::detail::stack_t;
using transfer_t = concore2full::detail::transfer_t;
using concore2full::detail::as_value;

//! The constrol structure that needs to be placed on a stack to be able to use it for stackfull
//! coroutines. We need to know how to deallocate the stack memory, and we also need to store the
//! data for the main function to be run on this stack.
template <stack_allocator S, context_function F> struct stack_control_structure {
  //! The stack we are operating on.
  stack_t stack_;
  //! The allocator used to create the stack, and to deallocate it.
  std::decay_t<S> allocator_;
  //! The main function to run in this new context.
  std::decay_t<F> main_function_;

  friend void destroy(stack_control_structure* record) {
    // Save needed data.
    S allocator = std::move(record->allocator_);
    stack_t stack = record->stack_;
    // Destruct the object.
    record->~stack_control_structure();
    // Destroy the stack.
    allocator.deallocate(stack);
  }

  //! The end of the useful portion of the stack.
  void* stack_end() const noexcept {
    // Create a 64-byte gap between the control structure and the usefule stack.
    constexpr uintptr_t gap = 64;
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(this) - gap);
  }
  //! The begin of the useful portion of the stack.
  void* stack_begin() const noexcept {
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(stack_.sp) -
                                   static_cast<uintptr_t>(stack_.size));
  }
  //! The useful size of the stack (where the executing code can store data).
  uintptr_t useful_size() const noexcept {
    return reinterpret_cast<uintptr_t>(stack_end()) - reinterpret_cast<uintptr_t>(stack_begin());
  }
};

//! Allocate memory to be used as stack by stackfull coroutines.
inline auto allocate_stack(stack_allocator auto&& allocator, context_function auto&& f) {
  using control_t = stack_control_structure<decltype(allocator), decltype(f)>;
  // Allocate the stack.
  stack_t stack = allocator.allocate();
  // Put the control structure on the stack, at the end of the allocated space.
  uintptr_t align = alignof(control_t);
  void* p = reinterpret_cast<void*>(
      (reinterpret_cast<uintptr_t>(stack.sp) - static_cast<uintptr_t>(sizeof(control_t))) & ~align);
  return new (p)
      control_t{stack, std::forward<decltype(allocator)>(allocator), std::forward<decltype(f)>(f)};
};

//! Called when finishing executing everything in a stack execution context to clean up the stack.
template <typename C>
inline detail::transfer_t execution_context_exit(detail::transfer_t t) noexcept {
  destroy(reinterpret_cast<C*>(t.data));
  return {nullptr, nullptr};
}

//! The entry point for a stack execution context.
//! Executes the *main function* (the one passed to `callcc`), and then destroys the execution
//! context.
template <typename C> inline void execution_context_entry(detail::transfer_t t) noexcept {
  // The parameter passed in is our control structure.
  auto* control = reinterpret_cast<C*>(t.data);
  assert(control);
  assert(t.fctx);

  // Start executing the given function.
  (void)profiling::zone{CURRENT_LOCATION_NC("callcc.start", profiling::color::green)}.set_value(
      as_value(t.fctx));
  t.fctx = std::invoke(control->main_function_, t.fctx);
  (void)profiling::zone{CURRENT_LOCATION_NC("callcc.end", profiling::color::green)}.set_value(
      as_value(t.fctx));
  assert(t.fctx);

  // Destroy the stack context.
  context_core_api_ontop_fcontext(t.fctx, control, execution_context_exit<C>);
  // Should never reach this point.
  assert(false);
}

//! Creates an execution context, and starts executing the given function.
//! Returns the continuation handle returned from the function.
inline continuation_t create_execution_context(stack_allocator auto&& allocator,
                                               context_function auto&& f) {
  auto* control =
      allocate_stack(std::forward<decltype(allocator)>(allocator), std::forward<decltype(f)>(f));

  // Create a context for running the new code.
  using C = std::decay_t<decltype(*control)>;
  continuation_t ctx = context_core_api_make_fcontext(control->stack_end(), control->useful_size(),
                                                      execution_context_entry<C>);
  assert(ctx != nullptr);
  // Transfer the control to `execution_context_entry`, in the given context.
  return context_core_api_jump_fcontext(ctx, control).fctx;
}

} // namespace detail

inline continuation_t callcc(context_function auto&& f);
inline continuation_t callcc(std::allocator_arg_t, stack_allocator auto&& salloc,
                             context_function auto&& f);
inline continuation_t resume(continuation_t continuation);

//! Call with current continuation.
//! Takes the context of the code immediatelly following this function call, and passes it to the
//! given context function. The given function is executed in a new stack context. We can suspend
//! the context and resume other context, or the given context.
inline continuation_t callcc(context_function auto&& f) {
  return callcc(std::allocator_arg, simple_stack_allocator(), std::forward<decltype(f)>(f));
}
inline continuation_t callcc(std::allocator_arg_t, stack_allocator auto&& salloc,
                             context_function auto&& f) {
  // detail::profile_event_callcc();
  (void)profiling::zone{CURRENT_LOCATION_NC("callcc", profiling::color::green)};
  return detail::create_execution_context(std::forward<decltype(salloc)>(salloc),
                                          std::forward<decltype(f)>(f));
}

//! Resumes the given continuation.
//! The current execution is interrupted, and the program continues from the given continuation
//! point. Returns the context that has been suspended.
inline continuation_t resume(continuation_t continuation) {
  (void)profiling::zone{CURRENT_LOCATION_NC("resume", profiling::color::green)}.set_value(
      detail::as_value(continuation));
  assert(continuation);
  return context_core_api_jump_fcontext(continuation, nullptr).fctx;
}

} // namespace context
} // namespace concore2full
