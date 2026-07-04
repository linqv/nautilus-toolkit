#ifndef COMPRESS_PROGRESS_H
#define COMPRESS_PROGRESS_H

#include <glib.h>

/**
 * Parse a line of 7z stdout output (with -bsp1) for progress info.
 * Returns progress fraction (0.0–1.0), or -1.0 if line is not a progress line.
 * If out_current_file is non-NULL, sets it to the current filename (caller frees).
 */
double progress_parse_7z_line(const char *line, char **out_current_file);

/**
 * Parse a line of tar -v output for progress info.
 * Increments *processed_count and returns fraction = *processed_count / total_files.
 * Returns the current filename via out_current_file (caller frees).
 */
double progress_parse_tar_line(const char *line, int total_files, int *processed_count);

/**
 * Count total files recursively under the given paths.
 * Used to compute tar progress denominator.
 */
int progress_count_files(char **paths, int count);

#endif /* COMPRESS_PROGRESS_H */
