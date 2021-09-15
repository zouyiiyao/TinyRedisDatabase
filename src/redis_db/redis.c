//
// Created by zouyi on 2021/8/31.
//

#include <time.h>
#include <sys/time.h>
#include "redis.h"

/*
 * 返回微秒级别的UNIX时间戳
 */
long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec) * 1000000;
    ust += tv.tv_usec;
    return ust;
}

/*
 * 返回毫秒级别的时间戳
 */
long long mstime(void) {
    return ustime() / 1000;
}

/*
 * 获取秒级时钟
 */
unsigned int getLRUClock(void) {
    return (mstime() / REDIS_LRU_CLOCK_RESOLUTION) & REDIS_LRU_CLOCK_MAX;
}

/*
 * 比较两个sds
 */
int dictSdsKeyCompare(void* privdata, const void* key1, const void* key2) {
    int l1, l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

/*
 * redis对象作为字典的键/值时使用的销毁函数
 */
void dictRedisObjectDestructor(void* privdata, void* val) {
    DICT_NOTUSED(privdata);

    if (val == NULL) return;
    decrRefCount(val);
}

/*
 * redis对象(只允许字符串类型对象)作为字典的键时使用的比较函数
 */
int dictEncObjKeyCompare(void* privdata, const void* key1, const void* key2) {
    robj* o1 = (robj*)key1;
    robj* o2 = (robj*)key2;
    int cmp;

    if (o1->encoding == REDIS_ENCODING_INT && o2->encoding == REDIS_ENCODING_INT)
        return o1->ptr == o2->ptr;

    o1 = getDecodedObject(o1);
    o2 = getDecodedObject(o2);
    cmp = dictSdsKeyCompare(privdata, o1->ptr, o2->ptr);
    decrRefCount(o1);
    decrRefCount(o2);
    return cmp;
}

/*
 * redis对象(只允许字符串类型对象)作为字典的键时使用的哈希函数
 */
unsigned int dictEncObjHash(const void* key) {
    robj* o = (robj*)key;

    if (sdsEncodesObject(o)) {
        return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
    } else {
        if (o->encoding == REDIS_ENCODING_INT) {
            char buf[32];
            int len;

            len = ll2string(buf, 32, (long)o->ptr);
            return dictGenHashFunction((unsigned char*)buf, len);
        // 此处源码中为无用分支，redis6.0稳定版还没有修复
        } else {
            exit(1);
        }
    }
}

/*
 * 字典用作集合类型对象的底层实现时，使用的特有函数
 */
dictType setDictType = {
    dictEncObjHash,
    NULL,
    NULL,
    dictEncObjKeyCompare,
    dictRedisObjectDestructor,
    NULL
};

/*
 * 字典用作有序集合类型对象的底层实现时，使用的特有函数
 */
dictType zsetDictType = {
    dictEncObjHash,
    NULL,
    NULL,
    dictEncObjKeyCompare,
    dictRedisObjectDestructor,
    NULL
};

/*
 * 字典用作哈希类型对象的底层实现时，使用的特有函数
 */
dictType hashDictType = {
    dictEncObjHash,
    NULL,
    NULL,
    dictEncObjKeyCompare,
    dictRedisObjectDestructor,
    dictRedisObjectDestructor
};

/*
 * 判断字典是否需要缩小
 */
int htNeedsResize(dict* dict) {
    long long size, used;

    size = dictSlots(dict);
    used = dictSize(dict);
    return (size && used && size > DICT_HT_INITIAL_SIZE && (used * 100 / size < REDIS_HT_MINFILL));
}
