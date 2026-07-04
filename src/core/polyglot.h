#pragma once

#include <stdint.h>

/* Find ZIP local header offset in a potential polyglot file.
   Returns: 1 = found, 0 = not found, -1 = error. */
int polyglot_find_zip_start(const char *filepath, uint64_t *zip_start);

/* Copy bytes [zip_start, EOF) from src_path to dst_path.
   Returns: 0 = success, -1 = error. */
int polyglot_copy_tail(const char *src_path, uint64_t zip_start,
                       const char *dst_path);

/* Build a temporary fixed ZIP file from a polyglot source.
   Returns:
     1  = temp fixed file created (out_temp_path owned by caller)
     0  = not applicable (no ZIP found or already plain ZIP)
    -1  = error
*/
int polyglot_make_temp_fixed_zip(const char *src_path, char **out_temp_path,
                                 uint64_t *out_zip_start);

/* Best-effort cleanup for temp file path created above. */
void polyglot_cleanup_temp(char **temp_path);

/* Return whether a non-password 7z failure should still try the polyglot
   fallback. Some archive parse errors are exactly how embedded ZIP payloads
   surface, while environmental errors cannot be fixed by trimming a prefix. */
int polyglot_should_try_after_7z_error(const char *error_output);
