/*
 * cache-membuffer.c: in-memory caching for Subversion
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <assert.h>
#include <apr_md5.h>
#include "svn_pools.h"
#include "svn_checksum.h"
#include "md5.h"
#include "svn_private_config.h"
#include "cache.h"
#include "svn_string.h"

/*
 * A membuffer cache consists of two parts:
 *
 * 1. A linear data buffer containing a cached items in a serialized
 *    representation. There may be arbitrary gaps between entries.
 * 2. A directory of cache entries. This is organized similar to CPU
 *    data caches: for every possible key, there is exactly one group
 *    of entries that may contain the header info for an item with
 *    that given key. The result is a GROUP_SIZE-way associative cache.
 *
 * Only the begnnings of these two data parts are addressed through a
 * native pointer. All other references are expressed as offsets to
 * these array pointers. With that design, it is relatively easy to
 * share the same data structure between different processes and / or
 * to persist them on disk.
 *
 * The data buffer usage information is implicitly given by the directory
 * entries. Every *used* entry has a reference to the previous and the
 * next used dictionary entry - in the order their item data is stored
 * in the data buffer. So, removing data, for instance, is done simply
 * by unlinking it from the chain, marking it as unused and possibly
 * adjusting global list pointers.
 *
 * Insertion can occur at one position. It is marked by its offset in
 * the data buffer plus the index of the first used entry equal or
 * larger that position. If this gap is too small to accomodate the
 * new item, the insertion window is extended as described below. The
 * new entry will always be inserted at the bottom end of the window
 * and since the next used entry is known, properly sorted insertion
 * is possible.
 *
 * To make the cache perform robustly in a wide range of usage scenarios,
 * a randomized variant of LFU is used. Every item holds a (read)
 * hit counter and there is a global (read) hit counter. The more hits
 * an entry has in relation to the average, the more it is likely to be
 * kept using a rand()-based condition. The test is applied only to the
 * entry at the end of the insertion window. If it doesn't get evicted,
 * its being moved to the begin of that window and this window is moved.
 *
 * Moreover, the entry's hits get halfed to make that entry more likely
 * to be removed the next time, the sliding insertion / auto-removal
 * window comes by. As a result, frequently used entries are likely not
 * to be dropped until they get not used for a while. Also, even a cache
 * thrashing situation about 50% of the content survives every 50% of the
 * cache being re-written with new entries.
 *
 * To limit the entry size and management overhead, the actual item keys
 * will not be stored but only their MD5 checksums, instead. This is
 * reasonably safe to do since users have only limited control over the
 * full keys, even if these are folder paths. So, it is very hard to
 * construct colliding keys.
 */

/* A 4-way associative cache seems to be the best compromise between 
 * performance (worst-case lookups) and efficiency-loss due to collisions.
 *
 * This value may be changed to any positive integer.
 */
#define GROUP_SIZE 4

/* We use MD5 for digest size and speed (SHA1 is >2x slower, for instance).
 */
#define KEY_SIZE APR_MD5_DIGESTSIZE

/* For more efficient copy operations, let'a align all data items properly.
 * Must be a power of 2.
 */
#define ITEM_ALIGNMENT 16

/* A single dictionary entry. Since they are allocated statically, these
 * entries can either be in use or in used state. An entry is unused, iff 
 * the offset member is -1. In that case, it must not be linked in the list 
 * of used entries.
 */
typedef struct entry_t
{
  /* Identifying the data item. Only valid for used entries.
   */
  unsigned char key [KEY_SIZE];

  /* If -1, the entry is not in used. Otherwise, it is the offset of the
   * cached item's serialized data within the data buffer.
   */
  apr_uint64_t offset;

  /* Size of the serialized item data. May be 0.
   * Only valid for used entries.
   */
  apr_uint32_t size;

  /* Number of (read) hits for this entry. Will be reset upon write.
   * Only valid for used entries.
   */
  apr_uint32_t hit_count;

  /* Reference to the next used entry in the order defined by offset.
   * -1 indicates the end of the list; this entry must be referenced
   * by the caches membuffer_cache_t.last member. -1 also implies that
   * the data buffer is not used beyond offset+size.
   * Only valid for used entries.
   */
  apr_uint32_t next;

  /* Reference to the previous used entry in the order defined by offset.
   * -1 indicates the end of the list; this entry must be referenced
   * by the caches membuffer_cache_t.first member.
   * Only valid for used entries.
   */
  apr_uint32_t previous;
} entry_t;

/* We group dictionary entries to make this GROUP-SIZE-way assicative.
 */
typedef entry_t entry_group_t[GROUP_SIZE];

/* The cache header structure.
 */
struct membuffer_cache_t
{
  /* The dictionary, GROUP_SIZE * group_count entries long. Never NULL.
   */
  entry_group_t *directory;

  /* Size of dictionary in groups. Must be > 0.
   */
  apr_uint32_t group_count;

  /* Reference to the first (defined by the order content in the data
   * buffer) dictionary entry used by any data item. -1 for an empty cache.
   */
  apr_uint32_t first;

  /* Reference to the last (defined by the order content in the data
   * buffer) dictionary entry used by any data item. -1 for an empty cache.
   */
  apr_uint32_t last;

  /* Reference to the first (defined by the order content in the data
   * buffer) used dictionary entry behind the insertion position 
   * (current_data). If -1, the data buffer is free starting at the
   * current_data offset.
   */
  apr_uint32_t next;


  /* Pointer to the data buffer, data_size bytes long. Never NULL.
   */
  unsigned char *data;

  /* Size of data buffer in bytes. Must be > 0.
   */
  apr_uint64_t data_size;

  /* Offset in the data buffer where the next insertion shall occur.
   */
  apr_uint64_t current_data;

  /* Total number of data buffer bytes in use. This is for statistics only.
   */
  apr_uint64_t data_used;


  /* Number of used dictionary entries, i.e. number of cached items.
   * In conjunction with hit_cout, this is used calculate the average
   * hit count as part of the randomized LFU algorithm.
   */
  apr_uint32_t used_entries;

  /* Sum of (read) hit counts of all used dictionary entries.
   * In conjunction used_entries used_entries, this is used calculate
   * the average hit count as part of the randomized LFU algorithm.
   */
  apr_uint64_t hit_count;


  /* Total number of calls to membuffer_cache_get.
   * Purely statistical information that may be used for profiling.
   */
  apr_uint64_t total_reads;

  /* Total number of calls to membuffer_cache_set.
   * Purely statistical information that may be used for profiling.
   */
  apr_uint64_t total_writes;

  /* Total number of hits since the cache's creation.
   * Purely statistical information that may be used for profiling.
   */
  apr_uint64_t total_hits;

#if APR_HAS_THREADS
  /* A lock for intra-process synchronization to the cache, or NULL if
   * the cache's creator doesn't feel the cache needs to be
   * thread-safe. 
   */
  apr_thread_mutex_t *mutex;
#endif
};

/* Align ADDRESS to the next ITEM_ALIGNMENT boundary.
 */
static APR_INLINE apr_uint64_t 
align_entry(apr_uint64_t address)
{
  return (address + ITEM_ALIGNMENT-1) & -ITEM_ALIGNMENT;
}

/* Acquire the cache mutex, if necessary.
 */
static svn_error_t *
lock_cache(membuffer_cache_t* cache)
{
#if APR_HAS_THREADS
  if (cache->mutex)
  {
    apr_status_t status = apr_thread_mutex_lock(cache->mutex);
    if (status)
      return svn_error_wrap_apr(status, _("Can't lock cache mutex"));
  }
#endif

  return SVN_NO_ERROR;
}

/* Release the cache mutex, if necessary.
 */
static svn_error_t *
unlock_cache(membuffer_cache_t* cache, svn_error_t *err)
{
#if APR_HAS_THREADS
  if (cache->mutex)
  {
    apr_status_t status = apr_thread_mutex_unlock(cache->mutex);
    if (err)
      return err;

    if (status)
      return svn_error_wrap_apr(status, _("Can't unlock cache mutex"));
  }
#endif

  return err;
}

/* Resolve a dictionary entry reference, i.e. return the entry
 * for the given IDX.
 */
static APR_INLINE entry_t*
get_entry(membuffer_cache_t* cache, apr_size_t idx)
{
  return &cache->directory[idx / GROUP_SIZE][idx % GROUP_SIZE];
}

/* Get the entry references for the given ENTRY.
 */
static APR_INLINE apr_uint32_t
get_index(membuffer_cache_t *cache, entry_t *entry)
{
  return entry - (entry_t *)cache->directory;
}

/* Remove the used ENTRY from the CACHE, i.e. make it "unused".
 * In contrast to insertion, removal is possible for any entry.
 */
static void
drop_entry(membuffer_cache_t *cache, entry_t *entry)
{
  apr_uint32_t idx = get_index(cache, entry);

  /* Only valid to be called for used entries.
   */
  assert(entry->offset != -1);

  /* update global cache usage counters
   */
  cache->used_entries--;
  cache->hit_count -= entry->hit_count;
  cache->data_used -= entry->size;

  /* extend the insertion window, if the entry happens to border it
   */
  if (idx == cache->next)
    cache->next = entry->next;
  else 
    if (entry->next == cache->next)
      {
        /* insertion window starts right behind the entry to remove
         */
        if (entry->previous == -1)
          {
            /* remove the first entry -> insertion may start at pos 0, now */
            cache->current_data = 0;
          }
          else
          {
            /* insertion may start right behind the previous entry */
            entry_t *previous = get_entry(cache, entry->previous);
            cache->current_data = align_entry(  previous->offset 
                                              + previous->size);
          }
      }

  /* unlink it from the chain of used entries
   */
  if (entry->previous == -1)
    cache->first = entry->next;
  else
    get_entry(cache, entry->previous)->next = entry->next;

  if (entry->next == -1)
    cache->last = entry->previous;
  else
    get_entry(cache, entry->next)->previous = entry->previous;

  /* Mark the entry as unused.
   */
  entry->offset = -1;
}

/* Insert ENTRY into the chain of used dictionary entries. The entry's
 * offset and size members must already have been initialized. Also,
 * the offset must match the beginning of the insertion window.
 */
static void
insert_entry(membuffer_cache_t *cache, entry_t *entry)
{
  apr_uint32_t idx = get_index(cache, entry);
  entry_t *next = cache->next == -1 ? NULL : get_entry(cache, cache->next);

  /* The entry must start at the beginning of the insertion window.
   */
  assert(entry->offset == cache->current_data);
  cache->current_data = align_entry(entry->offset + entry->size);

  /* update global cache usage counters
   */
  cache->used_entries++;
  cache->data_used += entry->size;
  entry->hit_count = 0;

  /* update entry chain
   */
  entry->next = cache->next;
  if (cache->first == -1)
    {
      /* insert as the first entry and only in the chain
       */
      entry->previous = -1;
      cache->last = idx;
      cache->first = idx;
    }
  else if (next == NULL)
    {
      /* insert as the last entry in the chain.
       * Note that it cannot also be at the beginning of the chain.
       */
      entry->previous = cache->last;
      get_entry(cache, cache->last)->next = idx;
      cache->last = idx;
    }
  else
    {
      /* insert either at the start of a non-empty list or
       * somewhere in the middle 
       */
      entry->previous = next->previous;
      next->previous = idx;

      if (entry->previous != -1)
        get_entry(cache, entry->previous)->next = idx;
      else
        cache->first = idx;
    }
}

/* Map a KEY of length LEN to the group that shall contain the respective 
 * item. Return the hash value in TO_FIND. Returns -1 upon error.
 */
static apr_uint32_t
get_group_index(membuffer_cache_t *cache,
                const void *key,
                apr_size_t len,
                unsigned char *to_find,
                apr_pool_t *pool)
{
  apr_uint32_t hash = 0;
  int i;

  /* calculate a hash value for the key */
  svn_checksum_t *checksum;
  svn_error_t *err;

  err = svn_checksum(&checksum, svn_checksum_md5, key, len, pool);
  if (err != NULL)
  {
    svn_error_clear(err);
    return -1;
  }

  memcpy(to_find, checksum->digest, APR_MD5_DIGESTSIZE);

  /* Get the group that *must* contain the entry. Fold the hash value 
   * just to be sure (it should not be necessary for perfect hashes).
   */
  for (i = 0; i < sizeof(to_find) / sizeof(apr_uint32_t); ++i)
    hash += ((apr_uint32_t*)to_find)[i] ^ ((hash >> 19) || (hash << 13));

  return hash % cache->group_count;
}

/* Given the GROUP_INDEX that shall contain an entry with the hash key
 * TO_FIND, find that entry in the specified group. 
 *
 * If FIND_EMPTY is not set, this function will return the one used entry
 * that actually matches the hash or NULL, if no such entry exists.
 *
 * If FIND_EMPTY has been set, this function will drop the one used entry
 * that actually matches the hash (i.e. make it fit to be replaced with
 * new content), an unused entry or a forcibly removed entry (if all
 * group entries are currently in use). The entries' hash value will be
 * initialized with TO_FIND.
 */
static entry_t *
find_entry(membuffer_cache_t *cache,
           apr_uint32_t group_index,
           unsigned char *to_find,
           svn_boolean_t find_empty)
{
  entry_t *group;
  entry_t *entry = NULL;
  int i;

  /* get the group that *must* contain the entry
   */
  group = &cache->directory[group_index][0];

  /* try to find the matching entry 
   */
  for (i = 0; i < GROUP_SIZE; ++i)
    if (group[i].offset != -1 && 
        !memcmp(to_find, group[i].key, sizeof(to_find)))
      {
        /* found it
         */
        entry = &group[i];
        if (find_empty)
          drop_entry(cache, entry);

        return entry;
      }

  /* None found. Are we looking for a free entry?
   */
  if (find_empty)
    {
      /* look for an empty entry and return that ...
       */
      for (i = 0; i < GROUP_SIZE; ++i)
        if (group[i].offset == -1)
          {
            entry = &group[i];
            break;
          }

      /* ... or, if none is empty, delete the oldest entry
       */
      if (entry == NULL)
        {
          for (i = 0; i < GROUP_SIZE; ++i)
            if (entry == NULL || entry->hit_count > group[i].hit_count)
              entry = &group[i];

          for (i = 0; i < GROUP_SIZE; ++i)
            if (entry != &group[i])
              {
                /* for the entries that don't have been removed,
                 * reduce their hitcounts to put them at a relative
                 * disadvantage the next time.
                 */
                apr_uint32_t hits_left = entry->hit_count >>= 1;
                cache->hit_count -= entry->hit_count - hits_left;
                entry->hit_count = hits_left;
              }

          drop_entry(cache, entry);
        }

      /* initialize entry for the new key */
      memcpy(entry->key, to_find, sizeof(to_find));
    }

  return entry;
}

/* Move a surviving ENTRY from just behind the insertion window to
 * its beginning and move the insertion window up accordingly.
 */
static void
move_entry(membuffer_cache_t* cache, entry_t *entry)
{
  /* This entry survived this cleansing run. Reset half of its
   * hit count so that its removal gets more likely in the next
   * run unless someone read / hit this entry in the meantime.
   */
  apr_uint32_t hits_left = entry->hit_count >>= 1;
  cache->hit_count -= entry->hit_count - hits_left;
  entry->hit_count = hits_left;

  /* Move the entry to the start of the empty / insertion section 
   * (if it isn't there already) 
   */
  if (entry->offset != cache->current_data)
    {
      memmove(cache->data + cache->current_data,
              cache->data + entry->offset,
              entry->size);
      entry->offset = cache->current_data;
    }

  /* The insertion position is now directly behind this entry.
   */
  cache->current_data = align_entry(entry->offset + entry->size);
  cache->next = entry->next;
}

/* If necessary, enlarge the insertion window until it is at least
 * SIZE bytes long. SIZE must not exceed the data buffer size;
 */
static void
ensure_data_insertable(membuffer_cache_t* cache, apr_size_t size)
{
  int average_hits;
  int threashold;
  entry_t *entry;

  /* Make sure that this function actually terminates.
   */
  assert(cache->data_size >= size);

  while (1)
    {
      /* first offset behind the insertion window
       */
      apr_uint64_t end = cache->next == -1
                       ? cache->data_size
                       : get_entry(cache, cache->next)->offset;

      /* leave function as soon as the insertion window is large enough
       */
      if (end - cache->current_data >= size)
        return;

      /* try to enlarge the insertion window
       */
      if (cache->next == -1)
        {
          /* We reached the end of the data buffer; restart at the beginning.
           * Due to the randomized nature of our LFU implementation, very
           * large data items may require multiple passes. Therefore, SIZE
           * should be restricted to significantly less than data_size.
           */
          cache->current_data = 0;
          cache->next = cache->first;
        }
      else
        {
          /* Roll the dice and determine a threashold somewhere from 0 up
           * to 2 times the average hit count. 
           */
          average_hits = cache->hit_count / cache->used_entries;
          if (average_hits < 1)
            average_hits = 1;

          threashold = rand() % (2 * average_hits);

          /* Drop the entry from the end of the insertion window, if it has
           * been hit less than the threashold. Otherwise, keep it and move
           * the insertion window one entry further.
           */
          entry = get_entry(cache, cache->next);
          if (entry->hit_count >= threashold)
            move_entry(cache, entry);
          else
            drop_entry(cache, entry);
        }
    }
}

/* Create a new membuffer cache instance. If the TOTAL_SIZE of the
 * memory i too small to accomodate the DICTIONARY_SIZE, the latte
 * will be resized automatically. Also, a minumum size is assured
 * for the DICTIONARY_SIZE. THREAD_SAFE may be FALSE, if there will
 * be no concurrent acccess to the CACHE returned.
 *
 * All allocations, in particular the data buffer and dictionary will 
 * be made from POOL.
 */
svn_error_t* 
svn_cache__membuffer_cache_create(membuffer_cache_t **cache,
                                  apr_size_t total_size,
                                  apr_size_t directory_size,
                                  svn_boolean_t thread_safe,
                                  apr_pool_t *pool)
{
  membuffer_cache_t* c = apr_palloc(pool, sizeof(*c));
  int i, k;

  /* We use this sub-pool to allocate the data buffer and the dictionary
   * so that we can release that memory easily upon OOM.
   */
  apr_pool_t *sub_pool = svn_pool_create(pool);

  /* prevent pathological conditions: ensure a certain minimum cache size 
   */
  if (total_size < 2 * sizeof(entry_group_t))
    total_size = 2 * sizeof(entry_group_t);

  /* adapt the dictionary size accordingly, if necessary:
   * It must hold at least one group and must not exceed the cache size.
   */
  if (directory_size > total_size - sizeof(entry_group_t))
    directory_size = total_size - sizeof(entry_group_t);
  if (directory_size < sizeof(entry_group_t))
    directory_size = sizeof(entry_group_t);

  /* allocate buffers and initialize cache members
   */
  c->group_count = directory_size / sizeof (entry_group_t);
  c->directory = apr_palloc(sub_pool, c->group_count * sizeof(entry_group_t));
  c->first = -1;
  c->last = -1;
  c->next = -1;

  c->data_size = total_size - directory_size;
  c->data = apr_palloc(sub_pool, c->data_size);
  c->data = (unsigned char *)align_entry((apr_uint64_t)c->data);
  c->data_size -= ITEM_ALIGNMENT;
  c->current_data = 0;
  c->data_used = 0;

  c->used_entries = 0;
  c->hit_count = 0;
  c->total_reads = 0;
  c->total_writes = 0;
  c->total_hits = 0;

  /* were allocations successful? 
   * If not, initialize a minimal cache structure.
   */
  if (c->data == NULL || c->directory == NULL)
  {
    /* in case we successfully allocated one part of the cache 
     * make sure we release it asap.
     */
    svn_pool_destroy(sub_pool);

    c->group_count = 1;
    c->data_size = 0;

    if (c->directory == NULL)
      c->directory = apr_palloc(pool, sizeof(entry_group_t));

    /* if that modest allocation failed as well, we definitly are OOM. */
    if (c->directory == NULL)
      return svn_error_wrap_apr(APR_ENOMEM, _("OOM"));
  }

  /* initialize directory entries as "unused"
   */
  for (i = 0; i < c->group_count; ++i)
    for (k = 0; k < GROUP_SIZE; ++k)
      c->directory[i][k].offset = -1;

#if APR_HAS_THREADS
  /* A lock for intra-process synchronization to the cache, or NULL if
  * the cache's creator doesn't feel the cache needs to be
  * thread-safe. */

  c->mutex = NULL;
  if (thread_safe)
    {
      apr_status_t status = apr_thread_mutex_create(&(c->mutex),
                                                    APR_THREAD_MUTEX_DEFAULT,
                                                    pool);
      if (status)
        return svn_error_wrap_apr(status, _("Can't create cache mutex"));
    }
#else
  if (thread_safe)
    return svn_error_wrap_apr(APR_ENOTIMPL, _("APR doesn't support threads"));
#endif

  /* done here 
   */
  *cache = c;
  return SVN_NO_ERROR;
}

/* Try to insert the ITEM and use the KEY to unqiuely identify it.
 * However, there is no guarantee that it will actually be put into
 * the cache. If there is already some data associated to the KEY,
 * it will be removed from the cache even if the new data cannot
 * be inserted.
 *
 * The SERIALIZER is called to transform the ITEM into a single,
 * flat data buffer. Temporary allocations may be done in POOL.
 */
static svn_error_t* 
membuffer_cache_set(membuffer_cache_t *cache,
                    const void *key,
                    apr_size_t key_len,
                    void *item,
                    svn_cache__serialize_func_t serializer,
                    apr_pool_t *pool)
{
  apr_uint32_t group_index;
  unsigned char to_find[KEY_SIZE];
  entry_t *entry;
  char *buffer;
  apr_size_t size;
  svn_error_t *err;

  /* find the entry group that will hold the key.
   */
  group_index = get_group_index(cache, key, key_len, to_find, pool);
  if (group_index == -1)
    return err;

  /* Serialize data data.
   */
  SVN_ERR(serializer(&buffer, &size, item, pool));

  /* The actual cache data access needs to sync'ed
   */
  SVN_ERR(lock_cache(cache));

  if (cache->data_size / 4 > size)
    {
      /* if necessary, enlarge the insertion window.
       */
      ensure_data_insertable(cache, size);

      /* Remove old data for this key, if it that exists.
       * Get an unused entry for the key and and initialize it with
       * the serialized item's (future) posion within data buffer.
       */
      entry = find_entry(cache, group_index, to_find, TRUE);
      entry->size = size;
      entry->offset = cache->current_data;

      /* Copy the serialized item data into the cache.
       */
      if (size)
        memcpy(cache->data + entry->offset, buffer, size);

      /* Link the entry properly.
       */
      insert_entry(cache, entry);
      cache->total_writes++;
    }
  else
    {
      /* if there is already an entry for this key, drop it.
       */
      find_entry(cache, group_index, to_find, TRUE);
    }

  /* done here -> unlock the cache
   */
  return unlock_cache(cache, SVN_NO_ERROR);
}

/* Look for the *ITEM identified by KEY. If no item has been stored
 * for KEY, *ITEM will be NULL. Otherwise, the DESERIALIZER is called
 * re-construct the proper object from the serialized data.
 * Allocations will be done in POOL.
 */
static svn_error_t *
membuffer_cache_get(membuffer_cache_t *cache,
                    const void *key,
                    apr_size_t key_len,
                    void **item,
                    svn_cache__deserialize_func_t deserializer,
                    apr_pool_t *pool)
{
  apr_uint32_t group_index;
  unsigned char to_find[KEY_SIZE];
  entry_t *entry;
  svn_error_t *err = SVN_NO_ERROR;

  /* find the entry group that will hold the key.
  */
  group_index = get_group_index(cache, key, key_len, to_find, pool);
  if (group_index == -1)
    {
      /* Some error occured, return "item not found".
       */
      *item = NULL;
      return SVN_NO_ERROR;
    }

  SVN_ERR(lock_cache(cache));

  /* The actual cache data access needs to sync'ed
   */
  entry = find_entry(cache, group_index, to_find, FALSE);
  cache->total_reads++;
  if (entry == NULL)
    {
      /* no such entry found.
       */
      *item = NULL;
    }
  else
    {
      /* update hit statistics
       */
      entry->hit_count++;
      cache->hit_count++;
      cache->total_hits++;

      /* re-construct the original data object from its serialized form.
       */
      err = deserializer(item,
                         (const char*)cache->data + entry->offset,
                         entry->size,
                         pool);
    }

  /* done here -> unlock the cache
   */
  return unlock_cache(cache, err);
}

/* Implement the svn_cache__t interface on top of a shared membuffer cache.
 *
 * Because membuffer caches tend to be very large, there will be rather few
 * of them (usually only one). Thus, the same instance shall be used as the
 * backend to many application-visible svn_cache__t instances. This should
 * also achive global resource usage fairness.
 *
 * To accomodate items from multiple resources, the individual keys must be
 * unique over all sources. This is achived by simply adding a prefix key
 * that unambigously identifies the item's context (e.g. path to the 
 * respective repository). The prefix will be set upon construction of the
 * svn_cache__t instance.
 */

/* Internal cache structure (used in svn_cache__t.cache_internal) basically
 * holding the additional parameters needed to call the respective membuffer
 * functions.
 */
typedef struct svn_membuffer_cache_t
{
  /* this is where all our data will end up in
   */
  membuffer_cache_t *membuffer;

  /* use this conversion function when inserting an item into the memcache
   */
  svn_cache__serialize_func_t serializer;

  /* use this conversion function when reading an item from the memcache
   */
  svn_cache__deserialize_func_t deserializer;

  /* Prepend this byte sequence to any key passed to us.
   * This makes (very likely) our keys different from all keys used
   * by other svn_membuffer_cache_t instances.
   */
  unsigned char prefix [APR_MD5_DIGESTSIZE];

  /* length of the keys that will be passed to us through the
   * svn_cache_t interface. May be APR_HASH_KEY_STRING.
   */
  apr_ssize_t key_len;

  /* a pool for temporary allocations during get() and set() 
   */
  apr_pool_t *pool;

  /* an internal counter that is used to clear the pool from time to time
   * but not too frequently.
   */
  int alloc_counter;
} svn_membuffer_cache_t;

/* After an estimated ALLOCATIONS_PER_POOL_CLEAR allocations, we should
 * clear the svn_membuffer_cache_t.pool to keep memory consumption in check.
 */
#define ALLOCATIONS_PER_POOL_CLEAR 10


/* Basically concatenate PREFIX and KEY and return the result in FULL_KEY.
 * Allocations will be made in POOL.
 */
static void
combine_key(const void *prefix,
            apr_size_t prefix_len,
            const void *key,
            apr_ssize_t key_len,
            void **full_key,
            apr_size_t *full_key_len,
            apr_pool_t *pool)
{
  if (key_len == APR_HASH_KEY_STRING)
    key_len = strlen((const char *) key);

  *full_key_len = prefix_len + key_len;
  *full_key = apr_palloc(pool, *full_key_len);

  memcpy(*full_key, prefix, prefix_len);
  memcpy((char *)*full_key + prefix_len, key, key_len);
}

/* Implement svn_cache__vtable_t.get
 */
static svn_error_t *
svn_membuffer_cache_get(void **value_p,
                        svn_boolean_t *found,
                        void *cache_void,
                        const void *key,
                        apr_pool_t *pool)
{
  svn_membuffer_cache_t *cache = cache_void;

  /* construct the full, i.e. globally unique, key by adding
   * this cache instances' prefix
   */
  void *full_key;
  apr_size_t full_key_len;

  combine_key(cache->prefix,
              sizeof(cache->prefix),
              key,
              cache->key_len,
              &full_key,
              &full_key_len,
              pool);

  /* Look the item up. */
  SVN_ERR(membuffer_cache_get(cache->membuffer,
                              full_key,
                              full_key_len,
                              value_p,
                              cache->deserializer,
                              pool));

  /* We don't need more the key anymore.
   * But since we allocate only small amounts of data per get() call and
   * apr_pool_clear is somewhat expensive, we clear it only now and then.
   */
  if (++cache->alloc_counter > ALLOCATIONS_PER_POOL_CLEAR)
    {
      apr_pool_clear(cache->pool);
      cache->alloc_counter = 0;
    }

  /* return result */
  *found = *value_p != NULL;
  return SVN_NO_ERROR;
}

/* Implement svn_cache__vtable_t.set
 */
static svn_error_t *
svn_membuffer_cache_set(void *cache_void,
                        const void *key,
                        void *value,
                        apr_pool_t *scratch_pool)
{
  svn_membuffer_cache_t *cache = cache_void;

  void *full_key;
  apr_size_t full_key_len;

  /* we do some allocations below, so increase the allocation counter
   * by a slightly larger amount. Free allocated memory every now and then.
   */
  cache->alloc_counter += 3;
  if (cache->alloc_counter > ALLOCATIONS_PER_POOL_CLEAR)
    {
      apr_pool_clear(cache->pool);
      cache->alloc_counter = 0;
    }

  /* construct the full, i.e. globally unique, key by adding
   * this cache instances' prefix
   */
  combine_key(cache->prefix,
              sizeof(cache->prefix),
              key,
              cache->key_len,
              &full_key,
              &full_key_len,
              cache->pool);

  /* (probably) add the item to the cache. But there is no real guarantee
   * that the item will actually be cached afterwards.
   */
  return membuffer_cache_set(cache->membuffer,
                             full_key,
                             full_key_len,
                             value,
                             cache->serializer,
                             cache->pool);
}

/* Implement svn_cache__vtable_t.iter as "not implemented"
 */
static svn_error_t *
svn_membuffer_cache_iter(svn_boolean_t *completed,
                          void *cache_void,
                          svn_iter_apr_hash_cb_t user_cb,
                          void *user_baton,
                          apr_pool_t *scratch_pool)
{
  return svn_error_create(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                          _("Can't iterate a membuffer-based cache"));
}

/* the v-table for membuffer-based caches
 */
static svn_cache__vtable_t membuffer_cache_vtable = {
  svn_membuffer_cache_get,
  svn_membuffer_cache_set,
  svn_membuffer_cache_iter
};

/* standard serialization function for svn_stringbuf_t items
 */
static svn_error_t *
serialize_svn_stringbuf(char **buffer,
                        apr_size_t *buffer_size,
                        void *item,
                        apr_pool_t *pool)
{
  svn_stringbuf_t *value_str = item;

  *buffer = value_str->data;
  *buffer_size = value_str->len;

  return SVN_NO_ERROR;
}

/* standard de-serialization function for svn_stringbuf_t items
 */
static svn_error_t *
deserialize_svn_stringbuf(void **item,
                          const char *buffer,
                          apr_size_t buffer_size,
                          apr_pool_t *pool)
{
  *item = svn_string_ncreate(buffer, buffer_size, pool);
  return SVN_NO_ERROR;
}

/* Construct a svn_cache__t object on top of a shared memcache.
 */
svn_error_t *
svn_cache__create_membuffer_cache(svn_cache__t **cache_p,
                                  membuffer_cache_t *membuffer,
                                  svn_cache__serialize_func_t serializer,
                                  svn_cache__deserialize_func_t deserializer,
                                  apr_ssize_t klen,
                                  const char *prefix,
                                  apr_pool_t *pool)
{
  svn_checksum_t *checksum;

  /* allocate the cache header structures
   */
  svn_cache__t *wrapper = apr_pcalloc(pool, sizeof(*wrapper));
  svn_membuffer_cache_t* cache = apr_palloc(pool, sizeof(*cache));

  /* initialize our internal cache header
   */
  cache->membuffer = membuffer;
  cache->serializer = serializer
                    ? serializer
                    : serialize_svn_stringbuf;
  cache->deserializer = deserializer
                      ? deserializer
                      : deserialize_svn_stringbuf;
  cache->key_len = klen;
  cache->pool = svn_pool_create(pool);
  cache->alloc_counter = 0;

  /* for performance reasons, we don't actually store the full prefix but a
   * hash value of it
   */
  SVN_ERR(svn_checksum(&checksum,
                       svn_checksum_md5,
                       prefix,
                       strlen(prefix),
                       pool));
  memcpy(cache->prefix, checksum->digest, sizeof(cache->prefix));

  /* initialize the generic cache wrapper
   */
  wrapper->vtable = &membuffer_cache_vtable;
  wrapper->cache_internal = cache;
  wrapper->error_handler = 0;
  wrapper->error_baton = 0;

  *cache_p = wrapper;
  return SVN_NO_ERROR;
}

