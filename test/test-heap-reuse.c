/* Heap cache regression test, distributed under the MIT license (see LICENSE). */
#include <stdint.h>
#include <stdlib.h>
#include "mimalloc.h"
#include "mimalloc/atomic.h"
#include "testhelper.h"
#if !defined(_WIN32)
#include <sched.h>
#endif

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

#if defined(_WIN32)
static void thread_yield(void) { SwitchToThread(); }
#else
static void thread_yield(void) { sched_yield(); }
#endif

// ---------------------------------------------------------------------------
// Heap address reuse
// ---------------------------------------------------------------------------
typedef struct heap_reuse_state_s {
  mi_heap_t* original;
  mi_heap_t* replacement;
  _Atomic(uintptr_t) phase;
} heap_reuse_state_t;

static bool heap_reuse_worker(void* arg) {
  heap_reuse_state_t* state = (heap_reuse_state_t*)arg;
  bool result = (mi_heap_malloc(state->original, 64) != NULL);
  result &= (mi_heap_malloc(state->original, 64) != NULL);
  mi_atomic_store_release(&state->phase, 1);
  // Keep the cached theap alive without using the heap during destruction.
  while (mi_atomic_load_acquire(&state->phase) != 2) thread_yield();
  if (state->replacement == NULL) return false;
  // Use a different size class so old cached blocks cannot satisfy the request.
  void* ptr = mi_heap_malloc(state->replacement, 8192);
  result &= (ptr != NULL);
  mi_free(ptr);
  return result;
}

static bool test_cached_heap_address_reuse(void) {
  heap_reuse_state_t state = {0};
  state.original = mi_heap_new();
  if (state.original == NULL) return false;
  const uintptr_t address = (uintptr_t)state.original;
  thread_t worker;
  if (!thread_start(&worker, heap_reuse_worker, &state)) {
    mi_heap_destroy(state.original);
    return false;
  }
  while (mi_atomic_load_acquire(&state.phase) != 1) thread_yield();
  mi_heap_destroy(state.original);
  mi_heap_t* heaps[256];
  size_t count = 0;
  while (count < sizeof(heaps) / sizeof(heaps[0])) {
    mi_heap_t* heap = mi_heap_new();
    if (heap == NULL) break;
    heaps[count++] = heap;
    if ((uintptr_t)heap == address) {
      state.replacement = heap;
      break;
    }
  }
  bool result = (state.replacement != NULL);
  mi_atomic_store_release(&state.phase, 2);
  result &= thread_join(&worker);
  for (size_t i = 0; i < count; ++i) mi_heap_destroy(heaps[i]);
  return result;
}

// ---------------------------------------------------------------------------
// Main testing
// ---------------------------------------------------------------------------
int main(void) {
  mi_option_disable(mi_option_verbose);
  CHECK("cached-heap-address-reuse", test_cached_heap_address_reuse());
  return print_test_summary();
}
