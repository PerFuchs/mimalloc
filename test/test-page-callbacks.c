/* Page callback tests, distributed under the MIT license (see LICENSE). */
#include <assert.h>
#include <stdint.h>
#include <mimalloc.h>
#include <mimalloc/atomic.h>
#include <mimalloc/page-callbacks.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "page callbacks: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
  abort(); \
} } while (0)

typedef struct callback_state_s {
  _Atomic(uintptr_t) accept;
  _Atomic(uintptr_t) acquire_calls;
  _Atomic(uintptr_t) release_calls;
  _Atomic(uintptr_t) acquired_bytes;
  _Atomic(uintptr_t) released_bytes;
  _Atomic(uintptr_t) vetoed;
  _Atomic(uintptr_t) deferred_after_veto;
  _Atomic(uintptr_t) errors;
} callback_state_t;

static bool acquire_page(size_t bytes, void* arg) {
  callback_state_t* state = (callback_state_t*)arg;
  CHECK(bytes > 0);
  mi_atomic_increment_relaxed(&state->acquire_calls);
  if (!mi_atomic_load_acquire(&state->accept)) {
    mi_atomic_store_release(&state->vetoed, 1);
    return false;
  }
  mi_atomic_add_relaxed(&state->acquired_bytes, bytes);
  return true;
}

static void release_page(size_t bytes, void* arg) {
  callback_state_t* state = (callback_state_t*)arg;
  CHECK(bytes > 0);
  const uintptr_t before = mi_atomic_add_relaxed(&state->released_bytes, bytes);
  CHECK(before + bytes <= mi_atomic_load_acquire(&state->acquired_bytes));
  mi_atomic_increment_relaxed(&state->release_calls);
}

static mi_heap_t* new_heap(callback_state_t* state) {
  mi_heap_t* heap = _mi_heap_new_with_page_callbacks(acquire_page, release_page, state);
  CHECK(heap != NULL);
  return heap;
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

static void check_veto(size_t size, size_t alignment, bool zero) {
  callback_state_t state = {0};
  mi_heap_t* heap = new_heap(&state);
  mi_register_deferred_free(deferred_free, &state);
  mi_register_error(error_handler, &state);
  void* ptr = (alignment != 0 ? mi_heap_malloc_aligned(heap, size, alignment)
                              : (zero ? mi_heap_zalloc(heap, size) : mi_heap_malloc(heap, size)));
  CHECK(ptr == NULL);
  CHECK(mi_atomic_load_acquire(&state.acquire_calls) == 1);
  CHECK(mi_atomic_load_acquire(&state.release_calls) == 0);
  CHECK(mi_atomic_load_acquire(&state.deferred_after_veto) == 0);
  CHECK(mi_atomic_load_acquire(&state.errors) == 0);
  mi_register_deferred_free(NULL, NULL);
  mi_register_error(NULL, NULL);
  mi_atomic_store_release(&state.accept, 1);
  ptr = (alignment != 0 ? mi_heap_malloc_aligned(heap, size, alignment)
                       : (zero ? mi_heap_zalloc(heap, size) : mi_heap_malloc(heap, size)));
  CHECK(ptr != NULL);
  CHECK(mi_atomic_load_acquire(&state.acquire_calls) == 2);
  if (alignment != 0) CHECK((uintptr_t)ptr % alignment == 0);
  if (zero) {
    for (size_t i = 0; i < size; ++i) CHECK(((unsigned char*)ptr)[i] == 0);
  }
  mi_free(ptr);
  mi_heap_collect(heap, true);
  mi_heap_destroy(heap);
  CHECK(mi_atomic_load_acquire(&state.acquired_bytes) == mi_atomic_load_acquire(&state.released_bytes));
  CHECK(mi_atomic_load_acquire(&state.release_calls) == 1);
}

static void check_release_before_destroy(void) {
  callback_state_t state = {0};
  mi_atomic_store_release(&state.accept, 1);
  mi_heap_t* heap = new_heap(&state);
  void* ptr = mi_heap_malloc(heap, 8 * 1024 * 1024);
  CHECK(ptr != NULL);
  mi_free(ptr);
  mi_heap_collect(heap, true);
  CHECK(mi_atomic_load_acquire(&state.release_calls) == 1);
  CHECK(mi_atomic_load_acquire(&state.acquired_bytes) == mi_atomic_load_acquire(&state.released_bytes));
  const uintptr_t released = mi_atomic_load_acquire(&state.released_bytes);
  mi_heap_destroy(heap);
  CHECK(mi_atomic_load_acquire(&state.released_bytes) == released);
  CHECK(mi_atomic_load_acquire(&state.acquired_bytes) == mi_atomic_load_acquire(&state.released_bytes));
}

static void check_delete_transfers_charge(void) {
  callback_state_t state = {0};
  mi_atomic_store_release(&state.accept, 1);
  mi_heap_t* heap = new_heap(&state);
  void* ptr = mi_heap_malloc(heap, 64);
  CHECK(ptr != NULL);
  CHECK(mi_atomic_load_acquire(&state.acquired_bytes) > 0);
  mi_heap_delete(heap);
  CHECK(mi_atomic_load_acquire(&state.acquired_bytes) == mi_atomic_load_acquire(&state.released_bytes));
  const uintptr_t released = mi_atomic_load_acquire(&state.released_bytes);
  mi_free(ptr);
  mi_collect(true);
  CHECK(mi_atomic_load_acquire(&state.released_bytes) == released);
}

typedef struct nested_state_s {
  callback_state_t accounting;
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

static void check_nested_veto(void) {
  nested_state_t state = {0};
  mi_atomic_store_release(&state.accounting.accept, 1);
  state.heap = _mi_heap_new_with_page_callbacks(nested_acquire, release_page, &state);
  CHECK(state.heap != NULL);
  void* ptr = mi_heap_malloc(state.heap, 64);
  CHECK(ptr != NULL);
  CHECK(state.nested_rejected);
  mi_free(ptr);
  mi_heap_destroy(state.heap);
  CHECK(mi_atomic_load_acquire(&state.accounting.acquired_bytes) == mi_atomic_load_acquire(&state.accounting.released_bytes));
}

typedef void (thread_fun_t)(void*);
typedef struct thread_s {
  thread_fun_t* fun;
  void* arg;
#if defined(_WIN32)
  HANDLE handle;
#else
  pthread_t handle;
#endif
} thread_t;

#if defined(_WIN32)
static DWORD WINAPI thread_entry(LPVOID arg) {
  thread_t* thread = (thread_t*)arg;
  thread->fun(thread->arg);
  return 0;
}
#else
static void* thread_entry(void* arg) {
  thread_t* thread = (thread_t*)arg;
  thread->fun(thread->arg);
  return NULL;
}
#endif

static void thread_start(thread_t* thread, thread_fun_t* fun, void* arg) {
  thread->fun = fun;
  thread->arg = arg;
#if defined(_WIN32)
  thread->handle = CreateThread(NULL, 0, thread_entry, thread, 0, NULL);
  CHECK(thread->handle != NULL);
#else
  CHECK(pthread_create(&thread->handle, NULL, thread_entry, thread) == 0);
#endif
}

static void thread_join(thread_t* thread) {
#if defined(_WIN32)
  CHECK(WaitForSingleObject(thread->handle, INFINITE) == WAIT_OBJECT_0);
  CHECK(CloseHandle(thread->handle));
#else
  CHECK(pthread_join(thread->handle, NULL) == 0);
#endif
}

static void shared_heap_worker(void* arg) {
  mi_heap_t* heap = (mi_heap_t*)arg;
  void* ptrs[300];
  for (size_t i = 0; i < 300; ++i) {
    ptrs[i] = mi_heap_malloc(heap, 37 + (i % 3) * 100);
    CHECK(ptrs[i] != NULL);
  }
  for (size_t i = 0; i < 300; ++i) mi_free(ptrs[i]);
}

static void check_concurrent_heap(void) {
  callback_state_t state = {0};
  mi_atomic_store_release(&state.accept, 1);
  mi_heap_t* heap = new_heap(&state);
  thread_t threads[8];
  for (size_t i = 0; i < 8; ++i) thread_start(&threads[i], shared_heap_worker, heap);
  for (size_t i = 0; i < 8; ++i) thread_join(&threads[i]);
  mi_heap_destroy(heap);
  CHECK(mi_atomic_load_acquire(&state.release_calls) > 0);
  CHECK(mi_atomic_load_acquire(&state.acquired_bytes) == mi_atomic_load_acquire(&state.released_bytes));
}

static void check_acquire_only(void) {
  callback_state_t state = {0};
  mi_heap_t* heap = _mi_heap_new_with_page_callbacks(acquire_page, NULL, &state);
  CHECK(heap != NULL);
  CHECK(mi_heap_malloc(heap, 8 * 1024 * 1024) == NULL);
  CHECK(mi_atomic_load_acquire(&state.acquire_calls) == 1);
  CHECK(mi_atomic_load_acquire(&state.acquired_bytes) == 0);
  mi_atomic_store_release(&state.accept, 1);
  void* ptr = mi_heap_malloc(heap, 8 * 1024 * 1024);
  CHECK(ptr != NULL);
  CHECK(mi_atomic_load_acquire(&state.acquire_calls) == 2);
  CHECK(mi_atomic_load_acquire(&state.acquired_bytes) > 0);
  mi_free(ptr);
  mi_heap_collect(heap, true);
  mi_heap_destroy(heap);
  CHECK(mi_atomic_load_acquire(&state.release_calls) == 0);
}

static void observe_release(size_t bytes, void* arg) {
  callback_state_t* state = (callback_state_t*)arg;
  CHECK(bytes > 0);
  mi_atomic_add_relaxed(&state->released_bytes, bytes);
  mi_atomic_increment_relaxed(&state->release_calls);
}

static void check_release_only(bool transfer) {
  callback_state_t state = {0};
  mi_heap_t* heap = _mi_heap_new_with_page_callbacks(NULL, observe_release, &state);
  CHECK(heap != NULL);
  void* ptr = mi_heap_malloc(heap, 8 * 1024 * 1024);
  CHECK(ptr != NULL);
  CHECK(mi_atomic_load_acquire(&state.release_calls) == 0);
  if (transfer) mi_heap_delete(heap);
  else mi_heap_destroy(heap);
  CHECK(mi_atomic_load_acquire(&state.acquire_calls) == 0);
  CHECK(mi_atomic_load_acquire(&state.release_calls) == 1);
  const uintptr_t released = mi_atomic_load_acquire(&state.released_bytes);
  CHECK(released > 0);
  if (transfer) {
    mi_free(ptr);
    mi_collect(true);
    CHECK(mi_atomic_load_acquire(&state.release_calls) == 1);
    CHECK(mi_atomic_load_acquire(&state.released_bytes) == released);
  }
}

static void check_no_callbacks(void) {
  mi_heap_t* heap = _mi_heap_new_with_page_callbacks(NULL, NULL, NULL);
  CHECK(heap != NULL);
  CHECK(mi_heap_malloc(heap, 64) != NULL);
  mi_heap_destroy(heap);
}

int main(void) {
  const size_t sizes[] = {1, 37, 8192, 65536, 1024 * 1024, 8 * 1024 * 1024};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) check_veto(sizes[i], 0, false);
  check_veto(97, 1024 * 1024, false);
  check_veto(128, 0, true);
  check_release_before_destroy();
  check_delete_transfers_charge();
  check_nested_veto();
  check_concurrent_heap();
  check_acquire_only();
  check_release_only(false);
  check_release_only(true);
  check_no_callbacks();
  puts("page callback tests passed");
  return 0;
}
