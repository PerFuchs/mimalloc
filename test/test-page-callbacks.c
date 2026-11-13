/* Page callback tests, distributed under the MIT license (see LICENSE). */
#include <stdint.h>
#include <stdlib.h>
#include "mimalloc.h"
#include "mimalloc/atomic.h"
#include "mimalloc/page-callbacks.h"
#include "testhelper.h"

// ---------------------------------------------------------------------------
// Callback helpers
// ---------------------------------------------------------------------------
typedef struct callback_state_s {
  _Atomic(uintptr_t) accept;
  _Atomic(uintptr_t) acquire_calls;
  _Atomic(uintptr_t) release_calls;
  _Atomic(uintptr_t) acquired_bytes;
  _Atomic(uintptr_t) released_bytes;
  _Atomic(uintptr_t) vetoed;
  _Atomic(uintptr_t) deferred_after_veto;
  _Atomic(uintptr_t) errors;
  _Atomic(uintptr_t) callback_errors;
} callback_state_t;

static bool acquire_page(size_t bytes, void* arg) {
  callback_state_t* state = (callback_state_t*)arg;
  if (bytes == 0) mi_atomic_increment_relaxed(&state->callback_errors);
  mi_atomic_increment_relaxed(&state->acquire_calls);
  if (!mi_atomic_load_acquire(&state->accept)) {
    mi_atomic_store_release(&state->vetoed, 1);
    return false;
  }
  mi_atomic_add_relaxed(&state->acquired_bytes, bytes);
  return true;
}

static void observe_release(size_t bytes, void* arg) {
  callback_state_t* state = (callback_state_t*)arg;
  if (bytes == 0) mi_atomic_increment_relaxed(&state->callback_errors);
  mi_atomic_add_relaxed(&state->released_bytes, bytes);
  mi_atomic_increment_relaxed(&state->release_calls);
}

static void release_page(size_t bytes, void* arg) {
  callback_state_t* state = (callback_state_t*)arg;
  if (bytes == 0) mi_atomic_increment_relaxed(&state->callback_errors);
  const uintptr_t before = mi_atomic_add_relaxed(&state->released_bytes, bytes);
  if (before + bytes > mi_atomic_load_acquire(&state->acquired_bytes)) {
    mi_atomic_increment_relaxed(&state->callback_errors);
  }
  mi_atomic_increment_relaxed(&state->release_calls);
}

static bool accounting_is_balanced(callback_state_t* state) {
  return (mi_atomic_load_acquire(&state->acquired_bytes) == mi_atomic_load_acquire(&state->released_bytes) &&
          mi_atomic_load_acquire(&state->callback_errors) == 0);
}

static mi_heap_t* new_heap(callback_state_t* state) {
  return _mi_heap_new_with_page_callbacks(acquire_page, release_page, state);
}

static void deferred_free(bool force, unsigned long long heartbeat, void* arg) {
  (void)force;
  (void)heartbeat;
  callback_state_t* state = (callback_state_t*)arg;
  if (mi_atomic_load_acquire(&state->vetoed)) {
    mi_atomic_increment_relaxed(&state->deferred_after_veto);
  }
}

static void error_handler(int error, void* arg) {
  (void)error;
  callback_state_t* state = (callback_state_t*)arg;
  mi_atomic_increment_relaxed(&state->errors);
}

// ---------------------------------------------------------------------------
// Page acquisition and release
// ---------------------------------------------------------------------------
static bool test_veto(size_t size, size_t alignment, bool zero) {
  callback_state_t state = {0};
  mi_heap_t* heap = new_heap(&state);
  if (heap == NULL) return false;
  mi_register_deferred_free(deferred_free, &state);
  mi_register_error(error_handler, &state);
  void* ptr = (alignment != 0 ? mi_heap_malloc_aligned(heap, size, alignment)
                              : (zero ? mi_heap_zalloc(heap, size) : mi_heap_malloc(heap, size)));
  bool result = (ptr == NULL &&
                 mi_atomic_load_acquire(&state.acquire_calls) == 1 &&
                 mi_atomic_load_acquire(&state.release_calls) == 0 &&
                 mi_atomic_load_acquire(&state.deferred_after_veto) == 0 &&
                 mi_atomic_load_acquire(&state.errors) == 0);
  mi_register_deferred_free(NULL, NULL);
  mi_register_error(NULL, NULL);
  mi_free(ptr);
  mi_atomic_store_release(&state.accept, 1);
  ptr = (alignment != 0 ? mi_heap_malloc_aligned(heap, size, alignment)
                       : (zero ? mi_heap_zalloc(heap, size) : mi_heap_malloc(heap, size)));
  if (ptr == NULL) {
    mi_heap_destroy(heap);
    return false;
  }
  result &= (mi_atomic_load_acquire(&state.acquire_calls) == 2);
  if (alignment != 0) result &= ((uintptr_t)ptr % alignment == 0);
  if (zero) {
    for (size_t i = 0; i < size; ++i) result &= (((unsigned char*)ptr)[i] == 0);
  }
  mi_free(ptr);
  mi_heap_collect(heap, true);
  mi_heap_destroy(heap);
  return (result && accounting_is_balanced(&state) && mi_atomic_load_acquire(&state.release_calls) == 1);
}

static bool test_release_before_destroy(void) {
  callback_state_t state = {0};
  mi_atomic_store_release(&state.accept, 1);
  mi_heap_t* heap = new_heap(&state);
  if (heap == NULL) return false;
  void* ptr = mi_heap_malloc(heap, 8 * 1024 * 1024);
  if (ptr == NULL) {
    mi_heap_destroy(heap);
    return false;
  }
  mi_free(ptr);
  mi_heap_collect(heap, true);
  bool result = (mi_atomic_load_acquire(&state.release_calls) == 1 && accounting_is_balanced(&state));
  const uintptr_t released = mi_atomic_load_acquire(&state.released_bytes);
  mi_heap_destroy(heap);
  return (result && mi_atomic_load_acquire(&state.released_bytes) == released && accounting_is_balanced(&state));
}

static bool test_delete_transfers_charge(void) {
  callback_state_t state = {0};
  mi_atomic_store_release(&state.accept, 1);
  mi_heap_t* heap = new_heap(&state);
  if (heap == NULL) return false;
  void* ptr = mi_heap_malloc(heap, 64);
  if (ptr == NULL) {
    mi_heap_destroy(heap);
    return false;
  }
  bool result = (mi_atomic_load_acquire(&state.acquired_bytes) > 0);
  mi_heap_delete(heap);
  result &= accounting_is_balanced(&state);
  const uintptr_t released = mi_atomic_load_acquire(&state.released_bytes);
  mi_free(ptr);
  mi_collect(true);
  return (result && mi_atomic_load_acquire(&state.released_bytes) == released && accounting_is_balanced(&state));
}

// ---------------------------------------------------------------------------
// Nested and concurrent allocations
// ---------------------------------------------------------------------------
typedef struct nested_state_s {
  callback_state_t accounting; // first so release_page can use the same argument
  mi_heap_t* heap;
  bool inside_callback;
  bool nested_rejected;
} nested_state_t;

static bool nested_acquire(size_t bytes, void* arg) {
  nested_state_t* state = (nested_state_t*)arg;
  if (state->inside_callback) return false;
  state->inside_callback = true;
  state->nested_rejected = (mi_heap_malloc(state->heap, 2 * 1024 * 1024) == NULL);
  state->inside_callback = false;
  return acquire_page(bytes, &state->accounting);
}

static bool test_nested_veto(void) {
  nested_state_t state = {0};
  mi_atomic_store_release(&state.accounting.accept, 1);
  state.heap = _mi_heap_new_with_page_callbacks(nested_acquire, release_page, &state);
  if (state.heap == NULL) return false;
  void* ptr = mi_heap_malloc(state.heap, 64);
  bool result = (ptr != NULL && state.nested_rejected);
  mi_free(ptr);
  mi_heap_destroy(state.heap);
  return (result && accounting_is_balanced(&state.accounting));
}

// ---------------------------------------------------------------------------
// Thread helpers (start all workers before joining)
// ---------------------------------------------------------------------------
typedef bool (thread_fun_t)(void*);
typedef struct thread_s {
  thread_fun_t* fun;
  void* arg;
  bool result;
#if defined(_WIN32)
  HANDLE handle;
#else
  pthread_t handle;
#endif
} thread_t;

#if defined(_WIN32)
static DWORD WINAPI thread_entry(LPVOID arg) {
  thread_t* thread = (thread_t*)arg;
  thread->result = thread->fun(thread->arg);
  return 0;
}
#else
static void* thread_entry(void* arg) {
  thread_t* thread = (thread_t*)arg;
  thread->result = thread->fun(thread->arg);
  return NULL;
}
#endif

static bool thread_start(thread_t* thread, thread_fun_t* fun, void* arg) {
  thread->fun = fun;
  thread->arg = arg;
  thread->result = false;
#if defined(_WIN32)
  thread->handle = CreateThread(NULL, 0, thread_entry, thread, 0, NULL);
  return (thread->handle != NULL);
#else
  return (pthread_create(&thread->handle, NULL, thread_entry, thread) == 0);
#endif
}

static bool thread_join(thread_t* thread) {
  // A failed join cannot safely return while the worker still uses stack data.
#if defined(_WIN32)
  if (WaitForSingleObject(thread->handle, INFINITE) != WAIT_OBJECT_0) {
    fprintf(stderr, "unable to join test thread\n");
    abort();
  }
  const bool closed = (CloseHandle(thread->handle) != 0);
  return (thread->result && closed);
#else
  if (pthread_join(thread->handle, NULL) != 0) {
    fprintf(stderr, "unable to join test thread\n");
    abort();
  }
  return thread->result;
#endif
}

static bool shared_heap_worker(void* arg) {
  mi_heap_t* heap = (mi_heap_t*)arg;
  void* ptrs[300];
  bool result = true;
  for (size_t i = 0; i < 300; ++i) {
    ptrs[i] = mi_heap_malloc(heap, 37 + (i % 3) * 100);
    result &= (ptrs[i] != NULL);
  }
  for (size_t i = 0; i < 300; ++i) mi_free(ptrs[i]);
  return result;
}

static bool test_concurrent_heap(void) {
  callback_state_t state = {0};
  mi_atomic_store_release(&state.accept, 1);
  mi_heap_t* heap = new_heap(&state);
  if (heap == NULL) return false;
  thread_t threads[8];
  size_t started = 0;
  while (started < 8 && thread_start(&threads[started], shared_heap_worker, heap)) started++;
  bool result = (started == 8);
  for (size_t i = 0; i < started; ++i) result &= thread_join(&threads[i]);
  mi_heap_destroy(heap);
  return (result && mi_atomic_load_acquire(&state.release_calls) > 0 && accounting_is_balanced(&state));
}

// ---------------------------------------------------------------------------
// Independently optional callbacks
// ---------------------------------------------------------------------------
static bool test_acquire_only(void) {
  callback_state_t state = {0};
  mi_heap_t* heap = _mi_heap_new_with_page_callbacks(acquire_page, NULL, &state);
  if (heap == NULL) return false;
  void* ptr = mi_heap_malloc(heap, 8 * 1024 * 1024);
  bool result = (ptr == NULL && mi_atomic_load_acquire(&state.acquire_calls) == 1 &&
                 mi_atomic_load_acquire(&state.acquired_bytes) == 0);
  mi_free(ptr);
  mi_atomic_store_release(&state.accept, 1);
  ptr = mi_heap_malloc(heap, 8 * 1024 * 1024);
  result &= (ptr != NULL && mi_atomic_load_acquire(&state.acquire_calls) == 2 &&
             mi_atomic_load_acquire(&state.acquired_bytes) > 0);
  mi_free(ptr);
  mi_heap_collect(heap, true);
  mi_heap_destroy(heap);
  return (result && mi_atomic_load_acquire(&state.release_calls) == 0 &&
          mi_atomic_load_acquire(&state.callback_errors) == 0);
}

static bool test_release_only(bool transfer) {
  callback_state_t state = {0};
  mi_heap_t* heap = _mi_heap_new_with_page_callbacks(NULL, observe_release, &state);
  if (heap == NULL) return false;
  void* ptr = mi_heap_malloc(heap, 8 * 1024 * 1024);
  if (ptr == NULL) {
    mi_heap_destroy(heap);
    return false;
  }
  bool result = (mi_atomic_load_acquire(&state.release_calls) == 0);
  if (transfer) mi_heap_delete(heap);
  else mi_heap_destroy(heap);
  result &= (mi_atomic_load_acquire(&state.acquire_calls) == 0 && mi_atomic_load_acquire(&state.release_calls) == 1);
  const uintptr_t released = mi_atomic_load_acquire(&state.released_bytes);
  result &= (released > 0);
  if (transfer) {
    mi_free(ptr);
    mi_collect(true);
    result &= (mi_atomic_load_acquire(&state.release_calls) == 1 && mi_atomic_load_acquire(&state.released_bytes) == released);
  }
  return (result && mi_atomic_load_acquire(&state.callback_errors) == 0);
}

static bool test_no_callbacks(void) {
  mi_heap_t* heap = _mi_heap_new_with_page_callbacks(NULL, NULL, NULL);
  if (heap == NULL) return false;
  bool result = (mi_heap_malloc(heap, 64) != NULL);
  mi_heap_destroy(heap);
  return result;
}

// ---------------------------------------------------------------------------
// Main testing
// ---------------------------------------------------------------------------
int main(void) {
  mi_option_disable(mi_option_verbose);
  CHECK("page-callback-veto-tiny", test_veto(1, 0, false));
  CHECK("page-callback-veto-small", test_veto(37, 0, false));
  CHECK("page-callback-veto-8KiB", test_veto(8192, 0, false));
  CHECK("page-callback-veto-64KiB", test_veto(65536, 0, false));
  CHECK("page-callback-veto-1MiB", test_veto(1024 * 1024, 0, false));
  CHECK("page-callback-veto-8MiB", test_veto(8 * 1024 * 1024, 0, false));
  CHECK("page-callback-veto-aligned", test_veto(97, 1024 * 1024, false));
  CHECK("page-callback-veto-zero", test_veto(128, 0, true));
  CHECK("page-callback-release-before-destroy", test_release_before_destroy());
  CHECK("page-callback-delete-transfers-charge", test_delete_transfers_charge());
  CHECK("page-callback-nested-veto", test_nested_veto());
  CHECK("page-callback-concurrent-heap", test_concurrent_heap());
  CHECK("page-callback-acquire-only", test_acquire_only());
  CHECK("page-callback-release-only-destroy", test_release_only(false));
  CHECK("page-callback-release-only-delete", test_release_only(true));
  CHECK("page-callback-none", test_no_callbacks());
  return print_test_summary();
}
