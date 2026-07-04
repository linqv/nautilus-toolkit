# Polyglot ZIP Zero-Copy Extraction Design

## Context

`nautilus-toolkit` currently handles MP4+ZIP polyglot archives after a normal `7z` extraction failure. The fallback finds the embedded ZIP offset, copies bytes from that offset to the end of the file into a temporary `.nautilus-toolkit-polyglot-*` file, then retries extraction through `7z`.

That approach is correct but slow for large disguised MP4 files because it writes a full duplicate of the embedded ZIP payload before extraction can begin.

## Goal

Add a fast path for unencrypted ZIP polyglot files that avoids copying the embedded ZIP payload. The implementation should extract directly from the original file after `zip_start`, while preserving the existing copy-and-`7z` fallback for compatibility.

## Non-Goals

- Do not support password-protected ZIP polyglot archives in the new fast path.
- Do not replace the general `7z` extraction backend.
- Do not hand-write a complete ZIP/Deflate implementation.
- Do not change compression behavior.

## Recommended Approach

Use libarchive as a zero-copy ZIP extraction backend for the polyglot fallback path.

The new backend will open the original polyglot file and expose it to libarchive through custom callbacks:

- `read`: reads from the original file descriptor.
- `seek`: maps libarchive's logical ZIP offset to the real file offset with `real_offset = zip_start + logical_offset`.
- `skip` or equivalent callback support: advances within the logical ZIP stream.
- `close`: closes owned resources.

From libarchive's perspective, the embedded ZIP starts at logical offset `0`, even though the bytes remain inside the original MP4-like file.

## Runtime Flow

1. The existing extraction loop first attempts normal `7z` extraction.
2. If `7z` fails with a non-password error and `polyglot_should_try_after_7z_error()` allows retry, call `polyglot_find_zip_start()`.
3. If `zip_start > 0` and the current attempt is not using a password, call the new libarchive polyglot ZIP extraction backend.
4. If libarchive extraction succeeds, mark the task successful and continue with the existing success bookkeeping.
5. If libarchive reports unsupported format, encrypted content, or another extraction error, fall back to the current `polyglot_make_temp_fixed_zip()` path and retry through `7z`.

## Components

### `src/core/polyglot.c`

Keep ZIP offset detection here. It already provides `polyglot_find_zip_start()`, which is sufficient for the fast path.

### New ZIP Extraction Backend

Add a focused backend in `src/core/polyglot_zip_extract.c` with a matching `src/core/polyglot_zip_extract.h` header.

Suggested public function:

```c
int polyglot_extract_plain_zip(const char *src_path,
                               uint64_t zip_start,
                               const char *outdir,
                               FILE *progress_pipe,
                               double start_pct,
                               double slot_size,
                               StrBuf *out,
                               const char *archive_label,
                               int task_index,
                               int task_total,
                               int *global_progress_floor);
```

Return values should match the existing extraction convention where `0` means success and nonzero means failure.

### `src/ui_gtk/ui_gtk_extract.c`

Change only the existing polyglot retry block. Before copying a temporary fixed ZIP, try the libarchive fast path when no password is active.

The existing temporary-copy retry remains the fallback.

### Build System

Update `CMakeLists.txt` to find and link libarchive for the Nautilus extension and the new tests. The install script already documents `bsdtar(libarchive)` as a runtime dependency, so this aligns the build with current runtime assumptions.

## Safety Requirements

The libarchive backend must prevent archive path traversal:

- Reject absolute paths.
- Reject entries containing `..` path components.
- Reject paths that resolve outside `outdir`.
- Create directories as needed with bounded permissions.
- Avoid following symlinks during extraction when creating regular files.

The backend should treat encrypted ZIP entries as unsupported and return failure so the existing `7z` fallback can handle them.

## Progress And Errors

Progress can be approximate. The backend should emit the same progress pipe style used by existing extraction where practical, based on bytes consumed or entries processed. If precise progress is unavailable, it should still send reset/current-file updates and avoid regressing the UI.

Errors should append readable messages into `StrBuf *out`, so the existing failure reason path can display useful context.

## Tests

Add focused tests for the new backend:

- Construct a small MP4-like prefix plus a normal ZIP payload.
- Verify `polyglot_find_zip_start()` returns the ZIP offset.
- Verify `polyglot_extract_plain_zip()` extracts files without creating a temporary copied ZIP.
- Verify nested directories extract correctly.
- Verify absolute paths and `../` entries are rejected.
- Verify an encrypted ZIP or unsupported method fails fast and can fall back to the old path.

Keep the existing `test_polyglot` retry-policy tests.

## Compatibility

If libarchive cannot open the embedded ZIP, cannot seek, encounters encrypted entries, or fails during extraction, the current behavior remains available by copying the ZIP tail to a temporary file and retrying through `7z`.

This makes the optimization opportunistic rather than a behavior-breaking replacement.
