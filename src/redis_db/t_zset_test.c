//
// Created by zouyi on 2021/9/12.
//

#include <stdio.h>
#include "redis.h"

// 顺序迭代
void printDict(dict* d) {
    // 非安全迭代器
    dictIterator* iter;
    dictEntry* entry;
    iter = dictGetIterator(d);

    while ((entry = dictNext(iter)) != NULL) {
        robj* k;
        robj* v;
        k = entry->key;
        v = entry->v.val;
        printf("iter->table: %d iter->index: %d key: %ld val: %f ", iter->table, iter->index, (long)k->ptr, *((double*)v));
    }
    printf("\n");
    dictReleaseIterator(iter);
}

void printZobj(robj* zobj) {
    if (zobj->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char* zl = zobj->ptr;
        ziplistRepr(zl);

        unsigned char* eptr = ziplistIndex(zl, 0);
        unsigned char* sptr = ziplistIndex(zl, 1);
        unsigned char* sval;
        unsigned int vlen;
        long long int lval;
        double score;

        ziplistGet(eptr, &sval, &vlen, &lval);
        score = zzlGetScore(sptr);
        printf("%lld %f ", lval, score);

        zzlNext(zl, &eptr, &sptr);

        ziplistGet(eptr, &sval, &vlen, &lval);
        score = zzlGetScore(sptr);
        printf("%lld %f ", lval, score);

        zzlNext(zl, &eptr, &sptr);

        ziplistGet(eptr, &sval, &vlen, &lval);
        score = zzlGetScore(sptr);
        printf("%lld %f ", lval, score);

        printf("\n");
    } else {
        zset* zs = zobj->ptr;
        zskiplist* zsl = zs->zsl;
        dict* dict = zs->dict;
        double* score;
        printDict(dict);

        zskiplistNode* ele1 = zslGetElementByRank(zsl, 1);
        robj* obj1 = ele1->obj;
        score = dictFetchValue(dict, obj1);
        printf("%ld %f ", (long)obj1->ptr, *score);

        zskiplistNode* ele2 = zslGetElementByRank(zsl, 2);
        robj* obj2 = ele2->obj;
        score = dictFetchValue(dict, obj2);
        printf("%ld %f ", (long)obj2->ptr, *score);

        zskiplistNode* ele3 = zslGetElementByRank(zsl, 3);
        robj* obj3 = ele3->obj;
        score = dictFetchValue(dict, obj3);
        printf("%ld %f ", (long)obj3->ptr, *score);
    }
}

int main(int argc, char* argv[]) {
    robj* zobj = createZsetZiplistObject();
    printf("zobj encoding: %s\n", strEncoding(zobj->encoding));

    robj* ele1 = createStringObjectFromLongLong(1);
    robj* ele2 = createStringObjectFromLongLong(2);
    robj* ele3 = createStringObjectFromLongLong(3);

    zobj->ptr = zzlInsert(zobj->ptr, ele1, 10.);
    zobj->ptr = zzlInsert(zobj->ptr, ele3, 30.);
    zobj->ptr = zzlInsert(zobj->ptr, ele2, 20.);

    printf("zobj length: %ul\n", zsetLength(zobj));
    printZobj(zobj);
    printf("\n");

    zsetConvert(zobj, REDIS_ENCODING_SKIPLIST);
    printf("zobj encoding: %s\n", strEncoding(zobj->encoding));
    printf("zobj length: %ul\n", zsetLength(zobj));
    printZobj(zobj);
    printf("\n");

    decrRefCount(ele1);
    decrRefCount(ele2);
    decrRefCount(ele3);

    decrRefCount(zobj);
    return 0;
}