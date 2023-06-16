/* jdupes file hashing function
 * This file is part of jdupes; see jdupes.c for license information */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>

#include <libjodycode.h>

#include "likely_unlikely.h"
#include "interrupt.h"
#include "progress.h"
#include "jdupes.h"

/* Hash part or all of a file
 *
 *              READ THIS BEFORE CHANGING THE HASH FUNCTION!
 * The hash function is only used to do fast exclusion. There is not much
 * benefit to using bigger or "better" hash functions. Upstream jdupes WILL
 * NOT accept any pull requests that change the hash function unless there
 * is an EXTREMELY compelling reason to do so. Do not waste your time with
 * swapping hash functions. If you want to do it for fun then that's fine. */
jdupes_hash_t *get_filehash(const file_t * const restrict checkfile, const size_t max_read)
{
  off_t fsize;
  /* This is an array because we return a pointer to it */
  static jdupes_hash_t hash[1];
  static jdupes_hash_t *chunk = NULL;
  FILE *file;
#ifndef USE_JODY_HASH
  XXH64_state_t *xxhstate;
#else
#endif

  if (unlikely(checkfile == NULL || checkfile->d_name == NULL)) jc_nullptr("get_filehash()");
  LOUD(fprintf(stderr, "get_filehash('%s', %" PRIdMAX ")\n", checkfile->d_name, (intmax_t)max_read);)

  /* Allocate on first use */
  if (unlikely(chunk == NULL)) {
    chunk = (jdupes_hash_t *)malloc(auto_chunk_size);
    if (unlikely(!chunk)) jc_oom("get_filehash() chunk");
  }

  /* Get the file size. If we can't read it, bail out early */
  if (unlikely(checkfile->size == -1)) {
    LOUD(fprintf(stderr, "get_filehash: not hashing because stat() info is bad\n"));
    return NULL;
  }
  fsize = checkfile->size;

  /* Do not read more than the requested number of bytes */
  if (max_read > 0 && fsize > (off_t)max_read)
    fsize = (off_t)max_read;

  /* Initialize the hash and file read parameters (with filehash_partial skipped)
   *
   * If we already hashed the first chunk of this file, we don't want to
   * wastefully read and hash it again, so skip the first chunk and use
   * the computed hash for that chunk as our starting point.
   */

  *hash = 0;
  if (ISFLAG(checkfile->flags, FF_HASH_PARTIAL)) {
    *hash = checkfile->filehash_partial;
    /* Don't bother going further if max_read is already fulfilled */
    if (max_read != 0 && max_read <= PARTIAL_HASH_SIZE) {
      LOUD(fprintf(stderr, "Partial hash size (%d) >= max_read (%" PRIuMAX "), not hashing anymore\n", PARTIAL_HASH_SIZE, (uintmax_t)max_read);)
      return hash;
    }
  }
  errno = 0;
#ifdef UNICODE
  if (!M2W(checkfile->d_name, wstr)) file = NULL;
  else file = _wfopen(wstr, FILE_MODE_RO);
#else
  file = fopen(checkfile->d_name, FILE_MODE_RO);
#endif
  if (file == NULL) {
    fprintf(stderr, "\n%s error opening file ", strerror(errno)); jc_fwprint(stderr, checkfile->d_name, 1);
    return NULL;
  }
  /* Actually seek past the first chunk if applicable
   * This is part of the filehash_partial skip optimization */
  if (ISFLAG(checkfile->flags, FF_HASH_PARTIAL)) {
    if (fseeko(file, PARTIAL_HASH_SIZE, SEEK_SET) == -1) {
      fclose(file);
      fprintf(stderr, "\nerror seeking in file "); jc_fwprint(stderr, checkfile->d_name, 1);
      return NULL;
    }
    fsize -= PARTIAL_HASH_SIZE;
  }

/* WARNING: READ NOTICE ABOVE get_filehash() BEFORE CHANGING HASH FUNCTIONS! */
#ifndef USE_JODY_HASH
  xxhstate = XXH64_createState();
  if (unlikely(xxhstate == NULL)) jc_nullptr("xxhstate");
  XXH64_reset(xxhstate, 0);
#endif

  /* Read the file in chunks until we've read it all. */
  while (fsize > 0) {
    size_t bytes_to_read;

    if (interrupt) return 0;
    bytes_to_read = (fsize >= (off_t)auto_chunk_size) ? auto_chunk_size : (size_t)fsize;
    if (unlikely(fread((void *)chunk, bytes_to_read, 1, file) != 1)) goto error_reading_file;

#ifndef USE_JODY_HASH
    XXH64_update(xxhstate, chunk, bytes_to_read);
#else
    if (unlikely(jc_block_hash(chunk, hash, bytes_to_read) != 0)) goto error_reading_file;
#endif

    if ((off_t)bytes_to_read > fsize) break;
    else fsize -= (off_t)bytes_to_read;

    check_sigusr1();
    if (jc_alarm_ring != 0) {
      jc_alarm_ring = 0;
      update_phase2_progress("hashing", (int)(((checkfile->size - fsize) * 100) / checkfile->size));
    }
    continue;
error_reading_file:
    fprintf(stderr, "\nerror reading from file "); jc_fwprint(stderr, checkfile->d_name, 1);
    fclose(file);
    return NULL;
  }

  fclose(file);

#ifndef USE_JODY_HASH
  *hash = XXH64_digest(xxhstate);
  XXH64_freeState(xxhstate);
#endif

  LOUD(fprintf(stderr, "get_filehash: returning hash: 0x%016jx\n", (uintmax_t)*hash));
  return hash;
}
