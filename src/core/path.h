#pragma once

char *path_parent(const char *path);
char *path_filename(const char *path);
char *path_stem(const char *path);
int path_exists(const char *path);
int mkdirs(const char *path);
