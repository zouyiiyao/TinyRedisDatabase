//
// Created by zouyi on 2021/9/1.
//

#ifndef TINYREDIS_UTILS_H
#define TINYREDIS_UTILS_H

#include "sds.h"

int stringmatchlen(const char* p, int plen, const char* s, int slen, int nocase);
int stringmatch(const char* p, const char* s, int nocase);
int ll2string(char* s, size_t len, long long value);
int string2ll(const char* s, size_t slen, long long* value);
int string2l(const char *s, size_t slen, long *lval);
int d2string(char* buf, size_t len, double value);

#endif //TINYREDIS_UTILS_H
