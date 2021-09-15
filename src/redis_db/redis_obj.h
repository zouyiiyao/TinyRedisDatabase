//
// Created by zouyi on 2021/9/2.
//

#ifndef TINYREDIS_REDIS_OBJ_H
#define TINYREDIS_REDIS_OBJ_H

#define REDIS_LRU_BITS 24

typedef struct redisObject {

    unsigned type:4;

    unsigned encoding:4;

    unsigned lru:REDIS_LRU_BITS;

    int refcount;

    void* ptr;

} robj;

#endif //TINYREDIS_REDIS_OBJ_H
