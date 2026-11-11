#pragma once
#ifndef MI_PAGE_CALLBACKS_H
#define MI_PAGE_CALLBACKS_H
#include "../mimalloc.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Callbacks are fixed at heap creation and can run concurrently on different
   threads using that heap. `bytes` is the full owned-page span, including page
   metadata and padding; it is not the object's requested size or resident RSS.
   Returning false vetoes that allocation without collection or retry after the
   veto. Rejected acquisitions receive no release callback. Accepted pages stay
   charged while cached, and are released once on return, destruction, or
   transfer out of the heap. `done(arg)` runs after the last release on successful
   heap deletion/destruction; on constructor failure the caller still owns arg.
   Callbacks must not unwind through C. Heap users must be quiescent before
   deletion/destruction, and callback state must remain valid until done. */
typedef bool mi_page_acquire_fun(size_t bytes, void* arg);
typedef void mi_page_release_fun(size_t bytes, void* arg);
typedef void mi_page_callbacks_done_fun(void* arg);
mi_decl_export mi_heap_t* _mi_heap_new_with_page_callbacks(
    mi_page_acquire_fun* acquire, mi_page_release_fun* release,
    void* arg, mi_page_callbacks_done_fun* done) mi_attr_noexcept;
#ifdef __cplusplus
}
#endif
#endif
