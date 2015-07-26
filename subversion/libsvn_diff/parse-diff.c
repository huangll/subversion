#include "private/svn_diff_private.h"
svn_error_t *
svn_diff_hunk__create_adds_single_line(svn_diff_hunk_t **hunk_out,
                                       const char *line,
                                       svn_patch_t *patch,
                                       apr_pool_t *result_pool,
                                       apr_pool_t *scratch_pool)
{
  svn_diff_hunk_t *hunk = apr_palloc(result_pool, sizeof(*hunk));
  static const char hunk_header[] = "@@ -0,0 +1 @@\n";
  const unsigned len = strlen(line);
  /* The +1 is for the 'plus' start-of-line character. */
  const apr_off_t end = STRLEN_LITERAL(hunk_header) + (1 + len);
  /* The +1 is for the second \n. */
  svn_stringbuf_t *buf = svn_stringbuf_create_ensure(end + 1, scratch_pool);

  hunk->patch = patch;

  /* hunk->apr_file is created below. */

  hunk->diff_text_range.start = STRLEN_LITERAL(hunk_header);
  hunk->diff_text_range.current = STRLEN_LITERAL(hunk_header);
  hunk->diff_text_range.end = end;

  hunk->original_text_range.start = 0; /* There's no "original" text. */
  hunk->original_text_range.current = 0;
  hunk->original_text_range.end = 0;

  hunk->modified_text_range.start = STRLEN_LITERAL(hunk_header);
  hunk->modified_text_range.current = STRLEN_LITERAL(hunk_header);
  hunk->modified_text_range.end = end;

  hunk->leading_context = 0;
  hunk->trailing_context = 0;

  /* Create APR_FILE and put just a hunk in it (without a diff header).
   * Save the offset of the last byte of the diff line. */
  svn_stringbuf_appendbytes(buf, hunk_header, STRLEN_LITERAL(hunk_header));
  svn_stringbuf_appendbyte(buf, '+');
  svn_stringbuf_appendbytes(buf, line, len);
  svn_stringbuf_appendbyte(buf, '\n');

  SVN_ERR(svn_io_open_unique_file3(&hunk->apr_file, NULL /* filename */,
                                   NULL /* system tempdir */,
                                   svn_io_file_del_on_pool_cleanup,
                                   result_pool, scratch_pool));
  SVN_ERR(svn_io_file_write_full(hunk->apr_file,
                                 buf->data, buf->len,
                                 NULL, scratch_pool));
  /* No need to seek. */

  *hunk_out = hunk;
  return SVN_NO_ERROR;
}

   state_old_mode_seen,   /* old mode 100644 */
/* Helper for git_old_mode() and git_new_mode().  Translate the git
 * file mode MODE_STR into a binary "executable?" notion EXECUTABLE_P. */
static svn_error_t *
parse_bits_into_executability(svn_tristate_t *executable_p,
                              const char *mode_str)
{
  apr_uint64_t mode;
  SVN_ERR(svn_cstring_strtoui64(&mode, mode_str,
                                0 /* min */,
                                0777777 /* max: six octal digits */,
                                010 /* radix (octal) */));
  switch (mode & 0777)
    {
      case 0644:
        *executable_p = svn_tristate_false;
        break;

      case 0755:
        *executable_p = svn_tristate_true;
        break;

      default:
        /* Ignore unknown values. */
        *executable_p = svn_tristate_unknown;
        break;
    }

  return SVN_NO_ERROR;
}

/* Parse the 'old mode ' line of a git extended unidiff. */
static svn_error_t *
git_old_mode(enum parse_state *new_state, char *line, svn_patch_t *patch,
             apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  SVN_ERR(parse_bits_into_executability(&patch->old_executable_p,
                                        line + STRLEN_LITERAL("old mode ")));

#ifdef SVN_DEBUG
  /* If this assert trips, the "old mode" is neither ...644 nor ...755 . */
  SVN_ERR_ASSERT(patch->old_executable_p != svn_tristate_unknown);
#endif

  *new_state = state_old_mode_seen;
  return SVN_NO_ERROR;
}

/* Parse the 'new mode ' line of a git extended unidiff. */
static svn_error_t *
git_new_mode(enum parse_state *new_state, char *line, svn_patch_t *patch,
             apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  SVN_ERR(parse_bits_into_executability(&patch->new_executable_p,
                                        line + STRLEN_LITERAL("new mode ")));

#ifdef SVN_DEBUG
  /* If this assert trips, the "old mode" is neither ...644 nor ...755 . */
  SVN_ERR_ASSERT(patch->new_executable_p != svn_tristate_unknown);
#endif

  /* Don't touch patch->operation. */

  *new_state = state_git_tree_seen;
  return SVN_NO_ERROR;
}

  SVN_ERR(
    parse_bits_into_executability(&patch->new_executable_p,
                                  line + STRLEN_LITERAL("new file mode ")));

  SVN_ERR(
    parse_bits_into_executability(&patch->old_executable_p,
                                  line + STRLEN_LITERAL("deleted file mode ")));



  {"old mode ",     state_git_diff_seen,    git_old_mode},
  {"new mode ",     state_old_mode_seen,    git_new_mode},




  patch->old_executable_p = svn_tristate_unknown;
  patch->new_executable_p = svn_tristate_unknown;