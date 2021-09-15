//
// Created by zouyi on 2021/9/6.
//

#include "redis.h"

void printSet(robj* set) {
    setTypeIterator* si = setTypeInitIterator(set);
    robj* objele;
    int64_t llele;
    int enc;
    while ((enc = setTypeNext(si, &objele, &llele)) != -1) {
        if (enc == REDIS_ENCODING_HT) {
            if (sdsEncodesObject(objele)) {
                printf("%s ", (sds)objele->ptr);
            } else {
                printf("%ld ", (long)objele->ptr);
            }
        } else {
            printf("%ld ", llele);
        }
    }
    printf("\n");
    setTypeReleaseIterator(si);
}

int main(int argc, char* argv[]) {

    // 创建一个REDIS_ENCODING_INTSET编码的集合类型对象
    robj* ele1 = createStringObjectFromLongLong(1024);
    robj* set = setTypeCreate(ele1);
    setTypeAdd(set, ele1);
    printf("set encoding: %s size: %lul\n", strEncoding(set->encoding), setTypeSize(set));

    printSet(set);

    // 插入"hello"后，转为REDIS_ENCODING_HT编码
    robj* ele2 = createStringObject("hello", 5);
    printf("Add %s\n", (sds)ele2->ptr);
    setTypeAdd(set, ele2);
    printf("set encoding: %s size: %lul\n", strEncoding(set->encoding), setTypeSize(set));
    printf("hello: %d\n", setTypeIsMember(set, ele2));

    printSet(set);

    setTypeRemove(set, ele2);
    printf("set encoding: %s size: %lul\n", strEncoding(set->encoding), setTypeSize(set));
    printf("hello: %d\n", setTypeIsMember(set, ele2));

    printSet(set);

    decrRefCount(ele1);
    decrRefCount(ele2);
    decrRefCount(set);
    return 0;
}
