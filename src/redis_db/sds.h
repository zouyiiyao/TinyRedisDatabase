//
// Created by zouyi on 2021/8/14.
//

#ifndef TINY_REDIS_SDS_H
#define TINY_REDIS_SDS_H

/*
 * 最大空间预分配界限: 1MB
 * 如果通过SDS API对SDS进行修改，
 * 当SDS修改后的长度newlen不超过1MB，则Redis分配2 * newlen字节，其中有一半未使用，即free，
 * 当SDS修改后的长度newlen超过1MB，则Redis分配newlen + 1MB字节，其中1MB未使用
 */
#define SDS_MAX_PREALLOC (1024 * 1024)

#include <sys/types.h>
#include <stdarg.h>

typedef char* sds;

struct sdshdr {
    int len;
    int free;
    // C语言技巧: 柔性数组
    // 处于结构体的最后一个字段，本身不占据任何内存空间，代表一个常量偏移
    char buf[];
};

/*
 * sdslen: 返回sds的长度len，不包括最后一个字符'\0'
 *
 * T = O(1)
 */
static inline size_t sdslen(const sds s) {
    struct sdshdr* sh = (void*)(s - (sizeof(struct sdshdr)));
    return sh->len;
}

/*
 * sdsavail: 返回sds剩余可用长度free
 *
 * T = O(1)
 */
static inline size_t sdsavail(const sds s) {
    struct sdshdr* sh = (void*)(s - (sizeof(struct sdshdr)));
    return sh->free;
}

sds sdsnewlen(const void* init, size_t initlen);

/*
 * sdsnew: 用C语言字符串初始化一个sdshdr，返回sds
 */
sds sdsnew(const char *init);

/*
 * sdsempty: 初始化一个sdshdr，保存空字符串，返回sds
 */
sds sdsempty(void);

/*
 * sdsdup: 复制一个sds
 */
sds sdsdup(const sds s);

/*
 * sdsfree: 释放一个sdshdr
 */
void sdsfree(sds s);

sds sdscatlen(sds s, const void* t, size_t len);

/*
 * sdscat: 将一个C语言字符串拼接到原来的sds后，返回新的sds
 * 可能发生内存的释放和重分配，导致传入的sds失效
 *
 * T = O(N)
 */
sds sdscat(sds s, const char* t);

/*
 * sdscatsds: 将一个sds拼接到原来的sds后，返回新的sds
 * 可能发生内存的释放和重分配，导致传入的sds失效
 */
sds sdscatsds(sds s, const sds t);

int sdscmp(const sds s1, const sds s2);

/*
 * sdsclear: 将sds置空
 * 惰性空间释放策略，只是修改len字段和free字段，并不实际释放空间
 */
void sdsclear(sds s);

sds sdsfromlonglong(long long value);

/* Low level functions exposed to the user API */
sds sdsMakeRoomFor(sds s, size_t addlen);
void sdsIncrLen(sds s, int incr);
sds sdsRemoveFreeSpace(sds s);
size_t sdsAllocSize(sds s);

sds sdscatvprintf(sds s, const char* fmt, va_list ap);
sds sdscatprintf(sds s, const char* fmt, ...);
sds sdscatfmt(sds s, char const *fmt, ...);
sds sdscatrepr(sds s, const char *p, size_t len);

void sdsrange(sds s, int start, int end);
sds *sdssplitargs(const char *line, int *argc);

#endif //TINY_REDIS_SDS_H
