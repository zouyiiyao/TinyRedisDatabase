//
// Created by zouyi on 2021/9/12.
//

#include "redis.h"

/*
 * Zset API: Ziplist-backed sorted set API
 */

/*
 * 取出sptr指向节点所保存的有序集合元素分值
 */
double zzlGetScore(unsigned char* sptr) {
    unsigned char* vstr;
    unsigned int vlen;
    long long vlong;
    char buf[128];
    double score;

    assert(sptr != NULL);
    assert(ziplistGet(sptr, &vstr, &vlen, &vlong));

    if (vstr) {
        memcpy(buf, vstr, vlen);
        buf[vlen] = '\0';
        score = strtod(buf, NULL);
    } else {
        score = vlong;
    }

    return score;
}

/*
 * 取出sptr指向节点所保存的有序集合元素，返回一个新创建的对象
 */
robj* ziplistGetObject(unsigned char* sptr) {
    unsigned char* vstr;
    unsigned int vlen;
    long long vlong;

    assert(sptr != NULL);
    assert(ziplistGet(sptr, &vstr, &vlen, &vlong));

    if (vstr) {
        return createStringObject((char*)vstr, vlen);
    } else {
        return createStringObjectFromLongLong(vlong);
    }
}

/*
 * 将eptr指向的元素与cstr进行比对
 */
int zzlCompareElements(unsigned char* eptr, unsigned char* cstr, unsigned int clen) {
    unsigned char* vstr;
    unsigned int vlen;
    long long vlong;
    unsigned char vbuf[32];
    int minlen, cmp;

    assert(ziplistGet(eptr, &vstr, &vlen, &vlong));
    if (vstr == NULL) {
        /* Store string representation of long long in buf. */
        vlen = ll2string((char*)vbuf, sizeof(vbuf), vlong);
        vstr = vbuf;
    }

    minlen = (vlen < clen) ? vlen : clen;
    cmp = memcmp(vstr, cstr, minlen);
    if (cmp == 0) return vlen - clen;
    return cmp;
}

/*
 * 返回有序集合保存的元素-分值对数目
 */
unsigned int zzlLength(unsigned char* zl) {
    return ziplistLen(zl) / 2;
}

/*
 * 移动到下一个元素和分值，其地址分别用*eptr和*sptr保存
 * 如果后面已经没有元素-分值对，则*eptr和*sptr都保存NULL
 */
void zzlNext(unsigned char* zl, unsigned char** eptr, unsigned char** sptr) {
    unsigned char* _eptr;
    unsigned char* _sptr;

    assert(*eptr != NULL && *sptr != NULL);

    _eptr = ziplistNext(zl, *sptr);
    if (_eptr != NULL) {
        _sptr = ziplistNext(zl, _eptr);
        assert(_sptr != NULL);
    } else {
        _sptr = NULL;
    }

    *eptr = _eptr;
    *sptr = _sptr;
}

/*
 * 移动到上一个元素和分值，其地址分别用*eptr和*sptr保存
 * 如果前面已经没有元素-分值对，则*eptr和*sptr都保存NULL
 */
void zzlPrev(unsigned char* zl, unsigned char** eptr, unsigned char** sptr) {
    unsigned char* _eptr;
    unsigned char* _sptr;

    assert(*eptr != NULL && *sptr != NULL);

    _sptr = ziplistPrev(zl, *eptr);
    if (_sptr != NULL) {
        _eptr = ziplistPrev(zl, _sptr);
        assert(_eptr != NULL);
    } else {
        _eptr = NULL;
    }

    *eptr = _eptr;
    *sptr = _sptr;
}

/*
 * 检查value是否大于范围内的最小值
 */
static int zzlValueGteMin(double value, zrangespec* spec) {
    return spec->minex ? (value > spec->min) : (value >= spec->min);
}

/*
 * 检查value是否小于范围内的最大值
 */
static int zzlValueLteMax(double value, zrangespec* spec) {
    return spec->maxex ? (value < spec->max) : (value <= spec->max);
}

/*
 * 判断压缩列表编码的有序集合分值范围是否与range有交集
 */
int zzlIsInRange(unsigned char* zl, zrangespec* range) {
    unsigned char* p;
    double score;

    // 空区间
    if (range->min > range->max || (range->min == range->max && (range->minex || range->maxex)))
        return 0;

    /* Last score */
    p = ziplistIndex(zl, -1);
    /* Empty sorted set */
    if (p == NULL) return 0;
    score = zzlGetScore(p);
    if (!zzlValueGteMin(score, range))
        return 0;

    /* First score */
    p = ziplistIndex(zl, 1);
    assert(p != NULL);
    score = zzlGetScore(p);
    if (!zzlValueLteMax(score, range))
        return 0;

    return 1;
}

/*
 * 从前往后遍历压缩列表，找到满足分值在range范围内的第一个元素
 */
unsigned char* zzlFirstInRange(unsigned char* zl, zrangespec* range) {
    unsigned char* eptr = ziplistIndex(zl, 0);
    unsigned char* sptr;
    double score;

    if (!zzlIsInRange(zl, range)) return NULL;

    while (eptr != NULL) {
        sptr = ziplistNext(zl, eptr);
        assert(sptr != NULL);

        score = zzlGetScore(sptr);
        if (zzlValueGteMin(score, range)) {
            if (zzlValueLteMax(score, range))
                return eptr;
            return NULL;
        }

        eptr = ziplistNext(zl, sptr);
    }

    return NULL;
}

/*
 * 从后往前遍历压缩列表，找到满足分值在range范围内的最后一个元素
 */
unsigned char* zzlLastInRange(unsigned char* zl, zrangespec* range) {
    unsigned char* eptr = ziplistIndex(zl, -2);
    unsigned char* sptr;
    double score;

    if (!zzlIsInRange(zl, range)) return NULL;

    while (eptr != NULL) {
        sptr = ziplistNext(zl, eptr);
        assert(sptr != NULL);

        score = zzlGetScore(sptr);
        if (zzlValueLteMax(score, range)) {
            if (zzlValueGteMin(score, range))
                return eptr;
            return NULL;
        }

        /* Move to previous element by moving to the score of previous element.
         * When this returns NULL, we know there also is no element. */
        sptr = ziplistPrev(zl, eptr);
        if (sptr != NULL)
            assert((eptr = ziplistPrev(zl, sptr)) != NULL);
        else
            eptr = NULL;
    }

    return NULL;
}

/*
 * 从ziplist编码的有序集合中查找ele元素，并将它的分值保存在score中
 */
unsigned char* zzlFind(unsigned char* zl, robj* ele, double* score) {
    unsigned char* eptr = ziplistIndex(zl, 0);
    unsigned char* sptr;

    ele = getDecodedObject(ele);

    while (eptr != NULL) {
        sptr = ziplistNext(zl, eptr);
        assert(sptr != NULL);

        if (ziplistCompare(eptr, ele->ptr, sdslen(ele->ptr))) {
            if (score != NULL) *score = zzlGetScore(sptr);
            decrRefCount(ele);
            return eptr;
        }

        eptr = ziplistNext(zl, sptr);
    }

    decrRefCount(ele);

    return NULL;
}

/*
 * 从ziplist编码的有序集合中删除eptr指向的有序集合元素-分值对
 */
unsigned char* zzlDelete(unsigned char* zl, unsigned char* eptr) {
    unsigned char* p = eptr;

    zl = ziplistDelete(zl, &p);
    zl = ziplistDelete(zl, &p);
    return zl;
}

/*
 * 将指定元素-分值对插入到eptr指向的节点前面
 */
unsigned char* zzlInsertAt(unsigned char* zl, unsigned char* eptr, robj* ele, double score) {
    unsigned char* sptr;
    char scorebuf[128];
    int scorelen;
    size_t offset;

    assert(sdsEncodesObject(ele));
    scorelen = d2string(scorebuf, sizeof(scorebuf), score);

    if (eptr == NULL) {
        zl = ziplistPush(zl, ele->ptr, sdslen(ele->ptr), ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char*)scorebuf, scorelen, ZIPLIST_TAIL);
    } else {
        offset = eptr - zl;
        zl = ziplistInsert(zl, eptr, ele->ptr, sdslen(ele->ptr));
        eptr = zl + offset;

        assert((sptr = ziplistNext(zl, eptr)) != NULL);
        zl = ziplistInsert(zl, sptr, (unsigned char*)scorebuf, scorelen);
    }

    return zl;
}

/*
 * 将元素-分值对按照分值从小到大顺序插入到ziplist编码的有序集合
 * 注意: 这个函数假设ele之前并未在有序集合中存在
 */
unsigned char* zzlInsert(unsigned char* zl, robj* ele, double score) {

    unsigned char* eptr = ziplistIndex(zl, 0);
    unsigned char* sptr;
    double s;

    ele = getDecodedObject(ele);

    while (eptr != NULL) {

        sptr = ziplistNext(zl, eptr);
        assert(sptr != NULL);
        s = zzlGetScore(sptr);

        if (s > score) {
            zl = zzlInsertAt(zl, eptr, ele, score);
            break;
        } else if (s == score) {
            if (zzlCompareElements(eptr, ele->ptr, sdslen(ele->ptr)) > 0) {
                zl = zzlInsertAt(zl, eptr, ele, score);
                break;
            }
        }

        // 新添加的元素分值比当前遍历到的元素分值大，移动到下一个节点
        eptr = ziplistNext(zl, sptr);
    }

    if (eptr == NULL)
        zl = zzlInsertAt(zl, NULL, ele, score);

    decrRefCount(ele);
    return zl;
}

/*
 * 删除ziplist中分值在指定范围内的元素-分值对
 */
unsigned char* zzlDeleteRangeByScore(unsigned char* zl, zrangespec* range, unsigned long* deleted) {
    unsigned char* eptr;
    unsigned char* sptr;
    double score;
    unsigned long num = 0;

    if (deleted != NULL) *deleted = 0;

    eptr = zzlFirstInRange(zl, range);
    if (eptr == NULL) return zl;

    while ((sptr = ziplistNext(zl, eptr)) != NULL) {
        score = zzlGetScore(sptr);
        if (zzlValueLteMax(score, range)) {
            /* Delete both the element and the score. */
            zl = ziplistDelete(zl, &eptr);
            zl = ziplistDelete(zl, &eptr);
            num++;
        } else {
            /* No longer in range. */
            break;
        }
    }

    if (deleted != NULL) *deleted = num;
    return zl;
}

/*
 * 删除ziplist中分值在指定排位范围内的元素-分值对
 */
unsigned char* zzlDeleteRangeByRank(unsigned char* zl, unsigned int start, unsigned int end, unsigned long* deleted) {
    unsigned int num = (end - start) + 1;

    if (deleted != NULL) *deleted = num;

    zl = ziplistDeleteRange(zl, 2 * (start - 1), 2 * num);

    return zl;
}

/*
 * Common sorted set API
 */

/*
 * 返回有序集合的元素-分值对数目
 */
unsigned int zsetLength(robj* zobj) {

    int length = -1;

    if (zobj->encoding == REDIS_ENCODING_ZIPLIST) {
        length = zzlLength(zobj->ptr);
    } else if (zobj->encoding == REDIS_ENCODING_SKIPLIST) {
        length = ((zset*)zobj->ptr)->zsl->length;
    } else {
        exit(1);
    }

    return length;
}

/*
 * 将有序集合对象zobj底层编码转换为encoding
 */
void zsetConvert(robj* zobj, int encoding) {
    zset* zs;
    zskiplistNode* node;
    zskiplistNode* next;
    robj* ele;
    double score;

    if (zobj->encoding == encoding) return;

    // zobj原来编码是REDIS_ENCODING_ZIPLIST，转为REDIS_ENCODING_SKIPLIST
    if (zobj->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char* zl = zobj->ptr;
        unsigned char* eptr;
        unsigned char* sptr;
        unsigned char* vstr;
        unsigned int vlen;
        long long vlong;

        if (encoding != REDIS_ENCODING_SKIPLIST)
            exit(1);

        zs = zmalloc(sizeof(zset));
        zs->dict = dictCreate(&zsetDictType, NULL);
        zs->zsl = zslCreate();

        eptr = ziplistIndex(zl, 0);
        assert(eptr != NULL);
        sptr = ziplistNext(zl, eptr);
        assert(sptr != NULL);

        while (eptr != NULL) {

            score = zzlGetScore(sptr);

            assert(ziplistGet(eptr, &vstr, &vlen, &vlong));

            if (vstr == NULL)
                ele = createStringObjectFromLongLong(vlong);
            else
                ele = createStringObject((char*)vstr, vlen);

            node = zslInsert(zs->zsl, score, ele);

            assert(dictAdd(zs->dict, ele, &node->score) == DICT_OK);
            incrRefCount(ele);

            zzlNext(zl, &eptr, &sptr);
        }

        zfree(zobj->ptr);

        zobj->ptr = zs;
        zobj->encoding = REDIS_ENCODING_SKIPLIST;

    // zobj原来编码是REDIS_ENCODING_SKIPLIST，转为REDIS_ENCODING_ZIPLIST
    } else if (zobj->encoding == REDIS_ENCODING_SKIPLIST) {

        unsigned char* zl = ziplistNew();

        if (encoding != REDIS_ENCODING_ZIPLIST)
            exit(1);

        zs = zobj->ptr;

        dictRelease(zs->dict);

        node = zs->zsl->header->level[0].forward;

        zfree(zs->zsl->header);
        zfree(zs->zsl);

        while (node) {
            ele = getDecodedObject(node->obj);

            zl = zzlInsertAt(zl, NULL, ele, node->score);
            decrRefCount(ele);

            next = node->level[0].forward;
            zslFreeNode(node);
            node = next;
        }

        zfree(zs);

        zobj->ptr = zl;
        zobj->encoding = REDIS_ENCODING_ZIPLIST;
    } else {
        exit(1);
    }
}
