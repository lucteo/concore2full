#include "concore2full/thread_pool.h"
#include "concore2full/detail/sleep_helper.h"
#include "concore2full/profiling.h"
#include "concore2full/this_thread.h"
#include "concore2full/thread_snapshot.h"
#include "thread_info.h"

#include <chrono>

using namespace std::chrono_literals;

namespace concore2full {

#ifndef NDEBUG
namespace {
//! Checks that the list represented by `head` is consistent.
bool check_list(concore2full_task* head, void* data) {
  concore2full_task* cur = head;
  while (cur) {
    assert(cur->prev_link_);
    assert(*cur->prev_link_ == cur);
    assert(cur->worker_data_ == data);
    cur = cur->next_;
  }
  return true;
}
} // namespace
#endif

namespace {
//! Return the desired level of conurrency.
size_t concurrency() {
  // Check if we have a maximum concurrency set as environment variable.
  const char* env_var = std::getenv("CONCORE_MAX_CONCURRENCY");
  if (env_var) {
    return std::stoul(env_var);
  }

  // Otherwise, return the hardware concurrency.
  return std::thread::hardware_concurrency();
}
} // namespace

thread_pool::thread_pool() : thread_pool(concurrency()) {}

thread_pool::thread_pool(int thread_count) : work_lines_(thread_count) {
  threads_.reserve(thread_count);
  try {
    for (int i = 0; i < thread_count; i++) {
      threads_.emplace_back([this, i] { thread_main(i); }, i);
    }
  } catch (...) {
    join();
  }
  profiling::define_counter_track(num_tasks_, "num_tasks");
}
thread_pool::~thread_pool() {
  profiling::zone zone{CURRENT_LOCATION()};
  if (num_tasks_.load(std::memory_order_relaxed) > 0) {
    // Users shall drain the tasks before destroying the thread pool.
    std::terminate();
  }
  join();
}

void thread_pool::enqueue(concore2full_task* task) noexcept {
  profiling::zone zone{CURRENT_LOCATION()};
  zone.set_param("task,x", reinterpret_cast<uint64_t>(task));
  zone.add_flow(reinterpret_cast<uint64_t>(task));

  task->next_ = nullptr;
  task->prev_link_ = nullptr;

  // Note: using uint32_t, as we need to safely wrap around.
  uint32_t work_line_count = work_lines_.size();
  assert(work_line_count > 0);
  uint32_t index = line_to_push_to_.fetch_add(1, std::memory_order_relaxed) % work_line_count;

  // Try to push this to a worker thread without blocking.
  for (uint32_t i = 0; i < work_line_count; i++) {
    uint32_t current_index = (index + i) % work_line_count;
    if (work_lines_[current_index].try_push(task)) {
      notify_one(current_index);
      return;
    }
  }
  // If that didn't work, just force-push to the queue of the selected worker thread.
  uint32_t current_index = index % work_line_count;
  work_lines_[current_index].push(task);
  notify_one(current_index);
}

bool thread_pool::extract_task(concore2full_task* task) noexcept {
  profiling::zone zone{CURRENT_LOCATION()};
  zone.set_param("task,x", reinterpret_cast<uint64_t>(task));
  zone.add_flow_terminate(reinterpret_cast<uint64_t>(task));
  auto d = static_cast<work_line*>(task->worker_data_);
  bool res = d ? d->extract_task(task) : false;
  if (res) {
    num_tasks_.fetch_sub(1, std::memory_order_release);
    // Sync: ensure that all the stores are published before this one
  }
  return res;
}

void thread_pool::join() noexcept {
  profiling::zone zone{CURRENT_LOCATION()};
  // Tell everybody to stop.
  stop_requested_.store(true, std::memory_order_release);
  // Sync: publish all previous state before joining.
  // Wake up all the threads.
  for (auto& t : threads_) {
    t.try_notify(0);
  }
  // Join the threads.
  for (auto& t : threads_) {
    t.join();
  }
  threads_.clear();
}

bool thread_pool::worker_thread_data::try_notify(int work_line_hint) noexcept {
  profiling::zone zone{CURRENT_LOCATION()};

  if (wake_requests_.fetch_add(1, std::memory_order_acquire) == 0) {
    // Sync: acquire: protecting wakeup_token_.
    // Sync: we don't need release, as this is called after `add_task`, which has release semantics.
    // It's also called in `join` after a release operation.

    // Tell the sleeping thread where to start looking for work.
    work_line_start_index_.store(work_line_hint, std::memory_order_relaxed);
    // Sync: no ordering guarantees needed here, as `notify()` acts as a release barrier.

    // Ensure that the sleeping thread is woken up.
    wakeup_token_.notify();
    return true;
  }
  return false;
}
int thread_pool::worker_thread_data::sleep(std::atomic<bool>& stop_requested) noexcept {
  profiling::zone zone{CURRENT_LOCATION()};

  detail::sleep_helper sleep_helper;
  wakeup_token_ = sleep_helper.get_wakeup_token();
  if (wake_requests_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    // Sync: acquire: don't move any sleep operations before this.
    // Sync: release: don't move the above `wakeup_token_` stores after this. A thread that is
    // trying to wake us up should have access to the wakeup token.
    if (!stop_requested.load(std::memory_order_relaxed)) {
      // Sync: no ordering guarantees needed here.
      sleep_helper.sleep();
    }
  }
  wakeup_token_.invalidate();
  wake_requests_.store(1, std::memory_order_release);
  // Sync: don't any stores after this.
  return work_line_start_index_.load(std::memory_order_acquire);
  // Sync: Don't move any loads before this; we might influence futher loads.
}
void thread_pool::worker_thread_data::join() { thread_.join(); }

bool thread_pool::work_line::try_push(concore2full_task* task) noexcept {
  // Fail if we can't acquire the lock.
  std::unique_lock lock{bottleneck_, std::try_to_lock};
  if (!lock)
    return false;

  push_unprotected(task);
  return true;
}
void thread_pool::work_line::push(concore2full_task* task) noexcept {
  // Add the task at the back of the queue.
  std::unique_lock lock{bottleneck_};
  push_unprotected(task);
}
concore2full_task* thread_pool::work_line::try_pop() noexcept {
  std::unique_lock lock{bottleneck_, std::try_to_lock};
  if (!lock || !tasks_stack_)
    return nullptr;
  return pop_unprotected();
}
bool thread_pool::work_line::extract_task(concore2full_task* task) noexcept {
  profiling::zone zone{CURRENT_LOCATION()};
  zone.set_param("line,x", this);
  std::unique_lock lock{bottleneck_};
  assert(check_list(tasks_stack_, this));
  assert(!tasks_stack_ || tasks_stack_->prev_link_ == &tasks_stack_);
  if (task->worker_data_) {
    assert(task->worker_data_ == this);
    assert(task->prev_link_);
    assert(*task->prev_link_ == task);

    *task->prev_link_ = task->next_;
    if (task->next_)
      task->next_->prev_link_ = task->prev_link_;
    task->worker_data_ = nullptr;
    task->prev_link_ = nullptr;
    assert(tasks_stack_ != task);
    assert(!tasks_stack_ || tasks_stack_->prev_link_ == &tasks_stack_);
    assert(check_list(tasks_stack_, this));
    return true;
  } else {
    return false;
  }
}

void thread_pool::work_line::push_unprotected(concore2full_task* task) noexcept {
  // Add the task in the front of the list.
  assert(check_list(tasks_stack_, this));
  task->worker_data_ = this;
  task->next_ = tasks_stack_;
  if (tasks_stack_)
    tasks_stack_->prev_link_ = &task->next_;
  task->prev_link_ = &tasks_stack_;
  tasks_stack_ = task;
  assert(check_list(tasks_stack_, this));
}

concore2full_task* thread_pool::work_line::pop_unprotected() noexcept {
  assert(check_list(tasks_stack_, this));
  if (tasks_stack_) {
    concore2full_task* res = tasks_stack_;
    tasks_stack_ = tasks_stack_->next_;
    if (tasks_stack_)
      tasks_stack_->prev_link_ = &tasks_stack_;
    res->prev_link_ = nullptr;
    res->worker_data_ = nullptr;
    assert(check_list(tasks_stack_, this));
    return res;
  }
  return nullptr;
}

void thread_pool::notify_one(int work_line_hint) noexcept {
  int old = num_tasks_.fetch_add(1, std::memory_order_relaxed);
  // Sync: no ordering guarantees needed here.
  if (old <= int(threads_.size())) {
    for (auto& t : threads_) {
      if (t.try_notify(work_line_hint)) {
        return;
      }
    }
  }
}

std::string thread_name(int index) { return "worker-" + std::to_string(index); }

void thread_pool::thread_main(int thread_index) noexcept {
  concore2full::profiling::emit_thread_name_and_stack(thread_name(thread_index).c_str());

  profiling::zone_instant z0{CURRENT_LOCATION_N("worker thread start")};

  z0.set_param("thread,x", &threads_[thread_index]);
  z0.set_param("cur_thread,x", &detail::get_current_thread_info());

  // We need to exit on the same thread.
  thread_snapshot t;

  int work_line_count = work_lines_.size();
  int work_line_hint = thread_index;
  while (!stop_requested_.load(std::memory_order_relaxed)) {
    // Sync: no ordering guarantees needed here.

    // First check if we need to restore this thread to somebody else.
    this_thread::inversion_checkpoint();

    if (num_tasks_.load(std::memory_order_acquire) == 0) {
      // Sync: don't move any sleep operations before this load.
      // If there are no tasks, we can sleep.
      work_line_hint = threads_[thread_index].sleep(stop_requested_);
    }

    concore2full_task* to_execute{nullptr};
    int line_index = 0;

    // Try to pop a task from the first thread data available.
    for (int i = 0; i < 2 * work_line_count; i++) {
      line_index = (i + work_line_hint) % work_line_count;
      to_execute = work_lines_[line_index].try_pop();
      if (to_execute)
        break;
    }

    // If we have a task, execute it.
    if (to_execute) {
      // We successfully popped a task; decrease the counter.
      num_tasks_.fetch_sub(1, std::memory_order_relaxed);

      profiling::zone zone2{CURRENT_LOCATION_N("execute")};
      zone2.set_param("task", to_execute);
      zone2.add_flow_terminate(to_execute);
      to_execute->task_function_(to_execute, line_index);
      continue;
    }
  }

  // Ensure we finish on the same thread
  t.revert();

  (void)profiling::zone_instant{CURRENT_LOCATION_N("worker thread end")};
}

} // namespace concore2full