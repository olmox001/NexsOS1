/*
 * kernel/include/kernel/sha256.h
 * SHA-256 (FIPS 180-4) — the kernel half of the image-integrity primitive.
 *
 * THE SAME ALGORITHM AS tools/mkdisk.c.  A digest only means something if both
 * sides compute it identically, so these must never become two transcriptions
 * of the spec that drifted; they are kept adjacent in review, and any change to
 * one is a change to both.
 *
 * Used as the leaf primitive of the Merkle tree over a partition (design doc
 * §4bis, D13): one leaf per 4 KiB block, leaves hashed pairwise up to a root
 * that lives in the META partition.  Verification then costs a full read at
 * boot, while a write costs only log2(n) rehashes — which is what lets "verify
 * ROOT at every boot" survive a writable filesystem.
 *
 * ASTRA: a library primitive under kernel/lib, consumed by the block/partition
 * layer.  Filesystems and drivers do not open-code hashing.
 */
#ifndef _KERNEL_SHA256_H
#define _KERNEL_SHA256_H

#include <kernel/types.h>

#define SHA256_DIGEST_SIZE 32

struct sha256_ctx {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  size_t buflen;
};

void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, size_t n);
void sha256_final(struct sha256_ctx *c, uint8_t out[SHA256_DIGEST_SIZE]);

/* sha256_buf - one-shot convenience over the three calls above. */
void sha256_buf(const void *data, size_t n, uint8_t out[SHA256_DIGEST_SIZE]);

/*
 * sha256_merkle_pair - parent = SHA256(left || right).
 *
 * A lone node at an odd level is PROMOTED unchanged by the caller, never
 * paired with a copy of itself: self-pairing is the classic Merkle malleability
 * bug (CVE-2012-2459), where two different block sequences yield the same root.
 */
void sha256_merkle_pair(const uint8_t left[SHA256_DIGEST_SIZE],
                        const uint8_t right[SHA256_DIGEST_SIZE],
                        uint8_t out[SHA256_DIGEST_SIZE]);

#endif /* _KERNEL_SHA256_H */
