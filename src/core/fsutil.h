#pragma once
int remove_tree(const char *path, const char *allowed_root);
int normalize_tree_utf8_names(const char *root, const char *locale,
                              int rename_root);
