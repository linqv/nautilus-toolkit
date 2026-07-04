#define _GNU_SOURCE
#include "polyglot.h"
#include "path.h"
#include "strbuf.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t read_be32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t read_be64(const unsigned char *p) {
  return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
         ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
         ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
         ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

static uint32_t read_le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const unsigned char *p) {
  return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint64_t read_le64(const unsigned char *p) {
  return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
         ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
         ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
         ((uint64_t)p[7] << 56);
}

static int has_mp4_atom_type(const unsigned char *type4) {
  static const char known[][4] = {
      {'f', 't', 'y', 'p'}, {'m', 'o', 'o', 'v'}, {'m', 'd', 'a', 't'},
      {'f', 'r', 'e', 'e'}, {'s', 'k', 'i', 'p'}, {'w', 'i', 'd', 'e'},
      {'p', 'n', 'o', 't'}, {'m', 'o', 'o', 'f'}, {'s', 'i', 'd', 'x'},
      {'s', 't', 'y', 'p'}, {'p', 'd', 'i', 'n'}, {'m', 'e', 't', 'a'},
      {'u', 'u', 'i', 'd'}};
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
    if (memcmp(type4, known[i], 4) == 0)
      return 1;
  }
  return 0;
}

static int read_file_size(const char *filepath, uint64_t *size_out) {
  if (!filepath || !size_out)
    return -1;
  struct stat st;
  if (stat(filepath, &st) != 0)
    return -1;
  if (st.st_size < 0)
    return -1;
  *size_out = (uint64_t)st.st_size;
  return 0;
}

static int seek_read(FILE *f, uint64_t pos, unsigned char *buf, size_t n) {
  if (!f || !buf)
    return -1;
  if (fseeko(f, (off_t)pos, SEEK_SET) != 0)
    return -1;
  return fread(buf, 1, n, f) == n ? 0 : -1;
}

static int zip_method_looks_plausible(uint16_t method) {
  return method == 0 || method == 1 || method == 6 || method == 8 ||
         method == 9 || method == 12 || method == 14 || method == 93 ||
         method == 95 || method == 98;
}

static int zip_local_header_valid(FILE *f, uint64_t pos, uint64_t file_size) {
  static const unsigned char sig_local[4] = {'P', 'K', 0x03, 0x04};
  unsigned char header[30];
  if (!f || pos > file_size || file_size - pos < sizeof(header))
    return 0;
  if (seek_read(f, pos, header, sizeof(header)) != 0)
    return 0;
  if (memcmp(header, sig_local, sizeof(sig_local)) != 0)
    return 0;

  uint16_t version_needed = read_le16(header + 4);
  uint16_t flags = read_le16(header + 6);
  uint16_t method = read_le16(header + 8);
  uint32_t compressed_size = read_le32(header + 18);
  uint16_t name_len = read_le16(header + 26);
  uint16_t extra_len = read_le16(header + 28);
  if (version_needed < 10 || version_needed > 63)
    return 0;
  if (!zip_method_looks_plausible(method))
    return 0;
  if (name_len == 0 || name_len > 4096)
    return 0;

  uint64_t data_start = pos + sizeof(header);
  if (data_start < pos || name_len > file_size - data_start)
    return 0;
  data_start += name_len;
  if (extra_len > file_size - data_start)
    return 0;
  data_start += extra_len;
  if ((flags & 0x0008u) == 0 && compressed_size > file_size - data_start)
    return 0;
  return 1;
}

static int read_rar5_vint(const unsigned char *buf, size_t len, uint64_t *value,
                          size_t *used) {
  if (!buf || !value || !used)
    return 0;
  uint64_t result = 0;
  unsigned shift = 0;
  for (size_t i = 0; i < len && i < 10; i++) {
    unsigned char b = buf[i];
    if (shift >= 64 && (b & 0x7f) != 0)
      return 0;
    result |= ((uint64_t)(b & 0x7f)) << shift;
    if ((b & 0x80) == 0) {
      *value = result;
      *used = i + 1;
      return 1;
    }
    shift += 7;
  }
  return 0;
}

static int rar5_header_valid(FILE *f, uint64_t pos, uint64_t file_size) {
  static const unsigned char sig_rar5[] = {'R', 'a', 'r', '!', 0x1a,
                                           0x07, 0x01, 0x00};
  unsigned char header[64];
  if (!f || pos > file_size || file_size - pos < 15)
    return 0;
  size_t to_read = sizeof(header);
  if (file_size - pos < (uint64_t)to_read)
    to_read = (size_t)(file_size - pos);
  if (seek_read(f, pos, header, to_read) != 0)
    return 0;
  if (memcmp(header, sig_rar5, sizeof(sig_rar5)) != 0)
    return 0;

  size_t off = sizeof(sig_rar5) + 4; /* signature + header CRC */
  uint64_t header_size = 0;
  size_t used_size = 0;
  if (!read_rar5_vint(header + off, to_read - off, &header_size, &used_size))
    return 0;
  off += used_size;
  if (header_size < 2 || header_size > (1U << 20))
    return 0;
  uint64_t header_data_start = pos + sizeof(sig_rar5) + 4 + used_size;
  if (header_data_start < pos || header_size > file_size - header_data_start)
    return 0;

  uint64_t header_type = 0;
  size_t used_type = 0;
  if (!read_rar5_vint(header + off, to_read - off, &header_type, &used_type))
    return 0;
  off += used_type;
  uint64_t header_flags = 0;
  size_t used_flags = 0;
  if (!read_rar5_vint(header + off, to_read - off, &header_flags, &used_flags))
    return 0;
  (void)header_flags;

  if (header_type < 1 || header_type > 5)
    return 0;
  if ((uint64_t)(used_type + used_flags) > header_size)
    return 0;
  return 1;
}

static int supported_archive_header_valid(FILE *f, uint64_t pos,
                                          uint64_t file_size) {
  return zip_local_header_valid(f, pos, file_size) ||
         rar5_header_valid(f, pos, file_size);
}

static int parse_mp4_atoms(FILE *f, uint64_t file_size, uint64_t *video_end) {
  if (!f || !video_end)
    return -1;
  uint64_t pos = 0;
  while (pos + 8 <= file_size) {
    unsigned char header[16];
    if (seek_read(f, pos, header, 8) != 0)
      break;

    uint64_t atom_size = (uint64_t)read_be32(header);
    const unsigned char *atom_type = header + 4;
    if (atom_size == 1) {
      if (seek_read(f, pos + 8, header + 8, 8) != 0)
        break;
      atom_size = read_be64(header + 8);
    } else if (atom_size == 0) {
      break;
    }

    if (atom_size < 8 || atom_size > file_size - pos ||
        !has_mp4_atom_type(atom_type)) {
      break;
    }
    pos += atom_size;
  }
  *video_end = pos;
  return 0;
}

static int find_last_sig(const unsigned char *buf, size_t len,
                         const unsigned char sig[4], size_t *idx_out) {
  if (!buf || len < 4 || !idx_out)
    return 0;
  for (size_t i = len - 4 + 1; i-- > 0;) {
    if (buf[i] == sig[0] && buf[i + 1] == sig[1] && buf[i + 2] == sig[2] &&
        buf[i + 3] == sig[3]) {
      *idx_out = i;
      return 1;
    }
  }
  return 0;
}

static int find_zip_start_from_eocd(FILE *f, uint64_t file_size,
                                    uint64_t *zip_start_out) {
  if (!f || !zip_start_out)
    return 0;
  const unsigned char sig_eocd[4] = {'P', 'K', 0x05, 0x06};
  const unsigned char sig_zip64[4] = {'P', 'K', 0x06, 0x06};
  const unsigned char sig_cd[4] = {'P', 'K', 0x01, 0x02};

  size_t search_size = (file_size < 65536ULL) ? (size_t)file_size : 65536U;
  if (search_size < 22)
    return 0;

  unsigned char *tail = (unsigned char *)malloc(search_size);
  if (!tail)
    return 0;
  uint64_t tail_abs = file_size - (uint64_t)search_size;
  if (seek_read(f, tail_abs, tail, search_size) != 0) {
    free(tail);
    return 0;
  }

  size_t eocd_idx = 0;
  if (!find_last_sig(tail, search_size, sig_eocd, &eocd_idx)) {
    free(tail);
    return 0;
  }

  uint64_t cd_offset_recorded = 0;
  size_t z64_idx = 0;
  if (find_last_sig(tail, search_size, sig_zip64, &z64_idx) &&
      search_size - z64_idx >= 56) {
    cd_offset_recorded = read_le64(tail + z64_idx + 48);
  } else {
    if (search_size - eocd_idx < 22) {
      free(tail);
      return 0;
    }
    cd_offset_recorded = (uint64_t)read_le32(tail + eocd_idx + 16);
    if (cd_offset_recorded == 0xFFFFFFFFULL) {
      free(tail);
      return 0;
    }
  }

  size_t cd_idx = 0;
  if (!find_last_sig(tail, search_size, sig_cd, &cd_idx)) {
    free(tail);
    return 0;
  }
  uint64_t cd_abs = tail_abs + (uint64_t)cd_idx;
  if (cd_abs < cd_offset_recorded) {
    free(tail);
    return 0;
  }

  uint64_t zip_start = cd_abs - cd_offset_recorded;
  if (zip_start >= file_size) {
    free(tail);
    return 0;
  }

  if (zip_local_header_valid(f, zip_start, file_size)) {
    *zip_start_out = zip_start;
    free(tail);
    return 1;
  }

  free(tail);
  return 0;
}

static int find_zip_start_scan(FILE *f, uint64_t file_size,
                               uint64_t *zip_start_out) {
  if (!f || !zip_start_out)
    return 0;
  const unsigned char sig_local[4] = {'P', 'K', 0x03, 0x04};
  const size_t chunk_size = 8U * 1024U * 1024U;
  unsigned char *buf = (unsigned char *)malloc(chunk_size + 3);
  if (!buf)
    return 0;

  uint64_t pos = 0;
  size_t carry = 0;
  while (pos < file_size) {
    size_t to_read = chunk_size;
    if (file_size - pos < (uint64_t)to_read)
      to_read = (size_t)(file_size - pos);

    if (fseeko(f, (off_t)pos, SEEK_SET) != 0)
      break;
    size_t n = fread(buf + carry, 1, to_read, f);
    if (n == 0)
      break;

    size_t total = carry + n;
    if (total >= 4) {
      for (size_t i = 0; i + 4 <= total; i++) {
        if (memcmp(buf + i, sig_local, 4) == 0) {
          uint64_t abs = (pos - (uint64_t)carry) + (uint64_t)i;
          if (abs < file_size && zip_local_header_valid(f, abs, file_size)) {
            *zip_start_out = abs;
            free(buf);
            return 1;
          }
        }
      }
    }

    carry = (total >= 3) ? 3 : total;
    if (carry > 0)
      memmove(buf, buf + (total - carry), carry);
    pos += (uint64_t)n;
  }

  free(buf);
  return 0;
}

static int find_archive_start_scan(FILE *f, uint64_t file_size,
                                   uint64_t *archive_start_out) {
  if (!f || !archive_start_out)
    return 0;
  const unsigned char sig_local[4] = {'P', 'K', 0x03, 0x04};
  const unsigned char sig_rar5[8] = {'R', 'a', 'r', '!', 0x1a,
                                     0x07, 0x01, 0x00};
  const size_t chunk_size = 8U * 1024U * 1024U;
  unsigned char *buf = (unsigned char *)malloc(chunk_size + 7);
  if (!buf)
    return 0;

  uint64_t pos = 0;
  size_t carry = 0;
  while (pos < file_size) {
    size_t to_read = chunk_size;
    if (file_size - pos < (uint64_t)to_read)
      to_read = (size_t)(file_size - pos);

    if (fseeko(f, (off_t)pos, SEEK_SET) != 0)
      break;
    size_t n = fread(buf + carry, 1, to_read, f);
    if (n == 0)
      break;

    size_t total = carry + n;
    for (size_t i = 0; i < total; i++) {
      int sig_match =
          (i + sizeof(sig_local) <= total &&
           memcmp(buf + i, sig_local, sizeof(sig_local)) == 0) ||
          (i + sizeof(sig_rar5) <= total &&
           memcmp(buf + i, sig_rar5, sizeof(sig_rar5)) == 0);
      if (sig_match) {
        uint64_t abs = (pos - (uint64_t)carry) + (uint64_t)i;
        if (abs < file_size &&
            supported_archive_header_valid(f, abs, file_size)) {
          *archive_start_out = abs;
          free(buf);
          return 1;
        }
      }
    }

    carry = (total >= 7) ? 7 : total;
    if (carry > 0)
      memmove(buf, buf + (total - carry), carry);
    pos += (uint64_t)n;
  }

  free(buf);
  return 0;
}

int polyglot_find_zip_start(const char *filepath, uint64_t *zip_start) {
  if (!filepath || !zip_start)
    return -1;
  *zip_start = 0;

  uint64_t file_size = 0;
  if (read_file_size(filepath, &file_size) != 0 || file_size < 4)
    return 0;

  FILE *f = fopen(filepath, "rb");
  if (!f)
    return -1;

  const unsigned char sig_local[4] = {'P', 'K', 0x03, 0x04};
  unsigned char head[8] = {0};
  if (seek_read(f, 0, head, 4) == 0 && memcmp(head, sig_local, 4) == 0 &&
      zip_local_header_valid(f, 0, file_size)) {
    *zip_start = 0;
    fclose(f);
    return 1;
  }

  if (seek_read(f, 0, head, 8) == 0 && memcmp(head + 4, "ftyp", 4) == 0) {
    uint64_t video_end = 0;
    if (parse_mp4_atoms(f, file_size, &video_end) == 0 && video_end > 0 &&
        video_end < file_size) {
      if (zip_local_header_valid(f, video_end, file_size)) {
        *zip_start = video_end;
        fclose(f);
        return 1;
      }
    }
  }

  if (find_zip_start_from_eocd(f, file_size, zip_start)) {
    fclose(f);
    return 1;
  }

  if (find_zip_start_scan(f, file_size, zip_start)) {
    fclose(f);
    return 1;
  }

  fclose(f);
  return 0;
}

int polyglot_find_archive_start(const char *filepath, uint64_t *archive_start) {
  if (!filepath || !archive_start)
    return -1;
  *archive_start = 0;

  int zip_rc = polyglot_find_zip_start(filepath, archive_start);
  if (zip_rc != 0)
    return zip_rc;

  uint64_t file_size = 0;
  if (read_file_size(filepath, &file_size) != 0 || file_size < 4)
    return 0;

  FILE *f = fopen(filepath, "rb");
  if (!f)
    return -1;

  if (rar5_header_valid(f, 0, file_size)) {
    *archive_start = 0;
    fclose(f);
    return 1;
  }

  unsigned char head[8] = {0};
  if (seek_read(f, 0, head, 8) == 0 && memcmp(head + 4, "ftyp", 4) == 0) {
    uint64_t video_end = 0;
    if (parse_mp4_atoms(f, file_size, &video_end) == 0 && video_end > 0 &&
        video_end < file_size &&
        supported_archive_header_valid(f, video_end, file_size)) {
      *archive_start = video_end;
      fclose(f);
      return 1;
    }
  }

  if (find_archive_start_scan(f, file_size, archive_start)) {
    fclose(f);
    return 1;
  }

  fclose(f);
  return 0;
}

int polyglot_copy_tail(const char *src_path, uint64_t zip_start,
                       const char *dst_path) {
  if (!src_path || !dst_path)
    return -1;

  uint64_t file_size = 0;
  if (read_file_size(src_path, &file_size) != 0)
    return -1;
  if (zip_start >= file_size)
    return -1;

  FILE *fin = fopen(src_path, "rb");
  if (!fin)
    return -1;
  FILE *fout = fopen(dst_path, "wb");
  if (!fout) {
    fclose(fin);
    return -1;
  }

  if (fseeko(fin, (off_t)zip_start, SEEK_SET) != 0) {
    fclose(fin);
    fclose(fout);
    return -1;
  }

  uint64_t remaining = file_size - zip_start;
  const size_t buf_size = 8U * 1024U * 1024U;
  unsigned char *buf = (unsigned char *)malloc(buf_size);
  if (!buf) {
    fclose(fin);
    fclose(fout);
    return -1;
  }

  int ok = 1;
  while (remaining > 0) {
    size_t nread = buf_size;
    if (remaining < (uint64_t)nread)
      nread = (size_t)remaining;
    size_t got = fread(buf, 1, nread, fin);
    if (got == 0) {
      ok = 0;
      break;
    }
    size_t off = 0;
    while (off < got) {
      size_t wr = fwrite(buf + off, 1, got - off, fout);
      if (wr == 0) {
        ok = 0;
        break;
      }
      off += wr;
    }
    if (!ok)
      break;
    remaining -= (uint64_t)got;
  }

  free(buf);
  if (fclose(fin) != 0)
    ok = 0;
  if (fclose(fout) != 0)
    ok = 0;
  return ok ? 0 : -1;
}

static char *make_temp_path(const char *src_path) {
  if (!src_path)
    return NULL;
  char *parent = path_parent(src_path);
  if (!parent)
    return NULL;

  StrBuf sb;
  sb_init(&sb);
  static const char suffix[] = ".nautilus-toolkit-polyglot-XXXXXX";
  int ok = sb_append(&sb, parent, strlen(parent)) && sb_append_c(&sb, '/') &&
           sb_append(&sb, suffix, strlen(suffix));
  free(parent);
  if (!ok || !sb.data) {
    sb_free(&sb);
    return NULL;
  }

  int fd = mkstemp(sb.data);
  if (fd < 0) {
    sb_free(&sb);
    return NULL;
  }
  close(fd);
  return sb.data;
}

int polyglot_make_temp_fixed_zip(const char *src_path, char **out_temp_path,
                                 uint64_t *out_zip_start) {
  if (!src_path || !out_temp_path)
    return -1;
  *out_temp_path = NULL;
  if (out_zip_start)
    *out_zip_start = 0;

  uint64_t zip_start = 0;
  int find_rc = polyglot_find_zip_start(src_path, &zip_start);
  if (find_rc <= 0)
    return find_rc;
  if (zip_start == 0)
    return 0;

  char *tmp_path = make_temp_path(src_path);
  if (!tmp_path)
    return -1;
  if (polyglot_copy_tail(src_path, zip_start, tmp_path) != 0) {
    unlink(tmp_path);
    free(tmp_path);
    return -1;
  }

  *out_temp_path = tmp_path;
  if (out_zip_start)
    *out_zip_start = zip_start;
  return 1;
}

int polyglot_make_temp_fixed_archive(const char *src_path, char **out_temp_path,
                                     uint64_t *out_archive_start) {
  if (!src_path || !out_temp_path)
    return -1;
  *out_temp_path = NULL;
  if (out_archive_start)
    *out_archive_start = 0;

  uint64_t archive_start = 0;
  int find_rc = polyglot_find_archive_start(src_path, &archive_start);
  if (find_rc <= 0)
    return find_rc;
  if (archive_start == 0)
    return 0;

  char *tmp_path = make_temp_path(src_path);
  if (!tmp_path)
    return -1;
  if (polyglot_copy_tail(src_path, archive_start, tmp_path) != 0) {
    unlink(tmp_path);
    free(tmp_path);
    return -1;
  }

  *out_temp_path = tmp_path;
  if (out_archive_start)
    *out_archive_start = archive_start;
  return 1;
}

void polyglot_cleanup_temp(char **temp_path) {
  if (!temp_path || !*temp_path)
    return;
  unlink(*temp_path);
  free(*temp_path);
  *temp_path = NULL;
}

int polyglot_should_try_after_7z_error(const char *error_output) {
  if (!error_output || !*error_output)
    return 1;

  static const char *non_retryable[] = {
      "No space left on device",
      "not enough space",
      "Permission denied",
      "Access is denied",
      "No such file or directory",
      "Headers Error",
      "Unexpected end of data",
      "Unexpected end of archive",
  };

  for (size_t i = 0; i < sizeof(non_retryable) / sizeof(non_retryable[0]);
       i++) {
    if (strstr(error_output, non_retryable[i]))
      return 0;
  }

  return 1;
}
