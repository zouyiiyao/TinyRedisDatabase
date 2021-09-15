//
// Created by zouyi on 2021/8/31.
//

#include <math.h>
#include <ctype.h>
#include "redis.h"

/*
 * redis对象有关操作
 */

/*
 * 创建一个redis对象，并执行初始化操作，
 * 调用时指定对象类型，和指向实际值的指针
 */
robj* createObject(int type, void* ptr) {
    robj* o = zmalloc(sizeof(robj));

    o->type = type;
    o->encoding = REDIS_ENCODING_RAW;
    o->ptr = ptr;
    o->refcount = 1;

    /* Set the LRU to the current lruclock */
    o->lru = getLRUClock();
    return o;
}

/*
 * 创建一个字符串类型的对象，使用REDIS_ENCODING_RAW编码，对象的指针属性指向一个sds
 * 注意: REDIS_ENCODING_RAW编码方式下需要调用两次内存分配操作，所以存放对象和存放sds的内存不一定连续
 */
robj* createRawStringObject(char* ptr, size_t len) {
    return createObject(REDIS_STRING, sdsnewlen(ptr, len));
}

/*
 * 创建一个字符串类型的对象，使用REDIS_ENCODING_EMBSTR编码，对象的指针属性指向一个sds
 * 注意: REDIS_ENCODING_EMBSTR编码方式下存放对象和存放sds的内存一起分配，是连续的一段内存，
 * 因此，这个字符串对象/sds是不可以修改的
 *
 */
robj* createEmbeddedStringObject(char* ptr, size_t len) {
    robj* o = zmalloc(sizeof(robj) + sizeof(struct sdshdr) + len + 1);
    // sh指向分配好的struct sdshdr首地址
    struct sdshdr* sh = (void*)(o + 1);

    o->type = REDIS_STRING;
    o->encoding = REDIS_ENCODING_EMBSTR;
    o->ptr = sh + 1;
    o->refcount = 1;
    o->lru = getLRUClock();

    sh->len = len;
    sh->free = 0;
    if (ptr) {
        memcpy(sh->buf, ptr, len);
        sh->buf[len] = '\0';
    } else {
        memset(sh->buf, 0, len + 1);
    }
    return o;
}

/*
 * 可以使用REDIS_ENCODING_EMBSTR进行编码的字符串类型对象长度上限
 * 注意: 当长度等于39时刚好一共需要分配64字节空间
 */
#define REDIS_ENCODING_EMBSTR_SIZE_LIMIT 39

/*
 * 创建一个字符串类型的对象
 * 根据指定字符串的长度是否超过能使用REDIS_ENCODING_EMBSTR进行编码的长度上限，
 * 选择使用REDIS_ENCODING_EMBSTR编码还是REDIS_ENCODING_RAW编码
 */
robj* createStringObject(char* ptr, size_t len) {
    if (len <= REDIS_ENCODING_EMBSTR_SIZE_LIMIT)
        return createEmbeddedStringObject(ptr, len);
    else
        return createRawStringObject(ptr, len);
}

/*
 * 根据传入的整数值，创建一个字符串对象，
 * 如果传入的整数值在long表示范围内，则直接使用对象的指针属性存该整数值(类型强转)，REDIS_ENCODING_INT编码；
 * 如果该整数值不能用long类型保存，则将值转化为一个sds，并创建一个REDIS_ENCODING_RAW的字符串对象来保存值
 */
robj* createStringObjectFromLongLong(long long value) {

    robj* o;

    // 初始化时定义的共享对象，暂时不使用
    /*
    if (value >= 0 && value <= REDIS_SHARED_INTEGERS) {
        incrRefCount(shared.integers[value]);
        o = shared.integers[value];
    } else */ {
        if (value >= LONG_MIN && value <= LONG_MAX) {
            o = createObject(REDIS_STRING, NULL);
            o->encoding = REDIS_ENCODING_INT;
            o->ptr = (void*)((long)value);
        } else {
            o = createObject(REDIS_STRING, sdsfromlonglong(value));
        }
    }

    return o;
}

/*
 * 根据传入的long double值，创建一个字符串对象，
 * 保存时将long double转为字符串
 */
robj* createStringObjectFromLongDouble(long double value) {
    char buf[256];
    int len;

    // 最多写入256字节，使用17位小数精度
    len = snprintf(buf, sizeof(buf), "%.17LF", value);

    if (strchr(buf, '.') != NULL) {
        char* p = buf + len - 1;
        while (*p == '0') {
            p--;
            len--;
        }
        if (*p == '.') len--;
    }

    return createStringObject(buf, len);
}

/*
 * 深度拷贝一个字符串对象，返回新创建的对象
 */
robj* dupStringObject(robj* o) {
    robj* d;

    assert(o->type == REDIS_STRING);

    switch (o->encoding) {

        case REDIS_ENCODING_RAW:
            return createRawStringObject(o->ptr, sdslen(o->ptr));

        case REDIS_ENCODING_EMBSTR:
            return createEmbeddedStringObject(o->ptr, sdslen(o->ptr));

        case REDIS_ENCODING_INT:
            d = createObject(REDIS_STRING, NULL);
            d->encoding = REDIS_ENCODING_INT;
            d->ptr = o->ptr;
            return d;

        default:
            exit(1);
    }
}

/*
 * 创建一个列表类型的对象，使用REIDS_ENCODING_LINKEDLIST编码
 * 注意: 这个列表类型对象的指针属性指向创建的list，设置释放该list节点的方法为decrRefCountVoid，
 * 意味着链表节点listNode中保存实际内容的指针指向的也是一个redis对象(嵌套字符串类型的对象)，通过引用计数来
 * 管理其内存资源的申请和释放
 */
robj* createListObject(void) {

    // 创建一个空的双向链表
    list* l = listCreate();

    // 创建一个列表类型的redis对象，使用REIDS_ENCODING_LINKEDLIST编码，底层使用的是双向链表
    robj* o = createObject(REDIS_LIST, l);

    // 设置释放节点的函数指针
    listSetFreeMethod(l, decrRefCountVoid);

    // 设置底层编码属性
    o->encoding = REDIS_ENCODING_LINKEDLIST;

    return o;
}

/*
 * 创建一个列表类型的对象，使用REDIS_ENCODING_ZIPLIST编码
 * 注意: 这个列表类型对象的指针属性等于创建的压缩列表首地址，不需要嵌套对象
 */
robj* createZiplistObject(void) {

    unsigned char* zl = ziplistNew();

    robj* o = createObject(REDIS_LIST, zl);

    o->encoding = REDIS_ENCODING_ZIPLIST;

    return o;
}

/*
 * 创建一个集合类型的对象，使用REDIS_ENCODING_HT编码
 */
robj* createSetObject(void) {

    dict* d = dictCreate(&setDictType, NULL);

    robj* o = createObject(REDIS_SET, d);

    o->encoding = REDIS_ENCODING_HT;

    return o;
}

/*
 * 创建一个集合类型的对象，使用REDIS_ENCODING_INTSET编码
 */
robj* createIntsetObject(void) {

    intset* is = intsetNew();

    robj* o = createObject(REDIS_SET, is);

    o->encoding = REDIS_ENCODING_INTSET;

    return o;
}

/*
 * 创建一个哈希类型的对象，使用REDIS_ENCODING_ZIPLIST编码
 * 注意: 哈希类型对象底层编码的转换(REDIS_ENCODING_HT)在t_hash.c中
 */
robj* createHashObject(void) {

    unsigned char* zl = ziplistNew();

    robj* o = createObject(REDIS_HASH, zl);

    o->encoding = REDIS_ENCODING_ZIPLIST;

    return o;
}

/*
 * 创建一个有序集合类型的对象，使用REDIS_ENCODING_SKIPLIST编码
 * 底层是跳表和字典
 */
robj* createZsetObject(void) {

    zset* zs = zmalloc(sizeof(zset));

    robj* o;

    zs->dict = dictCreate(&zsetDictType, NULL);
    zs->zsl = zslCreate();

    o = createObject(REDIS_ZSET, zs);

    o->encoding = REDIS_ENCODING_SKIPLIST;

    return o;
}

/*
 * 创建一个有序集合类型的对象，使用REDIS_ENCODING_ZIPLIST编码
 */
robj* createZsetZiplistObject(void) {

    unsigned char* zl = ziplistNew();

    robj* o = createObject(REDIS_ZSET, zl);

    o->encoding = REDIS_ENCODING_ZIPLIST;

    return o;
}

/*
 * 释放字符串对象的值，底层编码必须为REDIS_ENCODING_RAW
 * REDIS_ENCODING_INT编码直接使用指针属性存值，不需要释放；
 * REDIS_ENCODING_EMBST编码对象和值是同一段内存，所以也不需要(单独)释放；
 */
void freeStringObject(robj* o) {
    if (o->encoding == REDIS_ENCODING_RAW) {
        sdsfree(o->ptr);
    }
}

/*
 * 释放列表对象的值，根据底层编码不同释放操作不同
 */
void freeListObject(robj* o) {
    switch (o->encoding) {

        case REDIS_ENCODING_LINKEDLIST:
            listRelease((list*) o->ptr);
            break;

        case REDIS_ENCODING_ZIPLIST:
            zfree(o->ptr);
            break;

        default:
            exit(1);
    }
}

/*
 * 释放集合对象的值，根据底层编码不同释放操作不同
 */
void freeSetObject(robj* o) {
    switch (o->encoding) {

        case REDIS_ENCODING_HT:
            dictRelease((dict*)o->ptr);
            break;

        case REDIS_ENCODING_INTSET:
            zfree(o->ptr);
            break;

        default:
            exit(1);
    }
}

/*
 * 释放有序集合对象的值，根据底层编码不同释放操作不同
 */
void freeZsetObject(robj* o) {

    zset* zs;

    switch (o->encoding) {

        case REDIS_ENCODING_SKIPLIST:
            zs = o->ptr;
            dictRelease(zs->dict);
            zslFree(zs->zsl);
            zfree(zs);
            break;

        case REDIS_ENCODING_ZIPLIST:
            zfree(o->ptr);
            break;

        default:
            exit(1);
    }
}

/*
 * 释放哈希对象的值，根据底层编码不同释放操作不同
 */
void freeHashObject(robj* o) {
    switch (o->encoding) {

        case REDIS_ENCODING_HT:
            dictRelease((dict*)o->ptr);
            break;

        case REDIS_ENCODING_ZIPLIST:
            zfree(o->ptr);
            break;

        default:
            exit(1);
    }
}

/*
 * 增加对象的引用计数
 */
void incrRefCount(robj* o) {
    o->refcount++;
}

/*
 * 减少对象的引用计数，当引用计数被减到0时，释放对象(同时释放值)
 */
void decrRefCount(robj* o) {

    if (o->refcount <= 0) exit(1);

    if (o->refcount == 1) {
        switch (o->type) {
            case REDIS_STRING:
                freeStringObject(o);
                break;
            case REDIS_LIST:
                freeListObject(o);
                break;
            case REDIS_SET:
                freeSetObject(o);
                break;
            case REDIS_ZSET:
                freeZsetObject(o);
                break;
            case REDIS_HASH:
                freeHashObject(o);
                break;
            default:
                exit(1);
        }
        zfree(o);
    } else {
        o->refcount--;
    }
}

/*
 * 作用于特定数据类型释放函数的包装
 */
void decrRefCountVoid(void* o) {
    decrRefCount(o);
}

/*
 * 将对象的引用计数设为0，但并不释放对象
 */
robj* resetRefCount(robj* obj) {
    obj->refcount = 0;
    return obj;
}

/*
 * 检查对象o中的值是否能表示为long long，如果能表示的话将转换后的值存在*llval中
 */
int isObjectRepresentableAsLongLong(robj* o, long long* llval) {

    assert(o->type == REDIS_STRING);

    if (o->encoding == REDIS_ENCODING_INT) {
        if (llval) *llval = (long)o->ptr;
        return REDIS_OK;
    } else {
        return string2ll(o->ptr, sdslen(o->ptr), llval) ? REDIS_OK : REDIS_ERR;
    }
}

/*
 * 获取对象o的REDIS_ENCODING_RAW编码或REDIS_ENCODING_EMBSTR编码版本对象
 * 如果对象本来就是REDIS_ENCODING_RAW编码或REDIS_ENCODING_EMBSTR编码，则引用计数加1，返回原对象；
 * 否则，创建一个新的REDIS_ENCODING_RAW编码或REDIS_ENCODING_EMBSTR编码的对象，返回新创建的对象
 */
robj* getDecodedObject(robj* o) {
    robj* dec;

    // 字符串类型对象，是REDIS_ENCODING_RAW编码或REDIS_ENCODING_EMBSTR编码
    if (sdsEncodesObject(o)) {
        incrRefCount(o);
        return o;
    }

    // 字符串类型对象，是REDIS_ENCODING_INT编码
    if (o->type == REDIS_STRING && o->encoding == REDIS_ENCODING_INT) {
        // 将原对象的值转化为字符串，并创建一个新的对象，使用REDIS_ENCODING_RAW编码或REDIS_ENCODING_EMBSTR编码
        char buf[32];

        ll2string(buf, 32, (long)o->ptr);
        dec = createStringObject(buf, strlen(buf));
        return dec;
    } else {
        exit(1);
    }
}

/*
 * 根据flags的取值，使用strcmp或strcoll来对比字符串类型对象
 */
#define REDIS_COMPARE_BINARY (1<<0)
#define REDIS_COMPARE_COLL (1<<1)

int compareStringObjectsWithFlags(robj* a, robj* b, int flags) {
    assert(a->type == REDIS_STRING && b->type == REDIS_STRING);

    char bufa[128], bufb[128];
    char* astr;
    char* bstr;
    size_t alen, blen, minlen;

    if (a == b) return 0;

    // 取出对象内容，需要的话转为字符串
    if (sdsEncodesObject(a)) {
        astr = a->ptr;
        alen = sdslen(astr);
    } else {
        alen = ll2string(bufa, sizeof(bufa), (long)a->ptr);
        astr = bufa;
    }

    // 取出对象内容，需要的话转为字符串
    if (sdsEncodesObject(b)) {
        bstr = b->ptr;
        blen = sdslen(bstr);
    } else {
        blen = ll2string(bufb, sizeof(bufb), (long)b->ptr);
        bstr = bufb;
    }

    // 比较字符串
    if (flags & REDIS_COMPARE_COLL) {
        return strcoll(astr, bstr);
    // 逐字节比较，二进制安全
    } else {
        int cmp;

        minlen = (alen < blen) ? alen : blen;
        cmp = memcmp(astr, bstr, minlen);
        if (cmp == 0) return alen - blen;
        return cmp;
    }
}

/*
 * 比较两个字符串类型对象，二进制安全
 */
int compareStringObjects(robj* a, robj* b) {
    return compareStringObjectsWithFlags(a, b, REDIS_COMPARE_BINARY);
}

/*
 * 比较两个字符串类型对象，非二进制安全
 */
int collateStringObjects(robj* a, robj* b) {
    return compareStringObjectsWithFlags(a, b, REDIS_COMPARE_COLL);
}

/*
 * 比较两个字符串类型对象是否相等，对全为REDIS_ENCODING_INT编码的情况做了优化
 */
int equalStringObjects(robj* a, robj* b) {
    if (a->encoding == REDIS_ENCODING_INT && b->encoding == REDIS_ENCODING_INT) {
        return a->ptr == b->ptr;
    } else {
        return compareStringObjects(a, b) == 0;
    }
}

/*
 * 返回字符串类型对象的长度，如果是REDIS_ENCODING_INT编码，返回其转为字符串后的长度
 */
size_t stringObjectLen(robj* o) {

    assert(o->type == REDIS_STRING);

    if (sdsEncodesObject(o)) {
        return sdslen(o->ptr);
    } else {
        char buf[32];
        return ll2string(buf, 32, (long)o->ptr);
    }
}

/*
 * 尝试从对象中取值，转为double类型，存在*target中
 */
int getDoubleFromObject(robj* o, double* target) {
    double value;
    char* eptr;

    if (o == NULL) {
        value = 0;
    } else {
        assert(o->type == REDIS_STRING);

        if (sdsEncodesObject(o)) {
            errno = 0;
            value = strtod(o->ptr, &eptr);
            if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' ||
                (errno == ERANGE && (value == HUGE_VAL || value == -HUGE_VAL || value == 0)) ||
                errno == EINVAL || isnan(value))
                return REDIS_ERR;
        } else if (o->encoding == REDIS_ENCODING_INT) {
            value = (long)o->ptr;
        } else {
            exit(1);
        }
    }

    *target = value;
    return REDIS_OK;
}

/*
 * 尝试从对象中取值，转为long double类型，存在*target中
 */
int getLongDoubleFromObject(robj* o, long double* target) {
    long double value;
    char* eptr;

    if (o == NULL) {
        value = 0;
    } else {
        assert(o->type == REDIS_STRING);

        if (sdsEncodesObject(o)) {
            errno = 0;
            value = strtold(o->ptr, &eptr);
            if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' ||
                errno == ERANGE || isnan(value))
                return REDIS_ERR;
        } else if (o->encoding == REDIS_ENCODING_INT) {
            value = (long)o->ptr;
        } else {
            exit(1);
        }
    }

    *target = value;
    return REDIS_OK;
}

/*
 * 尝试从对象中取值，转为long long类型，存在*target中
 */
int getLongLongFromObject(robj* o, long long* target) {
    long long value;
    char* eptr;

    if (o == NULL) {
        value = 0;
    } else {

        assert(o->type == REDIS_STRING);
        if (sdsEncodesObject(o)) {
            errno = 0;
            value = strtoll(o->ptr, &eptr, 10);
            if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' ||
                errno == ERANGE)
                return REDIS_ERR;
        } else if (o->encoding == REDIS_ENCODING_INT) {
            value = (long)o->ptr;
        } else {
            exit(1);
        }
    }

    if (target) *target = value;

    return REDIS_OK;
}

/*
 * 返回编码对应的字符串表示
 */
char* strEncoding(int encoding) {
    switch (encoding) {
        case REDIS_ENCODING_RAW: return "raw";
        case REDIS_ENCODING_INT: return "int";
        case REDIS_ENCODING_HT: return "hashtable";
        case REDIS_ENCODING_LINKEDLIST: return "linkedlist";
        case REDIS_ENCODING_ZIPLIST: return "ziplist";
        case REDIS_ENCODING_INTSET: return "intset";
        case REDIS_ENCODING_SKIPLIST: return "skiplist";
        case REDIS_ENCODING_EMBSTR: return "embstr";
        default: return "unknown";
    }
}

/*
 * 计算给定对象的空转时长
 */
unsigned long long estimateObjectIdleTime(robj* o) {
    unsigned long long lruclock = getLRUClock();
    if (lruclock >= o->lru) {
        return (lruclock - o->lru) * REDIS_LRU_CLOCK_RESOLUTION;
    } else {
        // next time loop
        return (lruclock  + (REDIS_LRU_CLOCK_MAX - o->lru)) * REDIS_LRU_CLOCK_RESOLUTION;
    }
}
