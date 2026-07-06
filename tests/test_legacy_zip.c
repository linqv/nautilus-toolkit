#include "exec7z.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static void expect_int(const char *label, int got, int expected) {
  if (got != expected) {
    fprintf(stderr, "%s: expected %d got %d\n", label, expected, got);
    exit(1);
  }
}

static char *temp_dir_path(const char *suffix) {
  char tmpl[256];
  snprintf(tmpl, sizeof(tmpl), "/tmp/ntk-legacy-zip-%s-XXXXXX", suffix);
  if (!mkdtemp(tmpl)) {
    perror("mkdtemp");
    exit(2);
  }
  return strdup(tmpl);
}

static char *join_path(const char *dir, const char *name) {
  size_t dir_len = strlen(dir);
  size_t name_len = strlen(name);
  char *path = (char *)malloc(dir_len + 1 + name_len + 1);
  if (!path) {
    perror("malloc");
    exit(2);
  }
  memcpy(path, dir, dir_len);
  path[dir_len] = '/';
  memcpy(path + dir_len + 1, name, name_len + 1);
  return path;
}

static void write_fake_bsdtar(const char *dir) {
  char *path = join_path(dir, "bsdtar");
  FILE *f = fopen(path, "w");
  if (!f) {
    perror(path);
    exit(2);
  }
  fputs("#!/bin/sh\n"
        "printf '%s\\n' \"$@\" > \"$NTK_FAKE_BSDTAR_ARGS\"\n"
        "exit 0\n",
        f);
  fclose(f);
  if (chmod(path, 0755) != 0) {
    perror(path);
    exit(2);
  }
  free(path);
}

static void write_fake_7z_no_files(const char *dir) {
  char *path = join_path(dir, "7z");
  FILE *f = fopen(path, "w");
  if (!f) {
    perror(path);
    exit(2);
  }
  fputs("#!/bin/sh\n"
        "printf '%s\\n' 'No files to process' 'Everything is Ok' 'Files: 0'\n"
        "exit 0\n",
        f);
  fclose(f);
  if (chmod(path, 0755) != 0) {
    perror(path);
    exit(2);
  }
  free(path);
}

static void write_fake_7z_progress(const char *dir) {
  char *path = join_path(dir, "7z");
  FILE *f = fopen(path, "w");
  if (!f) {
    perror(path);
    exit(2);
  }
  fputs("#!/bin/sh\n"
        "printf ' 60%%\\n'\n"
        "exit 0\n",
        f);
  fclose(f);
  if (chmod(path, 0755) != 0) {
    perror(path);
    exit(2);
  }
  free(path);
}

static void write_fake_bsdtar_probe(const char *dir) {
  char *path = join_path(dir, "bsdtar");
  FILE *f = fopen(path, "w");
  if (!f) {
    perror(path);
    exit(2);
  }
  fputs("#!/bin/sh\n"
        "case \" $* \" in\n"
        "  *' -tf '*) printf '%s\\n' '和大乔姐的甜美假期/00111.webp'; exit 0 ;;\n"
        "esac\n"
        "if [ \"$2\" = good ]; then\n"
        "  printf 'RIFF0000WEBP'\n"
        "  exit 0\n"
        "fi\n"
        "printf '%s\\n' '和大乔姐的甜美假期/00111.webp: Incorrect passphrase: Unknown error -1' >&2\n"
        "exit 1\n",
        f);
  fclose(f);
  if (chmod(path, 0755) != 0) {
    perror(path);
    exit(2);
  }
  free(path);
}

static char *read_file_or_die(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    exit(2);
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    perror("fseek");
    exit(2);
  }
  long len = ftell(f);
  if (len < 0) {
    perror("ftell");
    exit(2);
  }
  rewind(f);
  char *buf = (char *)malloc((size_t)len + 1);
  if (!buf) {
    perror("malloc");
    exit(2);
  }
  if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
    perror("fread");
    exit(2);
  }
  buf[len] = 0;
  fclose(f);
  return buf;
}

static void expect_contains(const char *label, const char *haystack,
                            const char *needle) {
  if (!haystack || !strstr(haystack, needle)) {
    fprintf(stderr, "%s: expected to find <%s> in <%s>\n", label, needle,
            haystack ? haystack : "(null)");
    exit(1);
  }
}

static int progress_stream_has_value(FILE *stream, int expected) {
  fflush(stream);
  rewind(stream);

  char line[256];
  while (fgets(line, sizeof(line), stream)) {
    if (line[0] == '#')
      continue;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = 0;
    char *endptr = NULL;
    long value = strtol(line, &endptr, 10);
    if (endptr != line && *endptr == '\0' && value == expected)
      return 1;
  }
  return 0;
}

static char *prepend_temp_path(const char *bindir) {
  const char *old_path = getenv("PATH");
  char *old_path_copy = old_path ? strdup(old_path) : NULL;
  size_t path_len = strlen(bindir) + 1 + (old_path ? strlen(old_path) : 0) + 1;
  char *new_path = (char *)malloc(path_len);
  if (!new_path) {
    perror("malloc");
    exit(2);
  }
  snprintf(new_path, path_len, "%s:%s", bindir, old_path ? old_path : "");
  setenv("PATH", new_path, 1);
  free(new_path);
  return old_path_copy;
}

static void restore_path(char *old_path_copy) {
  if (old_path_copy)
    setenv("PATH", old_path_copy, 1);
  else
    unsetenv("PATH");
  free(old_path_copy);
}

static void test_gbk_extract_passes_password_to_bsdtar(const char *gbk_zip) {
  char *bindir = temp_dir_path("fake-bsdtar-bin");
  char *outdir = temp_dir_path("fake-bsdtar-out");
  char *args_path = join_path(bindir, "args.txt");
  write_fake_bsdtar(bindir);

  char *old_path_copy = prepend_temp_path(bindir);
  setenv("NTK_FAKE_BSDTAR_ARGS", args_path, 1);

  StrBuf out;
  sb_init(&out);
  int rc = run_extract_gbk_zip_for_file(gbk_zip, outdir, "correct horse",
                                        NULL, 0.0, 1.0, &out, "gbk.zip", 1,
                                        1, NULL);
  if (rc != 0) {
    fprintf(stderr, "gbk extract with fake bsdtar: expected 0 got %d\n", rc);
    exit(1);
  }

  char *args = read_file_or_die(args_path);
  expect_contains("gbk bsdtar password flag", args, "--passphrase\n");
  expect_contains("gbk bsdtar password value", args, "\ncorrect horse\n");

  restore_path(old_path_copy);
  unsetenv("NTK_FAKE_BSDTAR_ARGS");

  sb_free(&out);
  free(args);
  char *fake_bsdtar = join_path(bindir, "bsdtar");
  unlink(fake_bsdtar);
  unlink(args_path);
  rmdir(outdir);
  rmdir(bindir);
  free(fake_bsdtar);
  free(args_path);
  free(outdir);
  free(bindir);
}

static void test_7z_probe_rejects_zero_file_success(void) {
  char *bindir = temp_dir_path("fake-7z-bin");
  write_fake_7z_no_files(bindir);
  char *old_path_copy = prepend_temp_path(bindir);

  int rc = run_7z_probe_password_test("archive.zip", "missing.webp", "secret",
                                      NULL, 0);
  if (rc != -1) {
    fprintf(stderr, "7z probe no-files stream: expected -1 got %d\n", rc);
    exit(1);
  }

  rc = run_7z_probe_password_test("archive.zip", "missing.webp", "secret",
                                  NULL, 1);
  if (rc != -1) {
    fprintf(stderr, "7z probe no-files test: expected -1 got %d\n", rc);
    exit(1);
  }

  restore_path(old_path_copy);
  char *fake_7z = join_path(bindir, "7z");
  unlink(fake_7z);
  rmdir(bindir);
  free(fake_7z);
  free(bindir);
}

static void test_7z_progress_uses_raw_percent_mapping(void) {
  char *bindir = temp_dir_path("fake-7z-progress-bin");
  char *outdir = temp_dir_path("fake-7z-progress-out");
  write_fake_7z_progress(bindir);
  char *old_path_copy = prepend_temp_path(bindir);

  FILE *progress = tmpfile();
  if (!progress) {
    perror("tmpfile");
    exit(2);
  }

  StrBuf out;
  sb_init(&out);
  int floor = -1;
  int rc = run_extract_for_file("archive.7z", outdir, "", NULL, progress, 10.0,
                                80.0, &out, "archive.7z", 1, 1, &floor);
  expect_int("7z fake progress extraction", rc, 0);
  if (!progress_stream_has_value(progress, 58)) {
    fprintf(stderr, "7z progress mapping: expected raw mapped progress 58; "
                    "captured output did not contain it\n");
    exit(1);
  }

  fclose(progress);
  sb_free(&out);
  restore_path(old_path_copy);
  char *fake_7z = join_path(bindir, "7z");
  unlink(fake_7z);
  rmdir(outdir);
  rmdir(bindir);
  free(fake_7z);
  free(outdir);
  free(bindir);
}

static void test_bsdtar_gbk_probe_checks_passphrase(void) {
  char *bindir = temp_dir_path("fake-bsdtar-probe-bin");
  write_fake_bsdtar_probe(bindir);
  char *old_path_copy = prepend_temp_path(bindir);

  int rc = run_bsdtar_probe_password_for_file("archive.zip", "bad", NULL);
  if (rc != 0) {
    fprintf(stderr, "bsdtar gbk probe wrong password: expected 0 got %d\n",
            rc);
    exit(1);
  }

  rc = run_bsdtar_probe_password_for_file("archive.zip", "good", NULL);
  if (rc != 1) {
    fprintf(stderr, "bsdtar gbk probe correct password: expected 1 got %d\n",
            rc);
    exit(1);
  }

  restore_path(old_path_copy);
  char *fake_bsdtar = join_path(bindir, "bsdtar");
  unlink(fake_bsdtar);
  rmdir(bindir);
  free(fake_bsdtar);
  free(bindir);
}

static void test_plain_crc_failure_does_not_request_password_retry(void) {
  const char *crc_failure =
      "ERROR: CRC Failed : 01/01_001.png\n"
      "Sub items Errors: 1\n";
  expect_int("plain CRC failure is not a password retry",
             extraction_error_may_need_password(crc_failure, 0), 0);
  expect_int("explicit password prompt is a password retry",
             extraction_error_may_need_password("Enter password", 0), 1);
  expect_int("CRC after trying a password remains password-related",
             extraction_error_may_need_password(crc_failure, 1), 1);
}

static void test_non_password_failure_preserves_partial_output(void) {
  expect_int("password failures clean generated output",
             extract_failure_should_cleanup_output(0, 0), 1);
  expect_int("cancelled extraction cleans generated output",
             extract_failure_should_cleanup_output(1, 1), 1);
  expect_int("non-password failures preserve generated output",
             extract_failure_should_cleanup_output(1, 0), 0);
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
  char *encrypted_gbk_zip = temp_zip_path("encrypted-gbk");
  char *utf8_zip = temp_zip_path("utf8");
  char *ascii_zip = temp_zip_path("ascii");
  char *unicode_extra_zip = temp_zip_path("unicode-extra");

  write_zip_with_name(gbk_zip, gbk_name, sizeof(gbk_name), 0, NULL, 0);
  write_zip_with_name(encrypted_gbk_zip, gbk_name, sizeof(gbk_name), 1, NULL,
                      0);
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
  expect_detect("encrypted gbk legacy filename", encrypted_gbk_zip, 1);
  expect_int("encrypted gbk waits for password",
             archive_needs_legacy_gbk_password_before_extract(
                 encrypted_gbk_zip, invalid_listing),
             1);
  expect_int("plain archive does not wait for password",
             archive_needs_password_before_extract(gbk_zip), 0);
  expect_int("encrypted archive waits for password",
             archive_needs_password_before_extract(encrypted_gbk_zip), 1);
  test_gbk_extract_passes_password_to_bsdtar(gbk_zip);
  test_7z_probe_rejects_zero_file_success();
  test_7z_progress_uses_raw_percent_mapping();
  test_bsdtar_gbk_probe_checks_passphrase();
  test_plain_crc_failure_does_not_request_password_retry();
  test_non_password_failure_preserves_partial_output();

  unlink(gbk_zip);
  unlink(encrypted_gbk_zip);
  unlink(utf8_zip);
  unlink(ascii_zip);
  unlink(unicode_extra_zip);
  free(gbk_zip);
  free(encrypted_gbk_zip);
  free(utf8_zip);
  free(ascii_zip);
  free(unicode_extra_zip);
  return 0;
}
