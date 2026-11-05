#ifndef NODEHUB_CORE_UTILS_H
#define NODEHUB_CORE_UTILS_H

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h> // For mutex helpers
#include <threads.h>  // For thread helpers (C11)

// From https://hwisnu.bearblog.dev/giving-c-a-superpower-custom-header-file-safe_ch/

//==============================================================================
// ATTRIBUTES & BUILTINS
//==============================================================================

// The magic behind CLEANUP: zero overhead, maximum safety
// NOTE: [[cleanup]] is a C23 feature. __attribute__((cleanup)) is a GCC/Clang extension.
// This will work in C++ with GCC/Clang but is not standard C++.
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define CLEANUP(func) [[cleanup(func)]]
#else
#define CLEANUP(func) __attribute__((cleanup(func)))
#endif

// Branch prediction that actually matters in hot paths
#ifdef __GNUC__
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#endif

//==============================================================================
// SMART POINTERS
//==============================================================================

// --- UniquePtr ---
// NOTE: The blog post did not define UNIQUE_PTR_INIT or unique_ptr_delete.
// A reasonable implementation is provided below.

typedef struct {
    void* ptr;
    void (*deleter)(void*);
} UniquePtr;

#define UNIQUE_PTR_INIT(p, d) {.ptr = p, .deleter = d}

static inline void unique_ptr_cleanup(UniquePtr* uptr) {
    if (uptr && uptr->ptr && uptr->deleter) {
        uptr->deleter(uptr->ptr);
        uptr->ptr = NULL;
    }
}

#define AUTO_UNIQUE_PTR(name, ptr, deleter) \
    UniquePtr name CLEANUP(unique_ptr_cleanup) = UNIQUE_PTR_INIT(ptr, deleter)

// Manually release the resource, for reassignment.
static inline void unique_ptr_delete(UniquePtr* uptr) {
    unique_ptr_cleanup(uptr);
}


// --- SharedPtr ---
// NOTE: The blog post mentioned shared_ptr_init, shared_ptr_copy, and shared_ptr_delete
// but did not provide their implementations. A reasonable implementation is provided below.

typedef struct {
    void* ptr;
    void (*deleter)(void*);
    size_t* ref_count;
} SharedPtr;

static inline void shared_ptr_delete(SharedPtr* sptr); // Forward declare

static inline void shared_ptr_cleanup(SharedPtr* sptr) {
    shared_ptr_delete(sptr); // Decrement and free if last reference
}

#define AUTO_SHARED_PTR(name) \
    SharedPtr name CLEANUP(shared_ptr_cleanup) = {.ptr = NULL, .deleter = NULL, .ref_count = NULL}


static inline void shared_ptr_init(SharedPtr* sptr, void* ptr, void (*deleter)(void*)) {
    if (!sptr) return;
    sptr->ptr = ptr;
    sptr->deleter = deleter;
    if (ptr) {
        sptr->ref_count = (size_t*)malloc(sizeof(size_t));
        if (sptr->ref_count) {
            *(sptr->ref_count) = 1;
        }
    } else {
        sptr->ref_count = NULL;
    }
}

static inline SharedPtr shared_ptr_copy(SharedPtr* sptr) {
    SharedPtr new_sptr = { .ptr = NULL, .deleter = NULL, .ref_count = NULL };
    if (sptr && sptr->ptr && sptr->ref_count) {
        new_sptr.ptr = sptr->ptr;
        new_sptr.deleter = sptr->deleter;
        new_sptr.ref_count = sptr->ref_count;
        (*(new_sptr.ref_count))++;
    }
    return new_sptr;
}

static inline void shared_ptr_delete(SharedPtr* sptr) {
    if (sptr && sptr->ptr && sptr->ref_count) {
        (*(sptr->ref_count))--;
        if (*(sptr->ref_count) == 0) {
            if (sptr->deleter) {
                sptr->deleter(sptr->ptr);
            }
            free(sptr->ref_count);
        }
        sptr->ptr = NULL;
        sptr->deleter = NULL;
        sptr->ref_count = NULL;
    }
}

//==============================================================================
// DYNAMIC ARRAYS (VECTORS)
//==============================================================================

// NOTE: The DEFINE_VECTOR_TYPE macro in the blog post was truncated ("... (6495 chars omitted)...").
// It cannot be fully reproduced from the article.


//==============================================================================
// SAFE STRING OPERATIONS
//==============================================================================

static inline bool safe_strcpy(char* dest, size_t dest_size, const char* src) {
    if (!dest || dest_size == 0 || !src) return false;
    size_t src_len = strlen(src);
    if (src_len >= dest_size) return false;
    memcpy(dest, src, src_len + 1);
    return true;
}


//==============================================================================
// CONCURRENCY HELPERS
//==============================================================================

// --- RAII Mutexes ---

// RAII-style mutex locking using the CLEANUP attribute.
// The mutex is automatically unlocked when the pointer goes out of scope.
static inline void mutex_unlock_cleanup(pthread_mutex_t** lock) {
    if (lock && *lock) {
        pthread_mutex_unlock(*lock);
    }
}

/* Example Usage:
    pthread_mutex_t my_lock;
    pthread_mutex_init(&my_lock, NULL);
    ...
    {
        pthread_mutex_t* lock_ptr CLEANUP(mutex_unlock_cleanup) = &my_lock;
        pthread_mutex_lock(lock_ptr);
        // ... critical section ...
        if (some_error) return; // Mutex is automatically unlocked
    }
*/

// --- Threading Macros ---
// NOTE: These use C11 threads (<threads.h>). Ensure your compiler supports it.

#define SPAWN_THREAD(name, func, arg) \
    thrd_t name; \
    if (thrd_create(&name, (thrd_start_t)(func), (arg)) != thrd_success) { /* handle error */ }

#define JOIN_THREAD(name) \
    thrd_join(name, NULL)


#endif // NODEHUB_CORE_UTILS_H
