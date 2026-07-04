#include "compress-progress.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <gio/gio.h>

double
progress_parse_7z_line(const char *line, char **out_current_file)
{
    if (out_current_file)
        *out_current_file = NULL;

    if (!line)
        return -1.0;

    /* 7z -bsp2 output format: "  9% + filename.txt" or " 45% 12 + filename.txt" or "100%" */
    const char *p = line;

    /* skip leading whitespace and backspaces */
    while (*p && (isspace((unsigned char)*p) || *p == '\b'))
        p++;

    /* expect digits followed by '%' */
    if (!isdigit((unsigned char)*p))
        return -1.0;

    const char *num_start = p;
    while (isdigit((unsigned char)*p))
        p++;

    if (*p != '%')
        return -1.0;

    int percent = atoi(num_start);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    /* try to extract filename: skip past "% [number] +|- " */
    p++; /* skip '%' */
    while (*p && (isspace((unsigned char)*p) || *p == '\b'))
        p++;

    /* skip optional file index number */
    while (isdigit((unsigned char)*p))
        p++;
    while (*p && (isspace((unsigned char)*p) || *p == '\b'))
        p++;

    /* skip "+", "-", or "U" separator */
    if (*p == '+' || *p == '-' || *p == 'U') {
        p++;
        while (*p && (isspace((unsigned char)*p) || *p == '\b'))
            p++;
    }

    if (out_current_file && *p) {
        /* trim trailing whitespace/backspaces/CR/LF */
        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == '\n' ||
               p[len - 1] == '\b' || isspace((unsigned char)p[len - 1])))
            len--;
        if (len > 0)
            *out_current_file = g_strndup(p, len);
    }

    return percent / 100.0;
}

double
progress_parse_tar_line(const char *line, int total_files, int *processed_count)
{
    if (!line || !processed_count || total_files <= 0)
        return -1.0;

    /* Each line from tar -v is a filename being processed */
    /* Skip empty lines */
    const char *p = line;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p == '\0')
        return -1.0;

    (*processed_count)++;

    return (double)(*processed_count) / (double)total_files;
}

static void
count_files_recursive(GFile *file, int *count)
{
    GFileType type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL);

    if (type == G_FILE_TYPE_DIRECTORY) {
        GFileEnumerator *enumerator = g_file_enumerate_children(
            file, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);

        if (enumerator) {
            GFileInfo *info;
            while ((info = g_file_enumerator_next_file(enumerator, NULL, NULL)) != NULL) {
                GFile *child = g_file_enumerator_get_child(enumerator, info);
                count_files_recursive(child, count);
                g_object_unref(child);
                g_object_unref(info);
            }
            g_object_unref(enumerator);
        }
        (*count)++; /* count the directory itself */
    } else {
        (*count)++;
    }
}

int
progress_count_files(char **paths, int count)
{
    int total = 0;
    for (int i = 0; i < count; i++) {
        GFile *file = g_file_new_for_path(paths[i]);
        count_files_recursive(file, &total);
        g_object_unref(file);
    }
    return total;
}
