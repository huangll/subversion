/*
 * log.c -- Fetch log data and implement the log queries
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

#include "svn_cmdline.h"
#include "svn_client.h"
#include "svn_dirent_uri.h"
#include "svn_string.h"
#include "svn_path.h"
#include "svn_error.h"
#include "svn_sorts.h"
#include "svn_pools.h"
#include "svn_hash.h"

#include "private/svn_fspath.h"
#include "private/svn_subr_private.h"
#include "private/svn_sorts_private.h"

#include "mergeinfo-normalizer.h"

#include "svn_private_config.h"


/*** Code. ***/

typedef struct log_entry_t
{
  svn_revnum_t revision;
  const char *common_base;
  apr_array_header_t *paths;
} log_entry_t;

typedef struct copy_t
{
  const char *path;
  svn_revnum_t revision;

  const char *copyfrom_path;
  svn_revnum_t copyfrom_revision;
} copy_t;

typedef struct deletion_t
{
  const char *path;
  svn_revnum_t revision;
} deletion_t;

struct svn_min__log_t
{
  apr_hash_t *unique_paths;
  apr_pool_t *pool;

  svn_revnum_t first_rev;
  svn_revnum_t head_rev;
  apr_array_header_t *entries;

  apr_array_header_t *copies;
  apr_array_header_t *deletions;

  svn_boolean_t quiet;
};

static int
copy_order(const void *lhs,
           const void *rhs)
{
  const copy_t *lhs_copy = *(const copy_t * const *)lhs;
  const copy_t *rhs_copy = *(const copy_t * const *)rhs;

  int diff = strcmp(lhs_copy->path, rhs_copy->path);
  if (diff)
    return diff;

  if (lhs_copy->revision < rhs_copy->revision)
    return -1;

  return lhs_copy->revision == rhs_copy->revision ? 0 : 1;
}

static int
deletion_order(const void *lhs,
               const void *rhs)
{
  const deletion_t *lhs_deletion = *(const deletion_t * const *)lhs;
  const deletion_t *rhs_deletion = *(const deletion_t * const *)rhs;

  int diff = strcmp(lhs_deletion->path, rhs_deletion->path);
  if (diff)
    return diff;

  if (lhs_deletion->revision < rhs_deletion->revision)
    return -1;

  return lhs_deletion->revision == rhs_deletion->revision ? 0 : 1;
}

static const char *
internalize(apr_hash_t *unique_paths,
            const char *path,
            apr_ssize_t path_len,
            apr_pool_t *result_pool)
{
  const char *result = apr_hash_get(unique_paths, path, path_len);
  if (result == NULL)
    {
      result = apr_pstrmemdup(result_pool, path, path_len);
      apr_hash_set(unique_paths, result, path_len, result);
    }

  return result;
}

static svn_error_t *
log_entry_receiver(void *baton,
                   svn_log_entry_t *log_entry,
                   apr_pool_t *scratch_pool)
{
  svn_min__log_t *log = baton;
  apr_pool_t *result_pool = log->entries->pool;
  log_entry_t *entry;
  apr_hash_index_t *hi;
  const char *common_base;
  int count;

  if (!log_entry->changed_paths || !apr_hash_count(log_entry->changed_paths))
    return SVN_NO_ERROR;

  entry = apr_pcalloc(result_pool, sizeof(*entry));
  entry->revision = log_entry->revision;
  entry->paths = apr_array_make(result_pool,
                                apr_hash_count(log_entry->changed_paths),
                                sizeof(const char *));

  for (hi = apr_hash_first(scratch_pool, log_entry->changed_paths);
       hi;
       hi = apr_hash_next(hi))
    {
      const char *path = apr_hash_this_key(hi);
      svn_log_changed_path_t *change = apr_hash_this_val(hi);

      path = internalize(log->unique_paths, path, apr_hash_this_key_len(hi),
                         log->pool);
      APR_ARRAY_PUSH(entry->paths, const char *) = path;

      if (change->action == 'D' || change->action == 'R')
        {
          deletion_t *deletion = apr_pcalloc(log->pool, sizeof(*deletion));
          deletion->path = path;
          deletion->revision = log_entry->revision;

          APR_ARRAY_PUSH(log->deletions, deletion_t *) = deletion;
        }

      if (SVN_IS_VALID_REVNUM(change->copyfrom_rev))
        {
          copy_t *copy = apr_pcalloc(log->pool, sizeof(*copy));
          copy->path = path;
          copy->revision = log_entry->revision;
          copy->copyfrom_path = internalize(log->unique_paths,
                                            change->copyfrom_path,
                                            strlen(change->copyfrom_path),
                                            log->pool);
          copy->copyfrom_revision = change->copyfrom_rev;

          APR_ARRAY_PUSH(log->copies, copy_t *) = copy;
        }
    }

  count = entry->paths->nelts;
  if (count == 1)
    {
      entry->common_base = APR_ARRAY_IDX(entry->paths, 0, const char *);
    }
  else
    {
      svn_sort__array(entry->paths, svn_sort_compare_paths);

      common_base = svn_dirent_get_longest_ancestor(
                      APR_ARRAY_IDX(entry->paths, 0, const char *),
                      APR_ARRAY_IDX(entry->paths, count - 1, const char *),
                      scratch_pool);
      entry->common_base = internalize(log->unique_paths, common_base,
                                       strlen(common_base), log->pool);
    }

  APR_ARRAY_PUSH(log->entries, log_entry_t *) = entry;

  log->first_rev = log_entry->revision;
  if (log->head_rev == SVN_INVALID_REVNUM)
    log->head_rev = log_entry->revision;

  if (log->entries->nelts % 1000 == 0 && !log->quiet)
    {
      SVN_ERR(svn_cmdline_printf(scratch_pool, "."));
      SVN_ERR(svn_cmdline_fflush(stdout));
    }

  return SVN_NO_ERROR;
}


/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_min__log(svn_min__log_t **log,
             const char *url,
             svn_min__cmd_baton_t *baton,
             apr_pool_t *result_pool,
             apr_pool_t *scratch_pool)
{
  svn_client_ctx_t *ctx = baton->ctx;
  svn_min__log_t *result;

  apr_array_header_t *targets;
  apr_array_header_t *revisions;
  apr_array_header_t *revprops;
  svn_opt_revision_t peg_revision = { svn_opt_revision_head };
  svn_opt_revision_range_t range = { { svn_opt_revision_unspecified },
                                     { svn_opt_revision_unspecified } };

  targets = apr_array_make(scratch_pool, 1, sizeof(const char *));
  APR_ARRAY_PUSH(targets, const char *) = url;

  revisions = apr_array_make(scratch_pool, 1, sizeof(&range));
  APR_ARRAY_PUSH(revisions, svn_opt_revision_range_t *) = &range;

  revprops = apr_array_make(scratch_pool, 0, sizeof(const char *));

  result = apr_pcalloc(result_pool, sizeof(*result));
  result->unique_paths = svn_hash__make(scratch_pool);
  result->pool = result_pool;
  result->first_rev = SVN_INVALID_REVNUM;
  result->head_rev = SVN_INVALID_REVNUM;
  result->entries = apr_array_make(result_pool, 1024, sizeof(log_entry_t *));
  result->copies = apr_array_make(result_pool, 1024, sizeof(copy_t *));
  result->deletions = apr_array_make(result_pool, 1024, sizeof(deletion_t *));
  result->quiet = baton->opt_state->quiet;

  if (!baton->opt_state->quiet)
    {
      SVN_ERR(svn_cmdline_printf(scratch_pool,
                                 _("Fetching log for %s ..."),
                                 url));
      SVN_ERR(svn_cmdline_fflush(stdout));
    }

  SVN_ERR(svn_client_log5(targets,
                          &peg_revision,
                          revisions,
                          0, /* no limit */
                          TRUE, /* verbose */
                          TRUE, /* stop-on-copy */
                          FALSE, /* merge history */
                          revprops,
                          log_entry_receiver,
                          result,
                          ctx,
                          scratch_pool));

  svn_sort__array_reverse(result->entries, scratch_pool);
  svn_sort__array(result->copies, copy_order);
  svn_sort__array(result->deletions, deletion_order);

  if (!baton->opt_state->quiet)
    {
      SVN_ERR(svn_cmdline_printf(scratch_pool, "\n"));
      SVN_ERR(svn_min__print_log_stats(result, scratch_pool));
    }

  result->unique_paths = NULL;
  *log = result;

  return SVN_NO_ERROR;
}

static void
append_rev_to_ranges(svn_rangelist_t *ranges,
                     svn_revnum_t revision,
                     svn_boolean_t inheritable,
                     apr_pool_t *result_pool)
{
  svn_merge_range_t *range;
  if (ranges->nelts)
    {
      range = APR_ARRAY_IDX(ranges, ranges->nelts - 1, svn_merge_range_t *);
      if (range->end + 1 == revision && range->inheritable == inheritable)
        {
          range->end = revision;
          return;
        }
    }

  range = apr_pcalloc(result_pool, sizeof(*range));
  range->start = revision - 1;
  range->end = revision;
  range->inheritable = inheritable;

  APR_ARRAY_PUSH(ranges, svn_merge_range_t *) = range;
}

static int
compare_rev_log_entry(const void *lhs,
                      const void *rhs)
{
  const log_entry_t *entry = *(const log_entry_t * const *)lhs;
  svn_revnum_t revision = *(const svn_revnum_t *)rhs;

  if (entry->revision < revision)
    return -1;

  return entry->revision == revision ? 0 : 1;
}

static void
restrict_range(svn_min__log_t *log,
               svn_merge_range_t *range,
               svn_rangelist_t *ranges,
               apr_pool_t *result_pool)
{
  if (range->start + 1 < log->first_rev)
    {
      svn_merge_range_t *new_range
        = apr_pmemdup(result_pool, range, sizeof(*range));
      new_range->end = MIN(new_range->end, log->first_rev - 1);

      APR_ARRAY_PUSH(ranges, svn_merge_range_t *) = new_range;
      range->start = new_range->end;
    }

  if (range->end > log->head_rev)
    {
      svn_merge_range_t *new_range
        = apr_pmemdup(result_pool, range, sizeof(*range));
      new_range->start = log->head_rev;

      APR_ARRAY_PUSH(ranges, svn_merge_range_t *) = new_range;
      range->end = new_range->start;
    }
}

static svn_boolean_t
is_relevant(const char *changed_path,
            const char *path,
            const void *baton)
{
  return  svn_dirent_is_ancestor(changed_path, path)
       || svn_dirent_is_ancestor(path, changed_path);
}

static svn_boolean_t
in_subtree(const char *changed_path,
           const char *sub_tree,
           const void *baton)
{
  return svn_dirent_is_ancestor(sub_tree, changed_path);
}

static svn_boolean_t
below_path_outside_subtree(const char *changed_path,
                           const char *path,
                           const void *baton)
{
  const char *subtree = baton;

  /* Is this a change _below_ PATH but not within SUBTREE? */
  return   !svn_dirent_is_ancestor(subtree, changed_path)
        && svn_dirent_is_ancestor(path, changed_path)
        && strcmp(path, changed_path);
}

static svn_rangelist_t *
filter_ranges(svn_min__log_t *log,
              const char *path,
              svn_rangelist_t *ranges,
              svn_boolean_t (*path_relavent)(const char*, const char *,
                                             const void *),
              const void *baton,
              apr_pool_t *result_pool)
{
  svn_rangelist_t *result;
  int i, k, l;

  if (!SVN_IS_VALID_REVNUM(log->first_rev))
    return svn_rangelist_dup(ranges, result_pool);

  result = apr_array_make(result_pool, 0, ranges->elt_size);
  for (i = 0; i < ranges->nelts; ++i)
    {
      svn_merge_range_t range
        = *APR_ARRAY_IDX(ranges, i, const svn_merge_range_t *);
      restrict_range(log, &range, result, result_pool);

      ++range.start;
      for (k = svn_sort__bsearch_lower_bound(log->entries, &range.start,
                                             compare_rev_log_entry);
           k < log->entries->nelts;
           ++k)
        {
          const log_entry_t *entry = APR_ARRAY_IDX(log->entries, k,
                                                  const log_entry_t *);
          if (entry->revision > range.end)
            break;

          if (!is_relevant(entry->common_base, path, NULL))
            continue;

          for (l = 0; l < entry->paths->nelts; ++l)
            {
              const char *changed_path
                = APR_ARRAY_IDX(entry->paths, l, const char *);

              /* Is this a change _below_ PATH but not within SUBTREE? */
              if (path_relavent(changed_path, path, baton))
                {
                  append_rev_to_ranges(result, entry->revision,
                                       range.inheritable, result_pool);
                  break;
                }
            }
        }
    }

  return result;
}

svn_rangelist_t *
svn_min__operative(svn_min__log_t *log,
                   const char *path,
                   svn_rangelist_t *ranges,
                   apr_pool_t *result_pool)
{
  return filter_ranges(log, path, ranges, in_subtree, NULL, result_pool);
}

svn_rangelist_t *
svn_min__operative_outside_subtree(svn_min__log_t *log,
                                   const char *path,
                                   const char *subtree,
                                   svn_rangelist_t *ranges,
                                   apr_pool_t *result_pool)
{
  return filter_ranges(log, path, ranges, below_path_outside_subtree,
                       subtree, result_pool);
}

svn_revnum_t
svn_min__find_deletion(svn_min__log_t *log,
                       const char *path,
                       svn_revnum_t lower_rev,
                       svn_revnum_t upper_rev,
                       apr_pool_t *scratch_pool)
{
  svn_revnum_t latest = SVN_INVALID_REVNUM;

  deletion_t *to_find = apr_pcalloc(scratch_pool, sizeof(*to_find));
  to_find->path = path;
  to_find->revision = lower_rev;

  if (!SVN_IS_VALID_REVNUM(upper_rev))
    upper_rev = log->head_rev;

  if (!svn_fspath__is_root(to_find->path, strlen(to_find->path)))
    {
      int i;
      for (i = svn_sort__bsearch_lower_bound(log->deletions, &to_find,
                                             deletion_order);
           i < log->deletions->nelts;
           ++i)
        {
          const deletion_t *deletion = APR_ARRAY_IDX(log->deletions, i,
                                                     const deletion_t *);
          if (strcmp(deletion->path, to_find->path))
            break;
          if (deletion->revision > upper_rev)
            break;

          latest = deletion->revision;
          to_find->revision = deletion->revision;
        }

      to_find->path = svn_fspath__dirname(to_find->path, scratch_pool);
    }

  return latest;
}

apr_array_header_t *
svn_min__find_deletions(svn_min__log_t *log,
                        const char *path,
                        apr_pool_t *result_pool,
                        apr_pool_t *scratch_pool)
{
  apr_array_header_t *result = apr_array_make(result_pool, 0,
                                              sizeof(svn_revnum_t));
  int source, dest;

  deletion_t *to_find = apr_pcalloc(scratch_pool, sizeof(*to_find));
  to_find->path = path;
  to_find->revision = 0;

  if (!svn_fspath__is_root(to_find->path, strlen(to_find->path)))
    {
      int i;
      for (i = svn_sort__bsearch_lower_bound(log->deletions, &to_find,
                                             deletion_order);
           i < log->deletions->nelts;
           ++i)
        {
          const deletion_t *deletion = APR_ARRAY_IDX(log->deletions, i,
                                                     const deletion_t *);
          if (strcmp(deletion->path, to_find->path))
            break;

          APR_ARRAY_PUSH(result, svn_revnum_t) = deletion->revision;
        }

      to_find->path = svn_fspath__dirname(to_find->path, scratch_pool);
    }

  svn_sort__array(result, svn_sort_compare_revisions);
  for (source = 1, dest = 0; source < result->nelts; ++source)
    {
      svn_revnum_t source_rev = APR_ARRAY_IDX(result, source, svn_revnum_t);
      svn_revnum_t dest_rev = APR_ARRAY_IDX(result, dest, svn_revnum_t);
      if (source_rev != dest_rev)
        {
          ++dest_rev;
          APR_ARRAY_IDX(result, dest, svn_revnum_t) = source_rev;
        }
    }

  if (result->nelts)
    result->nelts = dest + 1;

  return result;
}

typedef struct segment_t
{
  const char *path;
  svn_revnum_t start;
  svn_revnum_t end;
} segment_t;

static const copy_t *
next_copy(svn_min__log_t *log,
          const char *path,
          svn_revnum_t revision,
          apr_pool_t *scratch_pool)
{
  const copy_t *copy = NULL;
  int idx;

  copy_t *to_find = apr_pcalloc(scratch_pool, sizeof(*to_find));
  to_find->path = path;
  to_find->revision = revision;
 
  idx = svn_sort__bsearch_lower_bound(log->copies, &to_find, copy_order);
  if (idx < log->copies->nelts)
    {
      /* Found an exact match? */
      copy = APR_ARRAY_IDX(log->copies, idx, const copy_t *);
      if (copy->revision != revision || strcmp(copy->path, path))
        copy = NULL;
    }

  if (!copy && idx > 0)
    {
      /* No exact match. The predecessor may be the closest copy. */
      copy = APR_ARRAY_IDX(log->copies, idx - 1, const copy_t *);
      if (strcmp(copy->path, path))
        copy = NULL;
    }

  /* Mabye, the parent folder got copied later, i.e. is the closest copy.
     We implicitly recurse up the tree. */
  if (!svn_fspath__is_root(to_find->path, strlen(to_find->path)))
    {
      const copy_t *parent_copy;
      to_find->path = svn_fspath__dirname(to_find->path, scratch_pool);

      parent_copy = next_copy(log, to_find->path, revision, scratch_pool);
      if (!copy)
        copy = parent_copy;
      else if (parent_copy && parent_copy->revision > copy->revision)
        copy = parent_copy;
    }

  return copy;
}

apr_array_header_t *
svn_min__get_history(svn_min__log_t *log,
                     const char *path,
                     svn_revnum_t start_rev,
                     svn_revnum_t end_rev,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  segment_t *segment;
  const copy_t *copy;
  apr_array_header_t *result = apr_array_make(result_pool, 16,
                                              sizeof(segment_t *));

  if (!SVN_IS_VALID_REVNUM(start_rev))
    start_rev = log->head_rev;

  for (copy = next_copy(log, path, start_rev, scratch_pool);
       copy && start_rev >= end_rev;
       copy = next_copy(log, path, start_rev, scratch_pool))
    {
      segment = apr_pcalloc(result_pool, sizeof(*segment));
      segment->start = MAX(end_rev, copy->revision);
      segment->end = start_rev;
      segment->path = apr_pstrdup(result_pool, path);

      APR_ARRAY_PUSH(result, segment_t *) = segment;

      start_rev = copy->copyfrom_revision;
      path = svn_fspath__join(copy->copyfrom_path,
                              svn_fspath__skip_ancestor(copy->path, path),
                              scratch_pool);
    }

  if (start_rev >= end_rev)
    {
      segment = apr_pcalloc(result_pool, sizeof(*segment));
      segment->start = end_rev;
      segment->end = start_rev;
      segment->path = apr_pstrdup(result_pool, path);

      APR_ARRAY_PUSH(result, segment_t *) = segment;
    }

  return result;
}

static int
compare_history_revision(const void *lhs,
                         const void *rhs)
{
  const segment_t *segment = *(const segment_t * const *)lhs;
  svn_revnum_t revision = *(const svn_revnum_t *)rhs;

  if (segment->start < revision)
    return -1;

  return segment->start == revision ? 0 : 1;
}

apr_array_header_t *
svn_min__intersect_history(apr_array_header_t *lhs,
                           apr_array_header_t *rhs,
                           apr_pool_t *result_pool)
{
  apr_array_header_t *result = apr_array_make(result_pool, 16,
                                              sizeof(segment_t *));

  int lhs_idx = 0;
  int rhs_idx = 0;

  /* Careful: the segments are ordered latest to oldest. */
  while (lhs_idx < lhs->nelts && rhs_idx < rhs->nelts)
    {
      segment_t *lhs_segment = APR_ARRAY_IDX(lhs, lhs_idx, segment_t *);
      segment_t *rhs_segment = APR_ARRAY_IDX(rhs, rhs_idx, segment_t *);

      /* Skip non-overlapping revision segments */
      if (lhs_segment->start > rhs_segment->end)
        {
          ++lhs_idx;
          continue;
        }
      else if (lhs_segment->end < rhs_segment->start)
        {
          ++rhs_idx;
          continue;
        }

      /* Revision ranges overlap. Also the same path? */
      if (!strcmp(lhs_segment->path, rhs_segment->path))
        {
          segment_t *segment = apr_pcalloc(result_pool, sizeof(*segment));
          segment->start = MAX(lhs_segment->start, rhs_segment->start);
          segment->end = MIN(lhs_segment->end, rhs_segment->end);
          segment->path = apr_pstrdup(result_pool, lhs_segment->path);

          APR_ARRAY_PUSH(result, segment_t *) = segment;
        }

      /* The segment that starts earlier may overlap with another one.
         If they should start at the same rev, the next iteration will
         skip the respective other segment. */
      if (lhs_segment->start > rhs_segment->start)
        ++lhs_idx;
      else
        ++rhs_idx;
    }

   return result;
}

svn_rangelist_t *
svn_min__history_ranges(apr_array_header_t *history,
                        apr_pool_t *result_pool)
{
  svn_rangelist_t *result = apr_array_make(result_pool, history->nelts,
                                           sizeof(svn_merge_range_t *));

  int i;
  for (i = 0; i < history->nelts; ++i)
    {
      const segment_t *segment = APR_ARRAY_IDX(history, i, segment_t *);

      /* Convert to merge ranges.  Note that start+1 is the first rev
         actually in that range. */
      svn_merge_range_t *range = apr_pcalloc(result_pool, sizeof(*range));
      range->start = MAX(0, segment->start - 1);
      range->end = segment->end;
      range->inheritable = TRUE;

      APR_ARRAY_PUSH(result, svn_merge_range_t *) = range;
    }

  return result;
}

svn_error_t *
svn_min__print_log_stats(svn_min__log_t *log,
                         apr_pool_t *scratch_pool)
{
  int change_count = 0;

  int i;
  for (i = 0; i < log->entries->nelts; ++i)
    {
      const log_entry_t *entry = APR_ARRAY_IDX(log->entries, i,
                                               const log_entry_t *);
      change_count += entry->paths->nelts;
    }

  SVN_ERR(svn_cmdline_printf(scratch_pool,
                             _("    Received %d revisions from %ld to %ld.\n"),
                             log->entries->nelts, log->first_rev,
                             log->head_rev));
  SVN_ERR(svn_cmdline_printf(scratch_pool,
                             _("    Received %d path changes.\n"),
                             change_count));
  SVN_ERR(svn_cmdline_printf(scratch_pool,
                             _("    Pool has %u different paths.\n\n"),
                             apr_hash_count(log->unique_paths)));

  return SVN_NO_ERROR;
}