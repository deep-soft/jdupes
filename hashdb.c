/* File hash database management
 * This file is part of jdupes; see jdupes.c for license information */

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "jdupes.h"
#include "libjodycode.h"
#include "likely_unlikely.h"
#include "hashdb.h"

#define HASHDB_VER 1
#define HASHDB_MIN_VER 1
#define HASHDB_MAX_VER 1
#ifndef PH_SHIFT
 #define PH_SHIFT 12
#endif
#define SECS_TO_TIME(a,b) strftime(a, 32, "%F %T", localtime(b));

#ifndef HT_SIZE
// #define HT_SIZE 256
// #define HT_SIZE 4096
// #define HT_SIZE 65536
 #define HT_SIZE 131072
#endif
#define HT_MASK (HT_SIZE - 1)
//#define HT_MASK 0xffLLU

static hashdb_t *hashdb[HT_SIZE];
static int hashdb_init = 0;
static int hashdb_algo = 0;
static int hashdb_dirty = 0;

/* Pivot direction for rebalance */
enum pivot { PIVOT_LEFT, PIVOT_RIGHT };


void hd16(char *a) {
  int i;

  printf("DUMP Hex: ");
  for (i = 0; i < 16; i++) printf("%x", a[i]);
  printf("\nDUMP ASCII: ");
  for (i = 0; i < 16; i++) printf("%c", a[i]);
  printf("\n");
  return;
}


static hashdb_t *alloc_hashdb_node(const int pathlen)
{
  int allocsize;

//  allocsize = sizeof(hashdb_t) + pathlen + 1;
  allocsize = sizeof(hashdb_t) + pathlen + 1;
  if ((allocsize & 0x0fLU) != 0) allocsize += 16 - (allocsize & 0xf);
  return (hashdb_t *)calloc(1, allocsize);
}


int save_hash_database(const char * const restrict dbname)
{
  FILE *db;
  unsigned long cnt;

  if (dbname == NULL) goto error_hashdb_null;
  LOUD(fprintf(stderr, "save_hash_database('%s')\n", dbname);)
  /* Don't save the hash database if it wasn't changed */
  if (hashdb_dirty == 0) return 0;
  errno = 0;
  db = fopen(dbname, "w+b");
  if (db == NULL) goto error_hashdb_open;

  if (write_hashdb_entry(db, NULL, &cnt) != 0) goto error_hashdb_write;
  fclose(db);
//fprintf(stderr, "Wrote %lu items to hash databse\n", cnt);

  return 0;

error_hashdb_null:
  fprintf(stderr, "error: internal failure: NULL pointer for hashdb\n");
  return 1;
error_hashdb_open:
  fprintf(stderr, "error: cannot open hashdb '%s' for writing: %s\n", dbname, strerror(errno));
  return 2;
error_hashdb_write:
  fprintf(stderr, "error: writing failed to hashdb '%s': %s\n", dbname, strerror(errno));
  fclose(db);
  return 2;
}


int write_hashdb_entry(FILE *db, hashdb_t *cur, unsigned long *cnt)
{
  struct timeval tm;
  int err = 0;
  static char out[PATH_MAX + 128];

//fprintf(stderr, "write_hashdb_entry(%p, %lu)\n", (void *)cur, *cnt);
  /* Write header and traverse array on first call */
  if (unlikely(cur == NULL)) {
    gettimeofday(&tm, NULL);
    snprintf(out, PATH_MAX + 127, "jdupes hashdb:%d,%d,%08lx\n", HASHDB_VER, hash_algo, tm.tv_sec);
    LOUD(fprintf(stderr, "write hashdb: %s", out);)
    errno = 0;
    fputs(out, db);
    if (errno != 0) return 1;
    /* Write out each hash bucket, skipping empty buckets */
    for (int i = 0; i < HT_SIZE; i++) {
//if (hashdb[i] == NULL) fprintf(stderr, "hashdb[%d] null\n", i);
      if (hashdb[i] == NULL) continue;
      err = write_hashdb_entry(db, hashdb[i], cnt);
      if (err != 0) return err;
    }
//fprintf(stderr, "exiting write_hashdb_entry loop\n");
    return 0;
  }

  /* Write out this node if it wasn't invalidated */
  if (cur->hashcount != 0) {
#ifdef ON_WINDOWS
    snprintf(out, PATH_MAX + 127, "%u,%016llx,%016llx,%08llx,%08llx,%016llx,%s\n",
      cur->hashcount, cur->partialhash, cur->fullhash, cur->mtime, cur->size, cur->inode, cur->path);
#else
    snprintf(out, PATH_MAX + 127, "%u,%016lx,%016lx,%08lx,%08lx,%016lx,%s\n",
      cur->hashcount, cur->partialhash, cur->fullhash, cur->mtime, cur->size, cur->inode, cur->path);
#endif
    (*cnt)++;
    LOUD(fprintf(stderr, "write hashdb: %s", out);)
    errno = 0;
    fputs(out, db);
    if (errno != 0) return 1;
  }

  /* Traverse the tree, propagating errors */
  if (cur->left != NULL) err = write_hashdb_entry(db, cur->left, cnt);
  if (err == 0 && cur->right != NULL) err = write_hashdb_entry(db, cur->right, cnt);
  return err;
}


void dump_hashdb(hashdb_t *cur)
{
  struct timeval tm;

//fprintf(stderr, "dump_hashdb(%p)\n", (void *)cur);
  if (cur == NULL) {
    gettimeofday(&tm, NULL);
    printf("jdupes hashdb:1,%d,%08lx\n", hash_algo, tm.tv_sec);
    for (int i = 0; i < HT_SIZE; i++) {
//fprintf(stderr, "bucket %d = %p\n", i, (void *)hashdb[i]);
      if (hashdb[i] == NULL) continue;
      dump_hashdb(hashdb[i]);
    }
    return;
  }
  /* db line format: hashcount,partial,full,mtime,path */
#ifdef ON_WINDOWS
  if (cur->hashcount != 0) printf("%u,%016llx,%016llx,%08llx,%08llx,%016llx,%s\n",
      cur->hashcount, cur->partialhash, cur->fullhash, cur->mtime, cur->size, cur->inode, cur->path);
#else
  if (cur->hashcount != 0) printf("%u,%016lx,%016lx,%08lx,%08lx,%016lx,%s\n",
      cur->hashcount, cur->partialhash, cur->fullhash, cur->mtime, cur->size, cur->inode, cur->path);
#endif
  if (cur->left != NULL) dump_hashdb(cur->left);
  if (cur->right != NULL) dump_hashdb(cur->right);
  return;
}


static void pivot_hashdb_tree(hashdb_t **parent, hashdb_t *cur, enum pivot direction)
{
  hashdb_t *temp;

//fprintf(stderr, "pivoting one %s\n", direction == PIVOT_LEFT ? "left" : "right");
  if (direction == PIVOT_LEFT) {
    temp = cur->right;
    cur->right = cur->right->left;
    temp->left = cur;
    *parent = temp;
  } else {  // PIVOT_RIGHT
    temp = cur->left;
    cur->left = cur->left->right;
    temp->right = cur;
    *parent = temp;
  }

  return;
}


static void rebalance_hashdb_tree(hashdb_t **parent)
{
  const uint64_t center = 0x8000000000000000ULL;
  hashdb_t *cur = *parent;

  if (unlikely(cur == NULL || parent == NULL)) return;
  if (cur->left == NULL && cur->right == NULL) return;

  if (cur->left != NULL) rebalance_hashdb_tree(&(cur->left));
  if (cur->right != NULL) rebalance_hashdb_tree(&(cur->right));
  if (cur->path_hash > center) {
    /* This node might be better off to the right */
    if (cur->left != NULL && cur->left->path_hash > center) pivot_hashdb_tree(parent, cur, PIVOT_RIGHT);
  } else if (cur->path_hash < center) {
    /* This node might be better off to the left */
    if (cur->right != NULL && cur->right->path_hash < center) pivot_hashdb_tree(parent, cur, PIVOT_LEFT);
  }
  return;
}


hashdb_t *add_hashdb_entry(const uint64_t path_hash, int pathlen, file_t *check)
{
  const unsigned int bucket = path_hash & HT_MASK;
  hashdb_t *file;
  hashdb_t *cur;
  int exclude;
  static int ldepth = 0, rdepth = 0, difference;
  static uint64_t rebal = 0;

  /* Allocate hashdb on first use */
  if (unlikely(hashdb_init == 0)) {
    memset(hashdb, 0, sizeof(hashdb_t *) * HT_SIZE);
    hashdb_init = 1;
  }

  if (check != NULL && check->d_name == NULL) return NULL;
//fprintf(stderr, "add_hashdb_entry(%016lx, %d, %p)\n", path_hash, pathlen, (void *)check);

  if (hashdb[bucket] == NULL) {
//fprintf(stderr, "root hash %016lx\n", path_hash);
    file = alloc_hashdb_node(pathlen);
    if (file == NULL) return NULL;
    hashdb[bucket] = file;
//fprintf(stderr, "file is now the head of bucket %u\n", bucket);
  } else {
    cur = hashdb[bucket];
    while (1) {
//fprintf(stderr, "%016lx >= %016lx ?\n", cur->path_hash, path_hash);
      /* If path is set then this entry may already exist and we need to check */
      if (check != NULL && cur->path != NULL) {
        if (cur->path_hash == path_hash && strcmp(cur->path, check->d_name) == 0) {
//fprintf(stderr, "file already exists: '%s'\n", cur->path);
          /* Invalidate this entry if something has changed */
          exclude = 0;
          if (cur->mtime != check->mtime) exclude |= 1;
          if (cur->inode != check->inode) exclude |= 2;
          if (cur->size  != check->size)  exclude |= 4;
          if (exclude == 0) {
            if (cur->hashcount == 1 && ISFLAG(check->flags, FF_HASH_FULL)) {
              cur->hashcount = 2;
              cur->fullhash = check->filehash;
              hashdb_dirty = 1;
//fprintf(stderr, "Added full hash to file %s\n", cur->path);
            }
            return cur;
          } else {
//fprintf(stderr, "invalidating file: '%s'\n", cur->path);
            cur->hashcount = 0;
            hashdb_dirty = 1;
            return NULL;
          }
        }
      }
//fprintf(stderr, "path hashes: %016lx %016lx\n", cur->path_hash, path_hash);
      if (cur->path_hash >= path_hash) {
        if (cur->left == NULL) {
          file = alloc_hashdb_node(pathlen);
          if (file == NULL) return NULL;
          cur->left = file;
          break;
        } else {
          cur = cur->left;
          ldepth++;
          continue;
        }
      } else {
        if (cur->right == NULL) {
          file = alloc_hashdb_node(pathlen);
          if (file == NULL) return NULL;
          cur->right = file;
          break;
        } else {
          cur = cur->right;
          rdepth++;
          continue;
        }
      }
    }
//fprintf(stderr, "ldepth %d, rdepth %d\n", ldepth, rdepth);
    difference = ldepth - rdepth;
    if (difference < 0) difference = -difference;
//fprintf(stderr, "difference %d > %d ?\n", difference, (rdepth + ldepth) / 2);
    if (difference > 100000) {
      rebal++;
//fprintf(stderr, "rebalance %lu, bucket %u\n", rebal, bucket);
      rebalance_hashdb_tree(&(hashdb[bucket]));
    }
  }

  /* If a check entry was given then populate it */
  if (check != NULL && check->d_name != NULL && ISFLAG(check->flags, FF_HASH_PARTIAL)) {
    hashdb_dirty = 1;
    file->path_hash = path_hash;
//fprintf(stderr, "path_hash: %08x\n", path_hash);
    file->path = (char *)((uintptr_t)file + (uintptr_t)sizeof(hashdb_t));
//    strncpy(file->path, check->d_name, pathlen);
    memcpy(file->path, check->d_name, pathlen + 1);
//fprintf(stderr, "file->path %s\n", file->path);
    *(file->path + pathlen) = '\0';
    file->size = check->size;
    file->inode = check->inode;
    file->mtime = check->mtime;
    file->partialhash = check->filehash_partial;
    file->fullhash = check->filehash;
    if (ISFLAG(check->flags, FF_HASH_FULL)) file->hashcount = 2;
    else file->hashcount = 1;
  } else {
    /* No check entry? Populate from passed parameters */
    file->path = (char *)((uintptr_t)file + (uintptr_t)sizeof(hashdb_t));
//fprintf(stderr, "path %p\n", (void *)file->path);
    file->path_hash = path_hash;
//fprintf(stderr, "path_hash: %08x\n", path_hash);
  }
  return file;
}

/* db header format: jdupes hashdb:dbversion,hashtype,update_mtime
 * db line format: hashcount,partial,full,mtime,size,inode,path */
#define FIXED_LINELEN 71
int load_hash_database(char *dbname)
{
  FILE *db;
  char buf[PATH_MAX + 128];
  char *field, *temp;
  int db_ver;
  int linenum = 1;
#ifdef LOUD_DEBUG
  time_t db_mtime;
  char date[32];
#endif /* LOUD_DEBUG */
  
  if (dbname == NULL) goto error_hashdb_null;
  LOUD(fprintf(stderr, "load_hash_database('%s')\n", dbname);)
  errno = 0;
  db = fopen(dbname, "rb");
  if (db == NULL) goto warn_hashdb_open;

  /* Read header line */
  if ((fgets(buf, PATH_MAX + 127, db) == NULL) || (ferror(db) != 0)) goto error_hashdb_read;
//fprintf(stderr, "read hashdb: %s", buf);
  field = strtok(buf, ":");
  if (strcmp(field, "jdupes hashdb") != 0) goto error_hashdb_header;
  field = strtok(NULL, ":");
  temp = strtok(field, ",");
  db_ver = (int)strtoul(temp, NULL, 10);
  temp = strtok(NULL, ",");
  hashdb_algo = (int)strtoul(temp, NULL, 10);
  temp = strtok(NULL, ",");
  /* Database mod time is currently set but not used */
  LOUD(db_mtime = (int)strtoul(temp, NULL, 16);)
  LOUD(SECS_TO_TIME(date, &db_mtime);)
  LOUD(fprintf(stderr, "hashdb header: ver %u, algo %u, mod %s\n", db_ver, hashdb_algo, date);)
  if (db_ver < HASHDB_MIN_VER || db_ver > HASHDB_MAX_VER) goto error_hashdb_version;
  if (hashdb_algo != hash_algo) goto warn_hashdb_algo;

  /* Read database entries */
  while (1) {
    int pathlen;
    int linelen;
    int hashcount;
    uint64_t partialhash, fullhash = 0;
    uint64_t path_hash;
    time_t mtime;
    char *path;
    hashdb_t *entry;
    off_t size;
    jdupes_ino_t inode;

    errno = 0;
    if ((fgets(buf, PATH_MAX + 127, db) == NULL)) {
      if (ferror(db) != 0 || errno != 0) goto error_hashdb_read;
      break;
    }
//    LOUD(fprintf(stderr, "read hashdb: %s", buf);)
//fprintf(stderr, "read hashdb: %s", buf);
    linenum++;
    linelen = (int)strlen(buf);
    if (linelen < FIXED_LINELEN + 1) goto error_hashdb_line;

    /* Split each entry into fields and
     * hashcount: 1 = partial only, 2 = partial and full */
    field = strtok(buf, ","); if (field == NULL) goto error_hashdb_line;
    hashcount = (int)strtol(field, NULL, 16);
    if (hashcount < 1 || hashcount > 2) goto error_hashdb_line;
    field = strtok(NULL, ","); if (field == NULL) goto error_hashdb_line;
    partialhash = strtoull(field, NULL, 16);
    field = strtok(NULL, ","); if (field == NULL) goto error_hashdb_line;
    if (hashcount == 2) fullhash = strtoull(field, NULL, 16);
    field = strtok(NULL, ","); if (field == NULL) goto error_hashdb_line;
    mtime = (time_t)strtoul(field, NULL, 16);
    field = strtok(NULL, ","); if (field == NULL) goto error_hashdb_line;
    size = strtoul(field, NULL, 16);
    field = strtok(NULL, ","); if (field == NULL) goto error_hashdb_line;
    inode = strtoull(field, NULL, 16);

    path = buf + FIXED_LINELEN;
    path = strtok(path, "\n"); if (path == NULL) goto error_hashdb_line;
    pathlen = linelen - FIXED_LINELEN + 1;
    if (pathlen > PATH_MAX) goto error_hashdb_line;
    *(path + pathlen) = '\0';
    if (get_path_hash(path, &path_hash) != 0) goto error_hashdb_path_hash;

//SECS_TO_TIME(date, &mtime);
//fprintf(stderr, "file entry: [%u:%016lx] '%s', mtime %s, size %ld, inode %lu, hashes [%u] %016lx:%016lx\n", pathlen, path_hash, path, date, size, inode, hashcount, partialhash, fullhash);

    entry = add_hashdb_entry(path_hash, pathlen, NULL);
    if (entry == NULL) goto error_hashdb_add;
    // init path entry items
    entry->path_hash = path_hash;
    memcpy(entry->path, path, pathlen + 1);
    entry->mtime = mtime;
    entry->inode = inode;
    entry->size = size;
    entry->partialhash = partialhash;
    entry->fullhash = fullhash;
    entry->hashcount = hashcount;
  }

  return 0;

warn_hashdb_open:
  fprintf(stderr, "warning: creating a new hash database '%s'\n", dbname);
  return 1;
error_hashdb_read:
  fprintf(stderr, "error reading hash database '%s': %s\n", dbname, strerror(errno));
  return 2;
error_hashdb_header:
  fprintf(stderr, "error in header of hash database '%s'\n", dbname);
  return 3;
error_hashdb_version:
  fprintf(stderr, "error: bad db version %u in hash database '%s'\n", db_ver, dbname);
  return 4;
error_hashdb_line:
  fprintf(stderr, "error: bad line %u in hash database '%s': '%s'\n", linenum, dbname, buf);
  return 5;
error_hashdb_add:
  fprintf(stderr, "error: internal failure allocating a hashdb entry\n");
  return 6;
error_hashdb_path_hash:
  fprintf(stderr, "error: internal failure hashing a path\n");
  return 7;
error_hashdb_null:
  fprintf(stderr, "error: internal failure: NULL pointer for hashdb\n");
  return 8;
warn_hashdb_algo:
  fprintf(stderr, "warning: hashdb uses a different hash algorithm than selected; not loading\n");
  return 9;
}
 

int get_path_hash(char *path, uint64_t *path_hash)
{
  uint64_t aligned_path[(PATH_MAX + 8) / sizeof(uint64_t)];
  int retval;

  *path_hash = 0;
  if ((uintptr_t)path & 0x0f) {
    strncpy((char *)&aligned_path, path, PATH_MAX);
    retval = jc_block_hash((uint64_t *)aligned_path, path_hash, strlen((char *)aligned_path));
  } else retval = jc_block_hash((uint64_t *)path, path_hash, strlen(path));
  return retval;
}


 /* If file hash info is already present in hash database then preload those hashes */
int read_hashdb_entry(file_t *file)
{
  unsigned int bucket;
  hashdb_t *cur;
  uint64_t path_hash;
  int exclude;

  LOUD(fprintf(stderr, "read_hashdb_entry('%s')\n", file->d_name);)
  if (file == NULL || file->d_name == NULL) goto error_null;
  if (get_path_hash(file->d_name, &path_hash) != 0) goto error_path_hash;
  bucket = path_hash & HT_MASK;
//fprintf(stderr, "sizeof %lu, sizeof _t %lu, hashdb %p, hashdb+bucket %p, bucket %u => %lu\n", sizeof(hashdb), sizeof(hashdb_t), (void *)hashdb, (void *)(hashdb + bucket), bucket, (uintptr_t)((hashdb + bucket) - hashdb));
//fprintf(stderr, "hashdb[bucket] %p\n", (void *)hashdb[bucket]);
  if (hashdb[bucket] == NULL) return 0;
  cur = hashdb[bucket];
  while (1) {
    if (cur->path_hash != path_hash) {
      if (path_hash < cur->path_hash) cur = cur->left;
      else cur = cur->right;
      if (cur == NULL) return 0;
      continue;
    }
    /* Found a matching path hash */
    if (strcmp(cur->path, file->d_name) != 0) {
      cur = cur->left;
      if (cur == NULL) return 0;
      continue;
    } else {
      /* Found a matching path too but check mtime */
      exclude = 0;
      if (cur->mtime != file->mtime) exclude |= 1;
      if (cur->inode != file->inode) exclude |= 2;
      if (cur->size  != file->size)  exclude |= 4;
      if (exclude != 0) {
        /* Invalidate if something has changed */
//fprintf(stderr, "invalidating file: '%s' [%d]\n", cur->path, exclude);
        cur->hashcount = 0;
        hashdb_dirty = 1;
        return -1;
      }
      file->filehash_partial = cur->partialhash;
      if (cur->hashcount == 2) {
        file->filehash = cur->fullhash;
        SETFLAG(file->flags, (FF_HASH_PARTIAL | FF_HASH_FULL));
      } else SETFLAG(file->flags, FF_HASH_PARTIAL);
      return 1;
    }
  }
  return 0;

error_null:
  fprintf(stderr, "error: internal error: NULL data passed to read_hashdb_entry()\n");
  return -255;
error_path_hash:
  fprintf(stderr, "error: internal error hashing a path\n");
  return -255;
}


#ifdef HASHDB_TESTING
/* For testing purposes only */
int hash_algo = 0;
int main(void) {
  file_t file;
  uint64_t path_hash;

  memset(&file, 0, sizeof(file_t));
  file.d_name = (char *)malloc(128);

  fprintf(stderr, "load_hash_database returned %d\n", load_hash_database("test_hashdb.txt"));

  strcpy(file.d_name, "THREE Turntables!@#"); file.mtime = 0x64e37acd;
  read_hashdb_entry(&file);
  printf("File info: name '%s', flags %x, hashes %016lx:%016lx\n", file.d_name, file.flags, file.filehash_partial, file.filehash);
  if (get_path_hash(file.d_name, &path_hash) != 0) return 1;
  add_hashdb_entry(path_hash, strlen(file.d_name), &file);

  file.filehash_partial = 0; file.filehash = 0; file.flags = 0; *(file.d_name) = '\0';
  strcpy(file.d_name, "BAR"); file.inode = 321; file.size = 11000;
  read_hashdb_entry(&file);
  printf("File info: name '%s', flags %x, hashes %016lx:%016lx\n", file.d_name, file.flags, file.filehash_partial, file.filehash);

  file.filehash_partial = 0; file.filehash = 0; file.flags = 0; *(file.d_name) = '\0';
  strcpy(file.d_name, "Two Turntables!@#"); file.inode = 1111; file.size = 12312414;
  read_hashdb_entry(&file);
  printf("File info: name '%s', flags %x, hashes %016lx:%016lx\n", file.d_name, file.flags, file.filehash_partial, file.filehash);

  file.filehash_partial = 0; file.filehash = 0; file.flags = 0; *(file.d_name) = '\0';
  strcpy(file.d_name, "XyzZ");
  read_hashdb_entry(&file);
  printf("File info: name '%s', flags %x, hashes %016lx:%016lx\n", file.d_name, file.flags, file.filehash_partial, file.filehash);

  file.filehash_partial = 0; file.filehash = 0; file.flags = 0; *(file.d_name) = '\0';
  strcpy(file.d_name, "NOT IN THE DATABASE.");
  read_hashdb_entry(&file);
  printf("File info: name '%s', flags %x, hashes %016lx:%016lx\n", file.d_name, file.flags, file.filehash_partial, file.filehash);

  file.filehash_partial = 1; file.filehash = 2; file.flags = 6; file.mtime = 0x6e412345; strcpy(file.d_name, "File to add to DB");
  if (get_path_hash(file.d_name, &path_hash) != 0) return 1;
  add_hashdb_entry(path_hash, strlen(file.d_name), &file);

  dump_hashdb(NULL);

  return 0;
}
#endif /* HASHDB_TESTING */
