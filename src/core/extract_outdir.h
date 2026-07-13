#pragma once

int prepare_extract_outdir(char **outdir, const char *parent,
                           const char *custom_dest,
                           int reuse_existing_created,
                           int *created_by_us, int *existed_before);
