#pragma once
#include <stddef.h>

typedef struct {
  char *id;
  char *value;
  char *desc;
  int hit_count;
} PwdItem;

typedef struct {
  PwdItem *data;
  size_t len;
  size_t cap;
} PwdVec;

void pwdvec_init(PwdVec *v);
void pwdvec_free(PwdVec *v);
int pwdvec_push(PwdVec *v, PwdItem it);

char *password_lib_path(void);
void load_password_lib(PwdVec *v);
int save_password_lib(PwdVec *v);
int add_password_to_lib(PwdVec *v, const char *password);
void bump_password_hit(PwdVec *v, const char *password);
