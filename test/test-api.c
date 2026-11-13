/* ----------------------------------------------------------------------------
Copyright (c) 2018-2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Walloc-size-larger-than="
#endif

/*
Testing allocators is difficult as bugs may only surface after particular
allocation patterns. The main approach to testing _mimalloc_ is therefore
to have extensive internal invariant checking (see `page_is_valid` in `page.c`
for example), which is enabled in debug mode with `-DMI_DEBUG_FULL=ON`.
The main testing is then to run `mimalloc-bench` [1] using full invariant checking
to catch any potential problems over a wide range of intensive allocation bench
marks.

However, this does not test well for the entire API surface. In this test file
we therefore test the API over various inputs. Please add more tests :-)

[1] https://github.com/daanx/mimalloc-bench
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>

#ifdef __cplusplus
#include <vector>
#endif

#include "mimalloc.h"
// #include "mimalloc/internal.h"
#include "mimalloc/types.h" // for MI_DEBUG and MI_PAGE_MAX_OVERALLOC_ALIGN

#include "testhelper.h"


// ---------------------------------------------------------------------------
// Test functions
// ---------------------------------------------------------------------------
bool test_theap1(void);
bool test_theap2(void);
bool test_theap_arena_destroy(void);
bool test_theap_arena_delete(void);
bool test_stl_allocator1(void);
bool test_stl_allocator2(void);

bool test_stl_theap_allocator1(void);
bool test_stl_theap_allocator2(void);
bool test_stl_theap_allocator3(void);
bool test_stl_theap_allocator4(void);

static bool test_zero_aligned_first(void);
static bool test_veto(size_t size, size_t alignment, bool zero);
static bool test_release_before_destroy(void);
static bool test_delete_transfers_charge(void);
static bool test_nested_veto(void);
static bool test_concurrent_heap(void);
static bool test_acquire_only(void);
static bool test_release_only(bool transfer);
static bool test_no_callbacks(void);

static bool mem_has_vals(const uint8_t* p, size_t size, uint8_t val) {
  if (p==NULL) return false;
  for (size_t i = 0; i < size; ++i) {
    if (p[i] != val) return false;
  }
  return true;
}
static bool mem_is_zero(const void* p, size_t size) {
  return mem_has_vals((const uint8_t*)p,size,0);
}

// ---------------------------------------------------------------------------
// Main testing
// ---------------------------------------------------------------------------
int main(void) {
  mi_option_disable(mi_option_verbose);

  #if 1
  #if defined(__cplusplus) && !defined(_MSC_VER)
  CHECK_BODY("c++ new-handler") {
    std::set_new_handler([]{ throw std::bad_alloc(); });
    void* p = mi_new_nothrow(SIZE_MAX/2);
    result = (p==NULL);
  }
  CHECK_BODY("c++ new handler2") {
    try {
      void* p = mi_new_n(SIZE_MAX/2, 4);
      (void)(p);
      result = false;
    }
    catch(std::bad_alloc) {
      result = true;
    }
  }
  #endif
  #endif

  // ---------------------------------------------------
  // Malloc
  // ---------------------------------------------------

  CHECK_BODY("malloc-zero") {
    void* p = mi_malloc(0);
    result = (p != NULL);
    mi_free(p);
  };
  CHECK_BODY("malloc-nomem1") {
    result = (mi_malloc((size_t)PTRDIFF_MAX + (size_t)1) == NULL);
  };
  CHECK_BODY("malloc-free-null") {
    mi_free(NULL);
  };
  #if MI_INTPTR_BITS > 32
  CHECK_BODY("malloc-free-invalid-low") {
    mi_cfree((void*)(MI_ZU(0x0000000003990080))); // issue #1087
  };
  #endif
  CHECK_BODY("calloc-overflow") {
    // use (size_t)&mi_calloc to get some number without triggering compiler warnings
    result = (mi_calloc((size_t)&mi_calloc,SIZE_MAX/1000) == NULL);
  };
  CHECK_BODY("malloc-large") {   // see PR #544.
    void* p = mi_malloc(67108872);
    mi_free(p);
  };
  
  CHECK_BODY("calloc0") {
    void* p = mi_calloc(0,1000);
    const size_t usable = mi_usable_size(p);    
    result = (usable <= 16);
    mi_free(p);
  };
  
  CHECK_BODY("mi_urealloc_invalid") {
    void* p = mi_malloc(64);
    size_t pre, post;
    void* q = mi_urealloc((char*)p + 3, 32, &pre, &post);
    mi_free(p);
    result = (q==NULL || q==(uint8_t*)p+3);
  }
  
  // ---------------------------------------------------
  // Extended
  // ---------------------------------------------------
  CHECK_BODY("posix_memalign1") {
    void* p = &p;
    int err = mi_posix_memalign(&p, sizeof(void*), 32);
    result = ((err==0 && (uintptr_t)p % sizeof(void*) == 0) || p==&p);
    mi_free(p);
  };
  CHECK_BODY("posix_memalign_no_align") {
    void* p = &p;
    int err = mi_posix_memalign(&p, 3, 32);
    result = (err==EINVAL && p==&p);
  };
  CHECK_BODY("posix_memalign_zero") {
    void* p = &p;
    int err = mi_posix_memalign(&p, sizeof(void*), 0);
    mi_free(p);
    result = (err==0);
  };
  CHECK_BODY("posix_memalign_nopow2") {
    void* p = &p;
    int err = mi_posix_memalign(&p, 3*sizeof(void*), 32);
    result = (err==EINVAL && p==&p);
  };
  CHECK_BODY("posix_memalign_nomem") {
    void* p = &p;
    int err = mi_posix_memalign(&p, sizeof(void*), SIZE_MAX);
    result = (err==ENOMEM && p==&p);
  };

  // ---------------------------------------------------
  // Aligned API
  // ---------------------------------------------------
  CHECK_BODY("malloc-aligned1") {
    void* p = mi_malloc_aligned(32,32); result = (p != NULL && (uintptr_t)(p) % 32 == 0); mi_free(p);
  };
  CHECK_BODY("malloc-aligned2") {
    void* p = mi_malloc_aligned(48,32); result = (p != NULL && (uintptr_t)(p) % 32 == 0); mi_free(p);
  };
  CHECK_BODY("malloc-aligned3") {
    void* p1 = mi_malloc_aligned(48,32); bool result1 = (p1 != NULL && (uintptr_t)(p1) % 32 == 0);
    void* p2 = mi_malloc_aligned(48,32); bool result2 = (p2 != NULL && (uintptr_t)(p2) % 32 == 0);
    mi_free(p2);
    mi_free(p1);
    result = (result1&&result2);
  };
  CHECK_BODY("malloc-aligned4") {
    void* p;
    bool ok = true;
    for (int i = 0; i < 8 && ok; i++) {
      p = mi_malloc_aligned(8, 16);
      ok = (p != NULL && (uintptr_t)(p) % 16 == 0); mi_free(p);
    }
    result = ok;
  };
  CHECK_BODY("malloc-aligned5") {
    void* p = mi_malloc_aligned(4097,4096);
    size_t usable = mi_usable_size(p);
    result = (usable >= 4097 && usable < 16000);
    fprintf(stderr, "malloc_aligned5: usable size: %zi.  ", usable);
    mi_free(p);
  };
  /*
  CHECK_BODY("malloc-aligned6") {
    bool ok = true;
    for (size_t align = 1; align <= MI_PAGE_MAX_OVERALLOC_ALIGN && ok; align *= 2) {
      void* ps[8];
      for (int i = 0; i < 8 && ok; i++) {
        ps[i] = mi_malloc_aligned(align*13  // size
                                 , align);
        if (ps[i] == NULL || (uintptr_t)(ps[i]) % align != 0) {
          ok = false;
        }
      }
      for (int i = 0; i < 8 && ok; i++) {
        mi_free(ps[i]);
      }
    }
    result = ok;
  };
  */
  CHECK_BODY("malloc-aligned7") {
    void* p = mi_malloc_aligned(1024,MI_PAGE_MAX_OVERALLOC_ALIGN);
    mi_free(p);
    result = ((uintptr_t)p % MI_PAGE_MAX_OVERALLOC_ALIGN) == 0;
  };
  CHECK_BODY("malloc-aligned8") {
    bool ok = true;
    for (int i = 0; i < 5 && ok; i++) {
      int n = (1 << i);
      void* p = mi_malloc_aligned(1024, n * MI_PAGE_MAX_OVERALLOC_ALIGN);
      ok = ((uintptr_t)p % (n*MI_PAGE_MAX_OVERALLOC_ALIGN)) == 0;
      mi_free(p);
    }
    result = ok;
  };
  
  CHECK_BODY("malloc-aligned9") { // test large alignments
    bool ok = true;
    void* p[8];
    const int max_align_shift =
      #if SIZE_MAX > UINT32_MAX
      28  /* up to 64 MiB alignment */
      #else
      20
      #endif
      ;
    size_t sizes[8] = { 8, 512, 1024 * 1024, MI_PAGE_MAX_OVERALLOC_ALIGN, MI_PAGE_MAX_OVERALLOC_ALIGN + 1, 2 * MI_PAGE_MAX_OVERALLOC_ALIGN, 8 * MI_PAGE_MAX_OVERALLOC_ALIGN, 0 };
    for (int i = 0; i < max_align_shift && ok; i++) {
      int align = (1 << i);
      for (int j = 0; j < 8 && ok; j++) {
        p[j] = mi_zalloc_aligned(sizes[j], align);
        ok = ((uintptr_t)p[j] % align) == 0;
      }
      for (int j = 0; j < 8; j++) {
        mi_free(p[j]);
      }
    }
    result = ok;
  };
  
  CHECK_BODY("malloc-aligned9a") { // test large alignments
    void* p = mi_zalloc_aligned(1024 * 1024, 2);
    mi_free(p);
    p = mi_zalloc_aligned(1024 * 1024, 2);
    mi_free(p);
    result = true;
  };
  
  CHECK_BODY("malloc-aligned10") {
    bool ok = true;
    void* p[10+1];
    int align;
    int j;
    for(j = 0, align = 1; j <= 10 && ok; align *= 2, j++ ) {
      p[j] = mi_malloc_aligned(43 + align, align);
      ok = ((uintptr_t)p[j] % align) == 0;
    }
    for ( ; j > 0; j--) {
      mi_free(p[j-1]);
    }
    result = ok;
  }
  //CHECK_BODY("malloc_aligned11") {
  //  mi_theap_t* theap = mi_theap_new();
  //  void* p = mi_theap_malloc_aligned(theap, 33554426, 8);
  //  result = mi_theap_contains_block(theap, p);
  //  mi_theap_destroy(theap);
  //}
  CHECK_BODY("mimalloc-aligned12") {
    void* p = mi_malloc_aligned(0x100, 0x100);
    result = (((uintptr_t)p % 0x100) == 0); // #602
    mi_free(p);
  }
  
  CHECK_BODY("mimalloc-aligned13") {
    bool ok = true;
    for( size_t size = 1; size <= (MI_SMALL_SIZE_MAX * 2) && ok; size++ ) {
      for(size_t align = 1; align <= size && ok; align *= 2 ) {
        void* p[10];
        for(int i = 0; i < 10 && ok; i++) {
          p[i] = mi_malloc_aligned(size,align);;
          ok = (p[i] != NULL && ((uintptr_t)(p[i]) % align) == 0);
        }
        for(int i = 0; i < 10 && ok; i++) {
          mi_free(p[i]);
        }
        /*
        if (ok && align <= size && ((size + MI_PADDING_SIZE) & (align-1)) == 0) {
          size_t bsize = mi_good_size(size);
          ok = (align <= bsize && (bsize & (align-1)) == 0);
        }
        */
      }
    }
    result = ok;
  }
  CHECK_BODY("malloc-aligned-at1") {
    void* p = mi_malloc_aligned_at(48,32,0); result = (p != NULL && ((uintptr_t)(p) + 0) % 32 == 0); mi_free(p);
  };
  CHECK_BODY("malloc-aligned-at2") {
    void* p = mi_malloc_aligned_at(50,32,8); result = (p != NULL && ((uintptr_t)(p) + 8) % 32 == 0); mi_free(p);
  };
  CHECK_BODY("memalign1") {
    void* p;
    bool ok = true;
    for (int i = 0; i < 8 && ok; i++) {
      p = mi_memalign(16,8);
      ok = (p != NULL && (uintptr_t)(p) % 16 == 0); mi_free(p);
    }
    result = ok;
  };
  CHECK_BODY("zalloc-csize-large") {
    // mi_theap_zalloc_csize must zero its large branch too: it used to
    // delegate sizes above MI_SMALL_SIZE_MAX to the plain malloc path.
    mi_theap_t* theap = mi_theap_get_default();
    size_t size = MI_SMALL_SIZE_MAX + 64;
    bool ok = true;
    for (int round = 0; round < 16 && ok; round++) {
      uint8_t* junk = (uint8_t*)mi_theap_malloc(theap, size);
      memset(junk, 0xAB, size);
      mi_free(junk);
      uint8_t* z = (uint8_t*)mi_theap_zalloc_csize(theap, size);
      for (size_t i = 0; i < size; i++) {
        if (z[i] != 0) { ok = false; break; }
      }
      mi_free(z);
    }
    result = ok;
  };
  CHECK_BODY("zalloc-aligned-small1") {
    size_t zalloc_size = MI_SMALL_SIZE_MAX / 2;
    uint8_t* p = (uint8_t*)mi_zalloc_aligned(zalloc_size, MI_MAX_ALIGN_SIZE * 2);
    result = mem_is_zero(p, zalloc_size);
    mi_free(p);
  };
  CHECK_BODY("rezalloc_aligned-small1") {
    size_t zalloc_size = MI_SMALL_SIZE_MAX / 2;
    uint8_t* p = (uint8_t*)mi_zalloc_aligned(zalloc_size, MI_MAX_ALIGN_SIZE * 2);
    result = mem_is_zero(p, zalloc_size);
    zalloc_size *= 3;
    p = (uint8_t*)mi_rezalloc_aligned(p, zalloc_size, MI_MAX_ALIGN_SIZE * 2);
    result = result && mem_is_zero(p, zalloc_size);
    mi_free(p);
  };

  CHECK_BODY("rezalloc_aligned_zeros") {  // issue #763
    size_t alignment = 1024;
    size_t n = 1024 * 6;
    void* ptr = mi_zalloc_aligned(n, alignment);
    assert(mem_is_zero(ptr,n));
    memset(ptr,123,n/2);
    
    ptr = mi_rezalloc_aligned(ptr, n/2, alignment);
    assert(mem_has_vals((uint8_t*)ptr,n/2,123));
    
    ptr = mi_rezalloc_aligned(ptr, n, alignment);
    assert(mem_has_vals((uint8_t*)ptr,n/2,123));
    result = mem_is_zero((uint8_t*)ptr + n/2, n/2);    
  }

  // ---------------------------------------------------
  // Reallocation
  // ---------------------------------------------------
  CHECK_BODY("realloc-null") {
    void* p = mi_realloc(NULL,4);
    result = (p != NULL);
    mi_free(p);
  };

  CHECK_BODY("realloc-null-sizezero") {
    void* p = mi_realloc(NULL,0);  // <https://en.cppreference.com/w/c/memory/realloc> "If ptr is NULL, the behavior is the same as calling malloc(new_size)."
    result = (p != NULL);
    mi_free(p);
  };

  CHECK_BODY("realloc-sizezero") {
    void* p = mi_malloc(4);
    void* q = mi_realloc(p, 0);
    result = (q != NULL);
    mi_free(q);
  };

  CHECK_BODY("reallocarray-null-sizezero") {
    void* p = mi_reallocarray(NULL,0,16);  // issue #574
    result = (p != NULL && errno == 0);
    mi_free(p);
  };

  CHECK_BODY("realloc-guarded") {      // issue #1304
 	  void* shared_ptr = NULL;
    for (int iterations = 0; iterations < 64; ++iterations) {
      for (int i = 0; i < 1024; ++i) {
        shared_ptr = mi_realloc(shared_ptr, i * 64);
      }
    }
  }

  // ---------------------------------------------------
  // Small allocations
  // ---------------------------------------------------
  CHECK_BODY("free_small1") {
    for(size_t n = 1; n < MI_SMALL_SIZE_MAX; n *=2) {
      const size_t size = n*sizeof(int);
      int* p = (int*)mi_zalloc(size);
      p[n-1] = 42;
      mi_free_size(p,size);
    }
  }

  CHECK_BODY("free_small2") {
    for(size_t n = 1; n < MI_SMALL_SIZE_MAX; n *=2) {
      const size_t size = n*sizeof(int);
      int* p = (int*)mi_zalloc(size);
      p[n-1] = 42;
      p = (int*)mi_rezalloc(p, size + MI_SMALL_SIZE_MAX);
      mi_free_size(p,size + MI_SMALL_SIZE_MAX);
    }
  }
  
  CHECK_BODY("free_small3") {
    for(size_t n = 1; n < MI_SMALL_SIZE_MAX/sizeof(int); n *=2) {
      const size_t size = n*sizeof(int);
      int* p = (int*)mi_zalloc_small(size);
      p[n-1] = 42;
      mi_free_small(p);
    }
  }

  CHECK_BODY("free_small4") {
    for(size_t n = 1; n < MI_SMALL_SIZE_MAX/sizeof(int); n *=2) {
      const size_t size = n*sizeof(int);
      int* p = (int*)mi_zalloc_small(size);
      p[n-1] = 42;
      mi_free(p);
    }
  }
  // ---------------------------------------------------
  // Returned block sizes
  // ---------------------------------------------------
  CHECK_BODY("umalloc1") {
    for(size_t size = 1; size <= 32*MI_MiB; size *= 2 ) {
      size_t bsize;
      void* p = mi_umalloc(size,&bsize);
      assert(bsize >= size);
      size_t pre_size;
      size_t post_size;
      p = mi_urealloc(p, size + 1024, &pre_size, &post_size);
      assert(pre_size == bsize);
      assert(post_size >= size + 1024);
      size_t fsize;
      mi_ufree(p,&fsize);
      assert(fsize == post_size);
    }
  }

  #if (MI_INTPTR_SIZE > 4)
  CHECK_BODY("arena_reserve") {
    result = (0==mi_reserve_os_memory(16*MI_GiB,false,true));
  }
  #endif

  // ---------------------------------------------------
  // Heaps
  // ---------------------------------------------------

  CHECK_BODY("heap-os1") {
    // @zoxc opus bug #2.
    mi_heap_t* h = mi_heap_new();
    void* p = mi_heap_malloc_aligned(h, 1<<20, 2<<20);   // forced OS allocation
    mi_heap_delete(h);
    mi_free(p);                                          // SIGSEGV
  }

  CHECK_BODY("heap-os2") {
    // @zoxc opus bug #3.
    mi_collect(true);
    mi_stats_t_decl(stats0); 
    mi_stats_get(&stats0);

    mi_heap_t* h = mi_heap_new();
    long failed = 0;      
    for(int i = 0; i < 10; i++) {
      int* p = (int*)mi_heap_malloc_aligned(h, 1<<20, 2<<20);   // forced OS allocation
      if (p==NULL) {
        failed++;
      }
      else {
        p[0] = 42;
      }
    }
    mi_heap_destroy(h);

    mi_collect(true);
    mi_stats_t_decl(stats1); 
    mi_stats_get(&stats1);    
    result = (stats0.pages.current == stats1.pages.current);    
    if (!result) {
      fprintf(stderr, "heap-os2: pages: %ld != %ld (failed: %ld)\n", (long)stats0.pages.current, (long)stats1.pages.current, failed);
    }
  }

  #define NHEAPS (1000)
  CHECK_BODY("heap-many") {    // check creating many heaps and threadlocals, see issue #1358
    mi_heap_t* heaps[NHEAPS];
    for (size_t i = 0; i < NHEAPS; i++) {
      heaps[i] = mi_heap_new();
      if (heaps[i] == NULL) { result = false; break; };
      if (mi_heap_malloc(heaps[i], 32) == NULL) { result = false; break; }
    }
    for (size_t i = 0; i < NHEAPS; i++) {
      mi_heap_destroy(heaps[i]);
    }  
  }

  //CHECK("theap_destroy", test_theap1());
  //CHECK("theap_delete", test_theap2());
  //CHECK("theap_arena_destroy", test_theap_arena_destroy());
  //CHECK("theap_arena_delete", test_theap_arena_delete());

  
  // ---------------------------------------------------
  // Page callbacks
  // ---------------------------------------------------
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

  // ---------------------------------------------------
  // Threads
  // ---------------------------------------------------
  CHECK_BODY("zero_aligned_first") {
    result = mi_run_on_thread(&test_zero_aligned_first);
  }
  
  //mi_stats_print(NULL);

  // ---------------------------------------------------
  // various
  // ---------------------------------------------------
  #if !defined(MI_TRACK_ASAN)   // realpath may leak with ASAN enabled (as the ASAN allocator intercepts it)
  CHECK_BODY("realpath") {
    char* s = mi_realpath( ".", NULL );
    // printf("realpath: %s\n",s);
    mi_free(s);
  };
  #endif

  CHECK("stl_allocator1", test_stl_allocator1());
  CHECK("stl_allocator2", test_stl_allocator2());

	//CHECK("stl_theap_allocator1", test_stl_theap_allocator1());
	//CHECK("stl_theap_allocator2", test_stl_theap_allocator2());
	//CHECK("stl_theap_allocator3", test_stl_theap_allocator3());
	//CHECK("stl_theap_allocator4", test_stl_theap_allocator4());

  // ---------------------------------------------------
  // Done
  // ---------------------------------------------------[]
  return print_test_summary();
}

// ---------------------------------------------------
// Larger test functions
// ---------------------------------------------------

/*
bool test_theap1(void) {
  mi_theap_t* theap = mi_theap_new();
  int* p1 = mi_theap_malloc_tp(theap,int);
  int* p2 = mi_theap_malloc_tp(theap,int);
  *p1 = *p2 = 43;
  mi_theap_destroy(theap);
  return true;
}

bool test_theap2(void) {
  mi_theap_t* theap = mi_theap_new();
  int* p1 = mi_theap_malloc_tp(theap,int);
  int* p2 = mi_theap_malloc_tp(theap,int);
  mi_theap_delete(theap);
  *p1 = 42;
  mi_free(p1);
  mi_free(p2);
  return true;
}

bool test_theap_arena_destroy(void) {
  mi_arena_id_t arena_id = NULL;
  if (mi_reserve_os_memory_ex(64 * 1024 * 1024, true, false, true, &arena_id) != 0) {
    return false;
  }
  mi_theap_t* theap = mi_theap_new_ex(0, true, arena_id);
  if (theap == NULL) {
    return false;
  }
  mi_theap_destroy(theap);
  return true;
}

bool test_theap_arena_delete(void) {
  mi_arena_id_t arena_id = NULL;
  if (mi_reserve_os_memory_ex(64 * 1024 * 1024, true, false, true, &arena_id) != 0) {
    return false;
  }
  mi_theap_t* theap = mi_theap_new_ex(0, true, arena_id);
  if (theap == NULL) {
    return false;
  }
  mi_theap_delete(theap);
  return true;
}
*/
bool test_stl_allocator1(void) {
#ifdef __cplusplus
  std::vector<int, mi_stl_allocator<int> > vec;
  vec.push_back(1);
  vec.pop_back();
  return vec.size() == 0;
#else
  return true;
#endif
}

struct some_struct  { int i; int j; double z; };

bool test_stl_allocator2(void) {
#ifdef __cplusplus
  std::vector<some_struct, mi_stl_allocator<some_struct> > vec;
  vec.push_back(some_struct());
  vec.pop_back();
  return vec.size() == 0;
#else
  return true;
#endif
}

/*
bool test_stl_theap_allocator1(void) {
#ifdef __cplusplus
  std::vector<some_struct, mi_theap_stl_allocator<some_struct> > vec;
  vec.push_back(some_struct());
  vec.pop_back();
  return vec.size() == 0;
#else
  return true;
#endif
}

bool test_stl_theap_allocator2(void) {
#ifdef __cplusplus
  std::vector<some_struct, mi_theap_destroy_stl_allocator<some_struct> > vec;
  vec.push_back(some_struct());
  vec.pop_back();
  return vec.size() == 0;
#else
  return true;
#endif
}

bool test_stl_theap_allocator3(void) {
#ifdef __cplusplus
	mi_theap_t* theap = mi_theap_new();
	bool good = false;
	{
		mi_theap_stl_allocator<some_struct> myAlloc(theap);
		std::vector<some_struct, mi_theap_stl_allocator<some_struct> > vec(myAlloc);
		vec.push_back(some_struct());
		vec.pop_back();
		good = vec.size() == 0;
	}
	mi_theap_delete(theap);
  return good;
#else
  return true;
#endif
}

bool test_stl_theap_allocator4(void) {
#ifdef __cplusplus
	mi_theap_t* theap = mi_theap_new();
	bool good = false;
	{
		mi_theap_destroy_stl_allocator<some_struct> myAlloc(theap);
		std::vector<some_struct, mi_theap_destroy_stl_allocator<some_struct> > vec(myAlloc);
		vec.push_back(some_struct());
		vec.pop_back();
		good = vec.size() == 0;
	}
	mi_theap_destroy(theap);
  return good;
#else
  return true;
#endif
}
*/

// ---------------------------------------------------------------------------
// Test a zero size aligned allocation as the very first allocation of a fresh thread.
// ---------------------------------------------------------------------------

static bool test_zero_aligned_first(void) {
  void* p = mi_malloc_aligned(0, 16);     // must be the first mimalloc call on this thread
  bool res = (p != NULL && (uintptr_t)(p) % 16 == 0);
  mi_free(p);
  p = mi_zalloc_aligned(0, 32);
  res = res && (p != NULL && (uintptr_t)(p) % 32 == 0);
  mi_free(p);
  return res;
}

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
  assert(bytes > 0);
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
  assert(bytes > 0);
  mi_atomic_add_relaxed(&state->released_bytes, bytes);
  mi_atomic_increment_relaxed(&state->release_calls);
}

static void release_page(size_t bytes, void* arg) {
  callback_state_t* state = (callback_state_t*)arg;
  assert(bytes > 0);
  const uintptr_t before = mi_atomic_add_relaxed(&state->released_bytes, bytes);
  if (before + bytes > mi_atomic_load_acquire(&state->acquired_bytes)) {
    mi_atomic_increment_relaxed(&state->callback_errors);
  }
  mi_atomic_increment_relaxed(&state->release_calls);
}

// Error counters default to zero; callers specify only the expected call counts.
static bool check_state(callback_state_t* state, uintptr_t expected_acquire_calls, uintptr_t expected_release_calls) {
  const uintptr_t acquire_calls = mi_atomic_load_acquire(&state->acquire_calls);
  const uintptr_t release_calls = mi_atomic_load_acquire(&state->release_calls);
  const uintptr_t deferred_after_veto = mi_atomic_load_acquire(&state->deferred_after_veto);
  const uintptr_t errors = mi_atomic_load_acquire(&state->errors);
  const uintptr_t callback_errors = mi_atomic_load_acquire(&state->callback_errors);
  const bool result = (acquire_calls == expected_acquire_calls && release_calls == expected_release_calls &&
                       deferred_after_veto == 0 && errors == 0 && callback_errors == 0);
  if (!result) {
    fprintf(stderr, "page callbacks: acquire calls %zu (expected %zu), release calls %zu (expected %zu), "
                    "deferred calls after veto %zu, allocator errors %zu, callback errors %zu (expected zero)\n",
            (size_t)acquire_calls, (size_t)expected_acquire_calls, (size_t)release_calls, (size_t)expected_release_calls,
            (size_t)deferred_after_veto, (size_t)errors, (size_t)callback_errors);
  }
  return result;
}

static bool accounting_is_balanced(callback_state_t* state) {
  const uintptr_t acquired = mi_atomic_load_acquire(&state->acquired_bytes);
  const uintptr_t released = mi_atomic_load_acquire(&state->released_bytes);
  if (acquired != released) {
    fprintf(stderr, "page callbacks: acquired bytes %zu != released bytes %zu\n", (size_t)acquired, (size_t)released);
  }
  return (acquired == released);
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
  bool result = check_state(&state, 1, 0);
  result &= (ptr == NULL);
  mi_register_deferred_free(NULL, NULL);
  mi_register_error(NULL, NULL);
  mi_free(ptr);
  mi_atomic_store_release(&state.accept, 1);
  ptr = (alignment != 0 ? mi_heap_malloc_aligned(heap, size, alignment)
                       : (zero ? mi_heap_zalloc(heap, size) : mi_heap_malloc(heap, size)));
  if (ptr == NULL) {
    fprintf(stderr, "page callbacks: recovery allocation failed (size %zu, alignment %zu, zero %d)\n", size, alignment, (int)zero);
    mi_heap_destroy(heap);
    return false;
  }
  result &= check_state(&state, 2, 0);
  if (alignment != 0) result &= ((uintptr_t)ptr % alignment == 0);
  if (zero) {
    for (size_t i = 0; i < size; ++i) result &= (((unsigned char*)ptr)[i] == 0);
  }
  mi_free(ptr);
  mi_heap_collect(heap, true);
  mi_heap_destroy(heap);
  result &= check_state(&state, 2, 1);
  return (result && accounting_is_balanced(&state));
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
  bool result = (check_state(&state, 1, 1) && accounting_is_balanced(&state));
  const uintptr_t released = mi_atomic_load_acquire(&state.released_bytes);
  mi_heap_destroy(heap);
  result &= check_state(&state, 1, 1);
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
  result &= (check_state(&state, 1, 1) && accounting_is_balanced(&state));
  const uintptr_t released = mi_atomic_load_acquire(&state.released_bytes);
  mi_free(ptr);
  mi_collect(true);
  result &= check_state(&state, 1, 1);
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
  result &= check_state(&state.accounting, 1, 1);
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
  // Exercise reusable pages instead of guarded singleton allocations.
  mi_theap_guarded_set_sample_rate(mi_heap_theap(heap), 0, 0);
  void* ptrs[300];
  bool result = true;
  for (size_t i = 0; i < 300; ++i) {
    ptrs[i] = mi_heap_malloc(heap, 37 + (i % 3) * 100);
    result &= (ptrs[i] != NULL);
  }
  // Leave one live block so thread exit abandons a reusable page.
  for (size_t i = 1; i < 300; ++i) mi_free(ptrs[i]);
  return result;
}

static bool reclaim_heap_worker(void* arg) {
  mi_heap_t* heap = (mi_heap_t*)arg;
  mi_theap_guarded_set_sample_rate(mi_heap_theap(heap), 0, 0);
  void* ptr = mi_heap_malloc(heap, 37);
  const bool result = (ptr != NULL);
  mi_free(ptr);
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
  const uintptr_t acquired = mi_atomic_load_acquire(&state.acquire_calls);
  thread_t reclaimer;
  if (thread_start(&reclaimer, reclaim_heap_worker, heap)) result &= thread_join(&reclaimer);
  else result = false;
  result &= (mi_atomic_load_acquire(&state.acquire_calls) == acquired);
  mi_heap_destroy(heap);
  result &= check_state(&state, acquired, acquired);
  return (result && acquired > 0 && accounting_is_balanced(&state));
}

// ---------------------------------------------------------------------------
// Independently optional callbacks
// ---------------------------------------------------------------------------
static bool test_acquire_only(void) {
  callback_state_t state = {0};
  mi_heap_t* heap = _mi_heap_new_with_page_callbacks(acquire_page, NULL, &state);
  if (heap == NULL) return false;
  void* ptr = mi_heap_malloc(heap, 8 * 1024 * 1024);
  bool result = check_state(&state, 1, 0);
  result &= (ptr == NULL && mi_atomic_load_acquire(&state.acquired_bytes) == 0);
  mi_free(ptr);
  mi_atomic_store_release(&state.accept, 1);
  ptr = mi_heap_malloc(heap, 8 * 1024 * 1024);
  result &= (ptr != NULL && mi_atomic_load_acquire(&state.acquired_bytes) > 0);
  mi_free(ptr);
  mi_heap_collect(heap, true);
  mi_heap_destroy(heap);
  result &= check_state(&state, 2, 0);
  return result;
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
  bool result = check_state(&state, 0, 0);
  if (transfer) mi_heap_delete(heap);
  else mi_heap_destroy(heap);
  result &= check_state(&state, 0, 1);
  const uintptr_t released = mi_atomic_load_acquire(&state.released_bytes);
  result &= (released > 0);
  if (transfer) {
    mi_free(ptr);
    mi_collect(true);
    result &= check_state(&state, 0, 1);
    result &= (mi_atomic_load_acquire(&state.released_bytes) == released);
  }
  return result;
}

static bool test_no_callbacks(void) {
  mi_heap_t* heap = _mi_heap_new_with_page_callbacks(NULL, NULL, NULL);
  if (heap == NULL) return false;
  bool result = (mi_heap_malloc(heap, 64) != NULL);
  mi_heap_destroy(heap);
  return result;
}
