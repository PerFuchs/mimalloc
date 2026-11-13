/* Heap cache regression test, distributed under the MIT license (see LICENSE). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <mimalloc.h>
#include <mimalloc/atomic.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "heap reuse: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
  abort(); \
} } while (0)

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
static void thread_yield(void) { SwitchToThread(); }
#else
static void* thread_entry(void* arg) {
  thread_t* thread = (thread_t*)arg;
  thread->fun(thread->arg);
  return NULL;
}
static void thread_yield(void) { sched_yield(); }
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

typedef struct heap_reuse_state_s {
  mi_heap_t* original;
  mi_heap_t* replacement;
  _Atomic(uintptr_t) phase;
} heap_reuse_state_t;

static void heap_reuse_worker(void* arg) {
  heap_reuse_state_t* state = (heap_reuse_state_t*)arg;
  CHECK(mi_heap_malloc(state->original, 64) != NULL);
  CHECK(mi_heap_malloc(state->original, 64) != NULL);
  mi_atomic_store_release(&state->phase, 1);
  // Keep the cached theap alive without using the heap during destruction.
  while (mi_atomic_load_acquire(&state->phase) != 2) thread_yield();
  // Use a different size class so old cached blocks cannot satisfy the request.
  void* ptr = mi_heap_malloc(state->replacement, 8192);
  CHECK(ptr != NULL);
  mi_free(ptr);
}

static void check_cached_heap_address_reuse(void) {
  heap_reuse_state_t state = {0};
  state.original = mi_heap_new();
  CHECK(state.original != NULL);
  const uintptr_t address = (uintptr_t)state.original;
  thread_t worker;
  thread_start(&worker, heap_reuse_worker, &state);
  while (mi_atomic_load_acquire(&state.phase) != 1) thread_yield();
  mi_heap_destroy(state.original);
  mi_heap_t* heaps[256];
  size_t count = 0;
  while (count < sizeof(heaps) / sizeof(heaps[0])) {
    mi_heap_t* heap = mi_heap_new();
    CHECK(heap != NULL);
    heaps[count++] = heap;
    if ((uintptr_t)heap == address) {
      state.replacement = heap;
      break;
    }
  }
  CHECK(state.replacement != NULL);
  mi_atomic_store_release(&state.phase, 2);
  thread_join(&worker);
  for (size_t i = 0; i < count; ++i) mi_heap_destroy(heaps[i]);
}

int main(void) {
  check_cached_heap_address_reuse();
  puts("heap reuse test passed");
  return 0;
}
