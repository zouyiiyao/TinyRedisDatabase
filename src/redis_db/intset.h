//
// Created by zouyi on 2021/8/25.
//

#ifndef TINYREDIS_INTSET_H
#define TINYREDIS_INTSET_H

#include <stdint.h>

#define INTSET_ENC_INT16 (sizeof(int16_t))
#define INTSET_ENC_INT32 (sizeof(int32_t))
#define INTSET_ENC_INT64 (sizeof(int64_t))

/*
 * 整数集合，各个项在数组中有序排列，且不包含重复元素
 */
typedef struct intset {

    // 编码方式
    uint32_t encoding;

    // 集合中包含元素的数量
    uint32_t length;

    // 保存元素的数组，连续空间存储
    int8_t contents[];

} intset;

intset* intsetNew(void);
intset* intsetAdd(intset* is, int64_t value, uint8_t* success);
intset* intsetRemove(intset* is, int64_t value, int* success);
uint8_t intsetFind(intset* is, int64_t value);
int64_t intsetRandom(intset* is);
uint8_t intsetGet(intset* is, uint32_t pos, int64_t* value);
uint32_t intsetLen(intset* is);
size_t intsetBlobLen(intset* is);

#endif //TINYREDIS_INTSET_H
