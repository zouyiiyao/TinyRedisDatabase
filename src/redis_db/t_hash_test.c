//
// Created by zouyi on 2021/9/11.
//

#include <stdio.h>
#include "redis.h"

void printHash(robj* h) {
    hashTypeIterator* hi = hashTypeInitIterator(h);

    int count = 0;
    while (hashTypeNext(hi) == REDIS_OK) {
        if (hi->encoding == REDIS_ENCODING_HT) {
            robj* k = dictGetKey(hi->de);
            robj* v = dictGetVal(hi->de);
            printf("%ld %ld; ", (long)k->ptr, (long)v->ptr);
            count++;
        }
    }
    printf("\n");
    printf("count: %d\n", count);

    hashTypeReleaseIterator(hi);
}

int main(int argc, char* argv[]) {
    robj* h = createHashObject();
    printf("h encoding: %s h length: %lu\n", strEncoding(h->encoding), hashTypeLength(h));

    robj* keys[513];
    robj* values[513];
    for (int i = 0; i < 513; i++) {
        keys[i] = createStringObjectFromLongLong(i);
        values[i] = createStringObjectFromLongLong(i * 10);
    }

    for (int i = 0; i < 513; i++) {
        hashTypeSet(h, keys[i], values[i]);
    }

    printf("%d\n", keys[0]->refcount);

    printf("h encoding: %s h length: %lu\n", strEncoding(h->encoding), hashTypeLength(h));

    printHash(h);

    robj* k = createStringObjectFromLongLong(1024);
    robj* v = createStringObjectFromLongLong(10240);
    printf("%d\n", k->refcount);

    hashTypeSet(h, k, v);
    printf("%d\n", k->refcount);

    decrRefCount(k);
    decrRefCount(v);

    for (int i = 0; i < 513; i++) {
        decrRefCount(keys[i]);
        decrRefCount(values[i]);
    }

    decrRefCount(h);
    return 0;
}