//
// Created by zouyi on 2021/8/14.
//

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include "sds.h"
#include "zmalloc.h"

/*
 * sdsnewlen: 通过C字符串/sds创建一个sdshdr
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

/*
 * 创建一个sds的副本
 */
sds sdsdup(const sds s) {
    return sdsnewlen(s, sdslen(s));
}

/*
 * 释放一个sds(sdshdr结构)
 */
void sdsfree(sds s) {
    if (s == NULL) return;
    zfree(s - sizeof(struct sdshdr));
}

/*
 * sdsclear: 将sds置空
 * 惰性空间释放策略，只是修改len字段和free字段，并不实际释放空间
 */
void sdsclear(sds s) {

    struct sdshdr* sh = (void*)(s - sizeof(struct sdshdr));

    sh->free += sh->len;
    sh->len = 0;

    sh->buf[0] = '\0';
}

/*
 * sdsMakeRoomFor: 当对sds进行空间扩展时调用，采用空间预分配策略，addlen参数指示了需要增加的长度
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
 * sdsRemoveFreeSpace: 释放sds多余的空间
 */
sds sdsRemoveFreeSpace(sds s) {
    struct sdshdr* sh;

    sh = (void*)(s - sizeof(struct sdshdr));
    sh = zrealloc(sh, sizeof(struct sdshdr) + sh->len + 1);
    sh->free = 0;
    return sh->buf;
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

/*
 * 将long long转C字符串
 */
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

/*
 * 将unsigned long long转C字符串
 */
/* Identical sdsll2str(), but for unsigned long long type. */
int sdsull2str(char *s, unsigned long long v) {
    char *p, aux;
    size_t l;

    /* Generate the string representation, this method produces
     * an reversed string. */
    p = s;
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);

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

/*
 * 比较两个sds，调用memcmp逐字节比对
 */
/* Compare two sds strings s1 and s2 with memcmp().
 *
 * Return value:
 *
 *     1 if s1 > s2.
 *    -1 if s1 < s2.
 *     0 if s1 and s2 are exactly the same binary string.
 *
 * If two strings share exactly the same prefix, but one of the two has
 * additional characters, the longer string is considered to be greater than
 * the smaller one. */
int sdscmp(const sds s1, const sds s2) {
    size_t l1, l2, minlen;
    int cmp;

    l1 = sdslen(s1);
    l2 = sdslen(s2);
    minlen = (l1 < l2) ? l1 : l2;
    cmp = memcmp(s1, s2, minlen);
    if (cmp == 0) return l1 - l2;
    return cmp;
}

/*
 * 根据long long的值创建一个sds
 */
sds sdsfromlonglong(long long value) {
    char buf[SDS_LLSTR_SIZE];
    int len = sdsll2str(buf, value);

    return sdsnewlen(buf, len);
}

/*
 * 返回给定 sds 分配的内存字节数
 *
 * 复杂度
 *  T = O(1)
 */
/* Return the total size of the allocation of the specifed sds string,
 * including:
 * 1) The sds header before the pointer.
 * 2) The string.
 * 3) The free buffer at the end if any.
 * 4) The implicit null term.
 */
size_t sdsAllocSize(sds s) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));

    return sizeof(*sh)+sh->len+sh->free+1;
}

/* Increment the sds length and decrements the left free space at the
 * end of the string according to 'incr'. Also set the null term
 * in the new end of the string.
 *
 * 根据 incr 参数，增加 sds 的长度，缩减空余空间，
 * 并将 \0 放到新字符串的尾端
 *
 * This function is used in order to fix the string length after the
 * user calls sdsMakeRoomFor(), writes something after the end of
 * the current string, and finally needs to set the new length.
 *
 * 这个函数是在调用 sdsMakeRoomFor() 对字符串进行扩展，
 * 然后用户在字符串尾部写入了某些内容之后，
 * 用来正确更新 free 和 len 属性的。
 *
 * Note: it is possible to use a negative increment in order to
 * right-trim the string.
 *
 * 如果 incr 参数为负数，那么对字符串进行右截断操作。
 *
 * Usage example:
 *
 * Using sdsIncrLen() and sdsMakeRoomFor() it is possible to mount the
 * following schema, to cat bytes coming from the kernel to the end of an
 * sds string without copying into an intermediate buffer:
 *
 * 以下是 sdsIncrLen 的用例：
 *
 * oldlen = sdslen(s);
 * s = sdsMakeRoomFor(s, BUFFER_SIZE);
 * nread = read(fd, s+oldlen, BUFFER_SIZE);
 * ... check for nread <= 0 and handle it ...
 * sdsIncrLen(s, nread);
 *
 * 复杂度
 *  T = O(1)
 */
void sdsIncrLen(sds s, int incr) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));

    // 确保 sds 空间足够
    assert(sh->free >= incr);

    // 更新属性
    sh->len += incr;
    sh->free -= incr;

    // 这个 assert 其实可以忽略
    // 因为前一个 assert 已经确保 sh->free - incr >= 0 了
    assert(sh->free >= 0);

    // 放置新的结尾符号
    s[sh->len] = '\0';
}

/*
 * 打印函数，被 sdscatprintf 所调用
 */
/* Like sdscatprintf() but gets va_list instead of being variadic. */
sds sdscatvprintf(sds s, const char *fmt, va_list ap) {
    va_list cpy;
    char staticbuf[1024], *buf = staticbuf, *t;
    size_t buflen = strlen(fmt)*2;

    /* We try to start using a static buffer for speed.
     * If not possible we revert to heap allocation. */
    if (buflen > sizeof(staticbuf)) {
        buf = zmalloc(buflen);
        if (buf == NULL) return NULL;
    } else {
        buflen = sizeof(staticbuf);
    }

    /* Try with buffers two times bigger every time we fail to
     * fit the string in the current buffer size. */
    while(1) {
        buf[buflen-2] = '\0';
        va_copy(cpy,ap);
        vsnprintf(buf, buflen, fmt, cpy);
        if (buf[buflen-2] != '\0') {
            if (buf != staticbuf) zfree(buf);
            buflen *= 2;
            buf = zmalloc(buflen);
            if (buf == NULL) return NULL;
            continue;
        }
        break;
    }

    /* Finally concat the obtained string to the SDS string and return it. */
    t = sdscat(s, buf);
    if (buf != staticbuf) zfree(buf);
    return t;
}

/*
 * 打印任意数量个字符串，并将这些字符串追加到给定 sds 的末尾
 */
/* Append to the sds string 's' a string obtained using printf-alike format
 * specifier.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = sdsempty("Sum is: ");
 * s = sdscatprintf(s,"%d+%d = %d",a,b,a+b).
 *
 * Often you need to create a string from scratch with the printf-alike
 * format. When this is the need, just use sdsempty() as the target string:
 *
 * s = sdscatprintf(sdsempty(), "... your format ...", args);
 */
sds sdscatprintf(sds s, const char *fmt, ...) {
    va_list ap;
    char *t;
    va_start(ap, fmt);
    t = sdscatvprintf(s,fmt,ap);
    va_end(ap);
    return t;
}

/*
 * 该函数的功能类似于sdscatprintf，但是要快很多，因为不依赖于sprintf函数族
 */
/* This function is similar to sdscatprintf, but much faster as it does
 * not rely on sprintf() family functions implemented by the libc that
 * are often very slow. Moreover directly handling the sds string as
 * new data is concatenated provides a performance improvement.
 *
 * However this function only handles an incompatible subset of printf-alike
 * format specifiers:
 *
 * %s - C String
 * %S - SDS string
 * %i - signed int
 * %I - 64 bit signed integer (long long, int64_t)
 * %u - unsigned int
 * %U - 64 bit unsigned integer (unsigned long long, uint64_t)
 * %% - Verbatim "%" character.
 */
sds sdscatfmt(sds s, char const *fmt, ...) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    size_t initlen = sdslen(s);
    const char *f = fmt;
    int i;
    va_list ap;

    va_start(ap,fmt);
    f = fmt;    /* Next format specifier byte to process. */
    i = initlen; /* Position of the next byte to write to dest str. */
    while(*f) {
        char next, *str;
        size_t l;
        long long num;
        unsigned long long unum;

        /* Make sure there is always space for at least 1 char. */
        if (sh->free == 0) {
            s = sdsMakeRoomFor(s,1);
            sh = (void*) (s-(sizeof(struct sdshdr)));
        }

        switch(*f) {
            case '%':
                next = *(f+1);
                f++;
                switch(next) {
                    case 's':
                    case 'S':
                        str = va_arg(ap,char*);
                        l = (next == 's') ? strlen(str) : sdslen(str);
                        if (sh->free < l) {
                            s = sdsMakeRoomFor(s,l);
                            sh = (void*) (s-(sizeof(struct sdshdr)));
                        }
                        memcpy(s+i,str,l);
                        sh->len += l;
                        sh->free -= l;
                        i += l;
                        break;
                    case 'i':
                    case 'I':
                        if (next == 'i')
                            num = va_arg(ap,int);
                        else
                        num = va_arg(ap,long long);
                        {
                            char buf[SDS_LLSTR_SIZE];
                            l = sdsll2str(buf,num);
                            if (sh->free < l) {
                                s = sdsMakeRoomFor(s,l);
                                sh = (void*) (s-(sizeof(struct sdshdr)));
                            }
                            memcpy(s+i,buf,l);
                            sh->len += l;
                            sh->free -= l;
                            i += l;
                        }
                        break;
                    case 'u':
                    case 'U':
                        if (next == 'u')
                            unum = va_arg(ap,unsigned int);
                        else
                        unum = va_arg(ap,unsigned long long);
                        {
                            char buf[SDS_LLSTR_SIZE];
                            l = sdsull2str(buf,unum);
                            if (sh->free < l) {
                                s = sdsMakeRoomFor(s,l);
                                sh = (void*) (s-(sizeof(struct sdshdr)));
                            }
                            memcpy(s+i,buf,l);
                            sh->len += l;
                            sh->free -= l;
                            i += l;
                        }
                        break;
                    default: /* Handle %% and generally %<unknown>. */
                        s[i++] = next;
                        sh->len += 1;
                        sh->free -= 1;
                        break;
                }
                break;
            default:
                s[i++] = *f;
                sh->len += 1;
                sh->free -= 1;
                break;
        }
        f++;
    }
    va_end(ap);

    /* Add null-term */
    s[i] = '\0';
    return s;
}

/*
 * 按索引对截取 sds 字符串的其中一段，start 和 end 都是闭区间（包含在内）
 *
 * 索引从 0 开始，最大为 sdslen(s) - 1
 * 索引可以是负数， sdslen(s) - 1 == -1
 */
/* Turn the string into a smaller (or equal) string containing only the
 * substring specified by the 'start' and 'end' indexes.
 *
 * start and end can be negative, where -1 means the last character of the
 * string, -2 the penultimate character, and so forth.
 *
 * The interval is inclusive, so the start and end characters will be part
 * of the resulting string.
 *
 * The string is modified in-place.
 *
 * Example:
 *
 * s = sdsnew("Hello World");
 * sdsrange(s,1,-1); => "ello World"
 */
void sdsrange(sds s, int start, int end) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    size_t newlen, len = sdslen(s);

    if (len == 0) return;
    if (start < 0) {
        start = len+start;
        if (start < 0) start = 0;
    }
    if (end < 0) {
        end = len+end;
        if (end < 0) end = 0;
    }
    newlen = (start > end) ? 0 : (end-start)+1;
    if (newlen != 0) {
        if (start >= (signed)len) {
            newlen = 0;
        } else if (end >= (signed)len) {
            end = len-1;
            newlen = (start > end) ? 0 : (end-start)+1;
        }
    } else {
        start = 0;
    }

    // 如果有需要，对字符串进行移动
    // T = O(N)
    if (start && newlen) memmove(sh->buf, sh->buf+start, newlen);

    // 添加终结符
    sh->buf[newlen] = 0;

    // 更新属性
    sh->free = sh->free+(sh->len-newlen);
    sh->len = newlen;
}

/*
 * 将长度为 len 的字符串 p 以带引号（quoted）的格式追加到给定 sds 的末尾
 */
/* Append to the sds string "s" an escaped string representation where
 * all the non-printable characters (tested with isprint()) are turned into
 * escapes in the form "\n\r\a...." or "\x<hex-number>".
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdscatrepr(sds s, const char *p, size_t len) {

    s = sdscatlen(s,"\"",1);

    while(len--) {
        switch(*p) {
            case '\\':
            case '"':
                s = sdscatprintf(s,"\\%c",*p);
                break;
            case '\n': s = sdscatlen(s,"\\n",2); break;
            case '\r': s = sdscatlen(s,"\\r",2); break;
            case '\t': s = sdscatlen(s,"\\t",2); break;
            case '\a': s = sdscatlen(s,"\\a",2); break;
            case '\b': s = sdscatlen(s,"\\b",2); break;
            default:
                if (isprint(*p))
                    s = sdscatprintf(s,"%c",*p);
                else
                    s = sdscatprintf(s,"\\x%02x",(unsigned char)*p);
                break;
        }
        p++;
    }

    return sdscatlen(s,"\"",1);
}

/*
 * 如果 c 为十六进制符号的其中一个，返回1
 *
 * sdssplitargs的辅助函数
 */
/* Helper function for sdssplitargs() that returns non zero if 'c'
 * is a valid hex digit. */
int is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/*
 * 将十六进制符号转换为 10 进制
 *
 * sdssplitargs的辅助函数
 */
/* Helper function for sdssplitargs() that converts a hex digit into an
 * integer from 0 to 15 */
int hex_digit_to_int(char c) {
    switch(c) {
        case '0': return 0;
        case '1': return 1;
        case '2': return 2;
        case '3': return 3;
        case '4': return 4;
        case '5': return 5;
        case '6': return 6;
        case '7': return 7;
        case '8': return 8;
        case '9': return 9;
        case 'a': case 'A': return 10;
        case 'b': case 'B': return 11;
        case 'c': case 'C': return 12;
        case 'd': case 'D': return 13;
        case 'e': case 'E': return 14;
        case 'f': case 'F': return 15;
        default: return 0;
    }
}

/* Split a line into arguments, where every argument can be in the
 * following programming-language REPL-alike form:
 *
 * 将一行文本分割成多个参数，每个参数可以有以下的类编程语言 REPL 格式：
 *
 * foo bar "newline are supported\n" and "\xff\x00otherstuff"
 *
 * The number of arguments is stored into *argc, and an array
 * of sds is returned.
 *
 * 参数的个数会保存在 *argc 中，函数返回一个 sds 数组。
 *
 * The caller should free the resulting array of sds strings with
 * sdsfreesplitres().
 *
 * 调用者应该使用 sdsfreesplitres() 来释放函数返回的 sds 数组。
 *
 * Note that sdscatrepr() is able to convert back a string into
 * a quoted string in the same format sdssplitargs() is able to parse.
 *
 * sdscatrepr() 可以将一个字符串转换为一个带引号（quoted）的字符串，
 * 这个带引号的字符串可以被 sdssplitargs() 分析。
 *
 * The function returns the allocated tokens on success, even when the
 * input string is empty, or NULL if the input contains unbalanced
 * quotes or closed quotes followed by non space characters
 * as in: "foo"bar or "foo'
 *
 * 即使输入出现空字符串， NULL ，或者输入带有未对应的括号，
 * 函数都会将已成功处理的字符串先返回。
 *
 * 这个函数可用于 config.c 中对配置文件进行分析。
 * 例子：
 *  sds *arr = sdssplitargs("timeout 10086\r\nport 123321\r\n");
 * 会得出
 *  arr[0] = "timeout"
 *  arr[1] = "10086"
 *  arr[2] = "port"
 *  arr[3] = "123321"
 */
sds *sdssplitargs(const char *line, int *argc) {
    const char *p = line;
    char *current = NULL;
    char **vector = NULL;

    *argc = 0;
    while(1) {

        /* skip blanks */
        // 跳过空白
        // T = O(N)
        while(*p && isspace(*p)) p++;

        if (*p) {
            /* get a token */
            int inq=0;  /* set to 1 if we are in "quotes" */
            int insq=0; /* set to 1 if we are in 'single quotes' */
            int done=0;

            if (current == NULL) current = sdsempty();

            // T = O(N)
            while(!done) {
                if (inq) {
                    if (*p == '\\' && *(p+1) == 'x' &&
                        is_hex_digit(*(p+2)) &&
                        is_hex_digit(*(p+3)))
                    {
                        unsigned char byte;

                        byte = (hex_digit_to_int(*(p+2))*16)+
                               hex_digit_to_int(*(p+3));
                        current = sdscatlen(current,(char*)&byte,1);
                        p += 3;
                    } else if (*p == '\\' && *(p+1)) {
                        char c;

                        p++;
                        switch(*p) {
                            case 'n': c = '\n'; break;
                            case 'r': c = '\r'; break;
                            case 't': c = '\t'; break;
                            case 'b': c = '\b'; break;
                            case 'a': c = '\a'; break;
                            default: c = *p; break;
                        }
                        current = sdscatlen(current,&c,1);
                    } else if (*p == '"') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p+1) && !isspace(*(p+1))) goto err;
                        done=1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = sdscatlen(current,p,1);
                    }
                } else if (insq) {
                    if (*p == '\\' && *(p+1) == '\'') {
                        p++;
                        current = sdscatlen(current,"'",1);
                    } else if (*p == '\'') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p+1) && !isspace(*(p+1))) goto err;
                        done=1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = sdscatlen(current,p,1);
                    }
                } else {
                    switch(*p) {
                        case ' ':
                        case '\n':
                        case '\r':
                        case '\t':
                        case '\0':
                            done=1;
                            break;
                        case '"':
                            inq=1;
                            break;
                        case '\'':
                            insq=1;
                            break;
                        default:
                            current = sdscatlen(current,p,1);
                            break;
                    }
                }
                if (*p) p++;
            }
            /* add the token to the vector */
            // T = O(N)
            vector = zrealloc(vector,((*argc)+1)*sizeof(char*));
            vector[*argc] = current;
            (*argc)++;
            current = NULL;
        } else {
            /* Even on empty input string return something not NULL. */
            if (vector == NULL) vector = zmalloc(sizeof(void*));
            return vector;
        }
    }

    err:
    while((*argc)--)
        sdsfree(vector[*argc]);
    zfree(vector);
    if (current) sdsfree(current);
    *argc = 0;
    return NULL;
}
