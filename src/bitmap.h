/* ----------------------------------------------------------------------------
Copyright (c) 2019-2023 Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
Concurrent bitmap that can set/reset sequences of bits atomically
---------------------------------------------------------------------------- */
#pragma once
#ifndef MI_BITMAP_H
#define MI_BITMAP_H

/* --------------------------------------------------------------------------------
  Atomic bitmaps:

  `mi_bfield_t`: is a single machine word that can efficiently be bit counted (usually `size_t`)
      each bit usually represents a single MI_ARENA_SLICE_SIZE in an arena (64 KiB).
      We need 16K bits to represent a 1GiB arena.

  `mi_bchunk_t`: a chunk of bfield's of a total of MI_BCHUNK_BITS (= 512 on 64-bit, 256 on 32-bit)
      allocations never span across chunks -- so MI_ARENA_MAX_OBJ_SIZE is the number
      of bits in a chunk times the MI_ARENA_SLICE_SIZE (512 * 64KiB = 32 MiB).
      These chunks are cache-aligned and we can use AVX2/AVX512/NEON/SVE/SVE2/etc. instructions
      to scan for bits (perhaps) more efficiently.

   `mi_bchunkmap_t` == `mi_bchunk_t`: for each chunk we track if it has (potentially) any bit set.
      The chunkmap has 1 bit per chunk that is set if the chunk potentially has a bit set.
      This is used to avoid scanning every chunk. (and thus strictly an optimization)
      It is conservative: it is fine to a bit in the chunk map even if the chunk turns out
      to have no bits set. It is also allowed to briefly have a clear bit even if the
      chunk has bits set, as long as we guarantee that we set the bit later on -- this
      allows us to set the chunkmap bit after we set a bit in the corresponding chunk.

      However, when we clear a bit in a chunk, and the chunk is indeed all clear, we
      cannot safely clear the bit corresponding to the chunk in the chunkmap since it
      may race with another thread setting a bit in the same chunk. Therefore, when
      clearing, we first test if a chunk is clear, then clear the chunkmap bit, and
      then test again to catch any set bits that we missed.

      Since the chunkmap may thus be briefly out-of-sync, this means that we may sometimes
      not find a free page even though it's there (but we accept this as we avoid taking
      full locks). (Another way to do this is to use an epoch but we like to avoid that complexity
      for now).

   `mi_bitmap_t`: a bitmap with N chunks. A bitmap has a chunkmap of MI_BCHUNK_BITS (512)
      and thus has at most 512 chunks (=2^18 bits x 64 KiB slices = 16 GiB max arena size).
      The minimum is 1 chunk which is a 32 MiB arena.

   For now, the implementation assumes MI_HAS_FAST_BITSCAN and uses trailing-zero-count
   and pop-count (but we think it can be adapted work reasonably well on older hardware too)
--------------------------------------------------------------------------------------------- */

// A word-size bit field.
typedef size_t mi_bfield_t;

#define MI_BFIELD_BITS_SHIFT         (MI_SIZE_SHIFT+3)
#define MI_BFIELD_BITS               (1 << MI_BFIELD_BITS_SHIFT)
#define MI_BFIELD_SIZE               (MI_BFIELD_BITS/8)
#define MI_BFIELD_LO_BIT8            (((~(mi_bfield_t)0))/0xFF)         // 0x01010101 ..
#define MI_BFIELD_HI_BIT8            (MI_BFIELD_LO_BIT8 << 7)           // 0x80808080 ..

#define MI_BCHUNK_SIZE               (MI_BCHUNK_BITS / 8)
#define MI_BCHUNK_FIELDS             (MI_BCHUNK_BITS / MI_BFIELD_BITS)  // 8 on both 64- and 32-bit


// A bitmap chunk contains 512 bits on 64-bit  (256 on 32-bit)
typedef mi_decl_align(MI_BCHUNK_SIZE) struct mi_bchunk_s {
  _Atomic(mi_bfield_t) bfields[MI_BCHUNK_FIELDS];
} mi_bchunk_t;


// The chunkmap has one bit per corresponding chunk that is set if the chunk potentially has bits set.
// The chunkmap is itself a chunk.
typedef mi_bchunk_t mi_bchunkmap_t;

#define MI_BCHUNKMAP_BITS             MI_BCHUNK_BITS

#define MI_BITMAP_MAX_CHUNK_COUNT     (MI_BCHUNKMAP_BITS)
#define MI_BITMAP_MIN_CHUNK_COUNT     (1)
#if MI_SIZE_BITS > 32
#define MI_BITMAP_DEFAULT_CHUNK_COUNT     (64)  // 2 GiB on 64-bit -- this is for the page map
#else
#define MI_BITMAP_DEFAULT_CHUNK_COUNT      (1)  
#endif
#define MI_BITMAP_MAX_BIT_COUNT       (MI_BITMAP_MAX_CHUNK_COUNT * MI_BCHUNK_BITS)  // 16 GiB arena
#define MI_BITMAP_MIN_BIT_COUNT       (MI_BITMAP_MIN_CHUNK_COUNT * MI_BCHUNK_BITS)  // 32 MiB arena
#define MI_BITMAP_DEFAULT_BIT_COUNT   (MI_BITMAP_DEFAULT_CHUNK_COUNT * MI_BCHUNK_BITS)  // 2 GiB arena


// An atomic bitmap
typedef mi_decl_align(MI_BCHUNK_SIZE) struct mi_bitmap_s {
  _Atomic(size_t)  chunk_count;      // total count of chunks (0 < N <= MI_BCHUNKMAP_BITS)
  _Atomic(size_t)  chunk_max_clear;  // max chunk index that was once cleared 
  size_t           _padding[MI_BCHUNK_SIZE/MI_SIZE_SIZE - 2];    // suppress warning on msvc
  mi_bchunkmap_t   chunkmap;
  mi_bchunk_t      chunks[MI_BITMAP_DEFAULT_CHUNK_COUNT];        // usually dynamic MI_BITMAP_MAX_CHUNK_COUNT
} mi_bitmap_t;


static inline size_t mi_bitmap_chunk_count(const mi_bitmap_t* bitmap) {
  return mi_atomic_load_relaxed(&bitmap->chunk_count);
}

static inline size_t mi_bitmap_max_bits(const mi_bitmap_t* bitmap) {
  return (mi_bitmap_chunk_count(bitmap) * MI_BCHUNK_BITS);
}



/* --------------------------------------------------------------------------------
  Atomic bitmap operations
-------------------------------------------------------------------------------- */

// Many operations are generic over setting or clearing the bit sequence: we use `mi_xset_t` for this (true if setting, false if clearing)
typedef bool  mi_xset_t;
#define MI_BIT_SET    (true)
#define MI_BIT_CLEAR  (false)


// Required size of a bitmap to represent `bit_count` bits.
size_t mi_bitmap_size(size_t bit_count, size_t* chunk_count);

// Initialize a bitmap to all clear; avoid a mem_zero if `already_zero` is true
// returns the size of the bitmap.
size_t mi_bitmap_init(mi_bitmap_t* bitmap, size_t bit_count, bool already_zero);

// Set/clear a sequence of `n` bits in the bitmap (and can cross chunks). Not atomic so only use if local to a thread.
void mi_bitmap_unsafe_setN(mi_bitmap_t* bitmap, size_t idx, size_t n);


// Set/clear a bit in the bitmap; returns `true` if atomically transitioned from 0 to 1 (or 1 to 0)
bool mi_bitmap_xset(mi_xset_t set, mi_bitmap_t* bitmap, size_t idx);

static inline bool mi_bitmap_set(mi_bitmap_t* bitmap, size_t idx) {
  return mi_bitmap_xset(MI_BIT_SET, bitmap, idx);
}

static inline bool mi_bitmap_clear(mi_bitmap_t* bitmap, size_t idx) {
  return mi_bitmap_xset(MI_BIT_CLEAR, bitmap, idx);
}


// Set/clear a sequence of `n` bits in the bitmap; returns `true` if atomically transitioned from all 0's to 1's (or all 1's to 0's).
// `n` cannot cross chunk boundaries (and `n <= MI_BCHUNK_BITS`)!
// If `already_xset` is not NULL, it is to all the bits were already all set/cleared.
bool mi_bitmap_xsetN(mi_xset_t set, mi_bitmap_t* bitmap, size_t idx, size_t n, size_t* already_xset);

static inline bool mi_bitmap_setN(mi_bitmap_t* bitmap, size_t idx, size_t n, size_t* already_set) {
  return mi_bitmap_xsetN(MI_BIT_SET, bitmap, idx, n, already_set);
}

static inline bool mi_bitmap_clearN(mi_bitmap_t* bitmap, size_t idx, size_t n) {
  return mi_bitmap_xsetN(MI_BIT_CLEAR, bitmap, idx, n, NULL);
}


// Is a sequence of n bits already all set/cleared?
bool mi_bitmap_is_xsetN(mi_xset_t set, mi_bitmap_t* bitmap, size_t idx, size_t n);

static inline bool mi_bitmap_is_setN(mi_bitmap_t* bitmap, size_t idx, size_t n) {
  return mi_bitmap_is_xsetN(MI_BIT_SET, bitmap, idx, n);
}

static inline bool mi_bitmap_is_clearN(mi_bitmap_t* bitmap, size_t idx, size_t n) {
  return mi_bitmap_is_xsetN(MI_BIT_CLEAR, bitmap, idx, n);
}


// Try to set/clear a sequence of `n` bits in the bitmap; returns `true` if atomically transitioned from 0's to 1's (or 1's to 0's)
// and false otherwise leaving the bitmask as is.
// `n` cannot cross chunk boundaries (and `n <= MI_BCHUNK_BITS`)!
mi_decl_nodiscard bool mi_bitmap_try_xsetN(mi_xset_t set, mi_bitmap_t* bitmap, size_t idx, size_t n);

static inline bool mi_bitmap_try_setN(mi_bitmap_t* bitmap, size_t idx, size_t n) {
  return mi_bitmap_try_xsetN(MI_BIT_SET, bitmap, idx, n);
}

static inline bool mi_bitmap_try_clearN(mi_bitmap_t* bitmap, size_t idx, size_t n) {
  return mi_bitmap_try_xsetN(MI_BIT_CLEAR, bitmap, idx, n);
}

// Find a sequence of `n` bits in the bitmap with all bits set, and atomically unset all.
// Returns true on success, and in that case sets the index: `0 <= *pidx <= MI_BITMAP_MAX_BITS-n`.
mi_decl_nodiscard bool mi_bitmap_try_find_and_clearN(mi_bitmap_t* bitmap, size_t n, size_t tseq, size_t* pidx);

typedef bool (mi_claim_fun_t)(size_t slice_index, void* arg1, void* arg2, bool* keep_set);

mi_decl_nodiscard bool mi_bitmap_try_find_and_claim(mi_bitmap_t* bitmap, size_t tseq, size_t* pidx, 
                                                    mi_claim_fun_t* claim, void* arg1, void* arg2);

void mi_bitmap_clear_once_set(mi_bitmap_t* bitmap, size_t idx);

#endif // MI_BITMAP_H
