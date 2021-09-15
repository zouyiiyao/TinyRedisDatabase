//
// Created by zouyi on 2021/8/14.
//

#include <stdio.h>
#include <string.h>
#include "sds.h"
#include "zmalloc.h"

/*
 * sdsnewlen: 通过C字符串创建一个sdshdr
 */
sds sdsnewlen(const void* init, size_t initlen) {

    struct sdshdr* sh;

    if (init) {
        // init指针不为空时，调用zmalloc
        // 只分配空间，不初始化
        sh = zmalloc(sizeof(struct sdshdr) + initlen + 1);
    } else {
        // init指针为空时，调用zcalloc
        // 分配空间，并初始化为0
        sh = zcalloc(sizeof(struct sdshdr) + initlen + 1);
    }

    if (sh == NULL) return NULL;

    sh->len = initlen;
    sh->free = 0;
    // memcpy: 将init内容复制到sh->buf
    // T = O(N)
    if (initlen && init)
        memcpy(sh->buf, init, initlen);
    sh->buf[initlen] = '\0';

    return (char*)sh->buf;
}
/*
 * 拓展: memcpy和memmove的区别
 * memcpy: 只执行复制，不判断重叠的情况
 * memmove: 执行复制，并判断重叠的情况，如果重叠，则改为从高地址往低地址顺序复制
 */

/*
 * sdsempty: 调用sdsnewlen完成实际的任务
 */
sds sdsempty(void) {
    return sdsnewlen("", 0);
}

/*
 * sdsnew: 调用sdsnewlen完成实际的任务
 */
sds sdsnew(const char* init) {
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

void sdsfree(sds s) {
    if (s == NULL) return;
    zfree(s - sizeof(struct sdshdr));
}

void sdsclear(sds s) {

    struct sdshdr* sh = (void*)(s - sizeof(struct sdshdr));

    sh->free += sh->len;
    sh->len = 0;

    sh->buf[0] = '\0';
}

/*
 * sdsMakeRoomFor: 当对sds进行空间扩展时调用，采用空间预分配策略
 */
sds sdsMakeRoomFor(sds s, size_t addlen) {

    struct sdshdr* sh;
    struct sdshdr* newsh;

    size_t free = sdsavail(s);

    size_t len, newlen;

    // 如果剩下的空间能够满足需求，直接返回
    if (free >= addlen) return s;

    len = sdslen(s);
    sh = (void*)(s - (sizeof(struct sdshdr)));

    newlen = (len + addlen);

    if (newlen < SDS_MAX_PREALLOC)
        // 新的长度小于SDS_MAX_PREALLOC，分配两倍空间
        newlen *= 2;
    else
        // 新的长度不小于SDS_MAX_PREALLOC，多分配SDS_MAX_PREALLOC
        newlen += SDS_MAX_PREALLOC;

    /* zrealloc: 执行实际的虚拟内存空间申请
     * 先判断当前的指针后是否有足够的连续空间，如果有，扩大mem_address指向的地址，并且将mem_address返回，
     * 如果空间不够，先按照newsize指定的大小分配空间，将原有数据从头到尾拷贝到新分配的内存区域，而后释放原来mem_address所指内存区域
     * （注意：原来指针是自动释放，不需要使用free），同时返回新分配的内存区域的首地址。即重新分配存储器块的地址。
     */
    newsh = zrealloc(sh, sizeof(struct sdshdr) + newlen + 1);

    if (newsh == NULL) return NULL;

    newsh->free = newlen - len;

    return newsh->buf;
}

/*
 * sdscatlen: 字符串拼接
 */
sds sdscatlen(sds s, const void* t, size_t len) {

    struct sdshdr* sh;

    size_t curlen = sdslen(s);

    // 杜绝缓冲区溢出
    // 调用sdsMakeRoomFor保证足量空间
    s = sdsMakeRoomFor(s, len);

    if (s == NULL) return NULL;

    // 将新增加的字符串复制到之前字符串后面
    // T = O(N)
    sh = (void*)(s - sizeof(struct sdshdr));
    memcpy(s + curlen, t, len);

    sh->len = curlen + len;
    sh->free = sh->free - len;

    // 最后添加一个'\0'，兼容部分C字符串函数
    s[curlen + len] = '\0';

    return s;
}

/*
 * sdscat: 将一个C语言字符串拼接到原sds后，调用sdscatlen完成实际的工作
 */
sds sdscat(sds s, const char* t) {
    return sdscatlen(s, t, strlen(t));
}

/*
 * sdscatsds: 将一个sds拼接到原sds后，调用sdscatlen完成实际的工作
 */
sds sdscatsds(sds s, const sds t) {
    return sdscatlen(s, t, sdslen(t));
}

/* Helper for sdscatlonglong() doing the actual number -> string
 * conversion. 's' must point to a string with room for at least
 * SDS_LLSTR_SIZE bytes.
 *
 * The function returns the lenght of the null-terminated string
 * representation stored at 's'. */
#define SDS_LLSTR_SIZE 21
int sdsll2str(char *s, long long value) {
    char *p, aux;
    unsigned long long v;
    size_t l;

    /* Generate the string representation, this method produces
     * an reversed string. */
    v = (value < 0) ? -value : value;
    p = s;
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);
    if (value < 0) *p++ = '-';

    /* Compute length and add null term. */
    l = p-s;
    *p = '\0';

    /* Reverse the string. */
    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
    return l;
}

sds sdsfromlonglong(long long value) {
    char buf[SDS_LLSTR_SIZE];
    int len = sdsll2str(buf, value);

    return sdsnewlen(buf, len);
}