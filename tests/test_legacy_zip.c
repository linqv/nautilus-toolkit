#include "exec7z.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)((v >> 8) & 0xff), f);
}

static void put32(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)((v >> 8) & 0xff), f);
  fputc((int)((v >> 16) & 0xff), f);
  fputc((int)((v >> 24) & 0xff), f);
}

static long tell_or_die(FILE *f) {
  long pos = ftell(f);
  if (pos < 0) {
    perror("ftell");
    exit(2);
  }
  return pos;
}

static void write_zip_with_name(const char *path, const unsigned char *name,
                                size_t name_len, unsigned flags,
                                const unsigned char *extra,
                                size_t extra_len) {
  FILE *f = fopen(path, "wb");
  if (!f) {
    perror(path);
    exit(2);
  }

  put32(f, 0x04034b50u);
  put16(f, 20);
  put16(f, flags);
  put16(f, 0);
  put16(f, 0);
  put16(f, 0);
  put32(f, 0);
  put32(f, 0);
  put32(f, 0);
  put16(f, (unsigned)name_len);
  put16(f, (unsigned)extra_len);
  fwrite(name, 1, name_len, f);
  if (extra_len > 0)
    fwrite(extra, 1, extra_len, f);

  long cd_offset = tell_or_die(f);
  put32(f, 0x02014b50u);
  put16(f, 20);
  put16(f, 20);
  put16(f, flags);
  put16(f, 0);
  put16(f, 0);
  put16(f, 0);
  put32(f, 0);
  put32(f, 0);
  put32(f, 0);
  put16(f, (unsigned)name_len);
  put16(f, (unsigned)extra_len);
  put16(f, 0);
  put16(f, 0);
  put16(f, 0);
  put32(f, 0);
  put32(f, 0);
  fwrite(name, 1, name_len, f);
  if (extra_len > 0)
    fwrite(extra, 1, extra_len, f);

  long cd_end = tell_or_die(f);
  put32(f, 0x06054b50u);
  put16(f, 0);
  put16(f, 0);
  put16(f, 1);
  put16(f, 1);
  put32(f, (unsigned)(cd_end - cd_offset));
  put32(f, (unsigned)cd_offset);
  put16(f, 0);

  fclose(f);
}

static char *temp_zip_path(const char *suffix) {
  char tmpl[256];
  snprintf(tmpl, sizeof(tmpl), "/tmp/ntk-legacy-zip-%s-XXXXXX", suffix);
  int fd = mkstemp(tmpl);
  if (fd < 0) {
    perror("mkstemp");
    exit(2);
  }
  close(fd);
  return strdup(tmpl);
}

static void expect_detect(const char *label, const char *path, int expected) {
  int got = archive_has_legacy_gbk_zip_names(path);
  if (got != expected) {
    fprintf(stderr, "%s: expected %d got %d\n", label, expected, got);
    exit(1);
  }
}

static void expect_need(const char *label, const char *path,
                        const char *listing, int expected) {
  int got = archive_needs_legacy_gbk_zip_fallback(path, listing);
  if (got != expected) {
    fprintf(stderr, "%s: expected %d got %d\n", label, expected, got);
    exit(1);
  }
}

int main(void) {
  const unsigned char gbk_name[] = {
      0xb6, 0xe0, 0xc8, 0xcb, '.', 't', 'x', 't'};
  const unsigned char utf8_name[] = {
      0xe5, 0xa4, 0x9a, 0xe4, 0xba, 0xba, '.', 't', 'x', 't'};
  const unsigned char ascii_name[] = {'r', 'e', 'a', 'd', 'm', 'e', '.', 't',
                                      'x', 't'};
  const unsigned char unicode_path_extra[] = {
      0x75, 0x70, 0x0f, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
      0xe5, 0xa4, 0x9a, 0xe4, 0xba, 0xba, '.',  't',  'x',  't'};
  const char invalid_listing[] = {'x', ' ', (char)0xb6, (char)0xe0, '\n', 0};

  char *gbk_zip = temp_zip_path("gbk");
  char *utf8_zip = temp_zip_path("utf8");
  char *ascii_zip = temp_zip_path("ascii");
  char *unicode_extra_zip = temp_zip_path("unicode-extra");

  write_zip_with_name(gbk_zip, gbk_name, sizeof(gbk_name), 0, NULL, 0);
  write_zip_with_name(utf8_zip, utf8_name, sizeof(utf8_name), 0x0800, NULL, 0);
  write_zip_with_name(ascii_zip, ascii_name, sizeof(ascii_name), 0, NULL, 0);
  write_zip_with_name(unicode_extra_zip, gbk_name, sizeof(gbk_name), 0,
                      unicode_path_extra, sizeof(unicode_path_extra));

  expect_detect("gbk legacy filename", gbk_zip, 1);
  expect_detect("utf8 filename flag", utf8_zip, 0);
  expect_detect("ascii filename", ascii_zip, 0);
  expect_detect("unicode path extra field", unicode_extra_zip, 0);

  expect_need("gbk invalid 7z listing", gbk_zip, invalid_listing, 1);
  expect_need("gbk valid 7z listing", gbk_zip, "多人.txt\n", 0);
  expect_need("unicode path extra invalid listing", unicode_extra_zip,
              invalid_listing, 0);

  unlink(gbk_zip);
  unlink(utf8_zip);
  unlink(ascii_zip);
  unlink(unicode_extra_zip);
  free(gbk_zip);
  free(utf8_zip);
  free(ascii_zip);
  free(unicode_extra_zip);
  return 0;
}
