//
// Created by zouyi on 2021/8/25.
//

#include <stdlib.h>
#include <string.h>
#include "intset.h"
#include "zmalloc.h"

/*
 * 返回值v所需最小编码字节/类型
 */
static uint8_t _intsetValueEncoding(int64_t v) {
    if (v < INT32_MIN || v > INT32_MAX)
        return INTSET_ENC_INT64;
    else if (v < INT16_MIN || v > INT16_MAX)
        return INTSET_ENC_INT32;
    else
        return INTSET_ENC_INT16;
}

/*
 * 以enc编码方式获取整数集合位置pos处的元素
 */
static int64_t _intsetGetEncoded(intset* is, int pos, uint8_t enc) {
    int64_t v64;
    int32_t v32;
    int16_t v16;

    if (enc == INTSET_ENC_INT64) {
        memcpy(&v64, ((int64_t*)is->contents) + pos, sizeof(v64));
        return v64;
    } else if (enc == INTSET_ENC_INT32) {
        memcpy(&v32, ((int32_t*)is->contents) + pos, sizeof(v32));
        return v32;
    } else {
        memcpy(&v16, ((int16_t*)is->contents) + pos, sizeof(v16));
        return v16;
    }
}

/*
 * 以整数集合编码方式获取整数集合位置pos处的元素
 */
static int64_t _intsetGet(intset* is, int pos) {
    return _intsetGetEncoded(is, pos, is->encoding);
}

/*
 * 以整数集合编码方式设置pos处的元素值为value
 */
static void _intsetSet(intset* is, int pos, int64_t value) {
    uint32_t encoding = is->encoding;

    if (encoding == INTSET_ENC_INT64) {
        ((int64_t*)is->contents)[pos] = value;
    } else if (encoding == INTSET_ENC_INT32) {
        ((int32_t*)is->contents)[pos] = value;
    } else {
        ((int16_t*)is->contents)[pos] = value;
    }
}

/*
 * 创建一个新的整数集合，
 * 初始化编码方式为16位，初始化长度为0
 */
intset* intsetNew(void) {
    intset* is = zmalloc(sizeof(intset));

    is->encoding = INTSET_ENC_INT16;

    is->length = 0;

    return is;
}

/*
 * 重新给整数集合分配内存，大小为len * is->encoding
 */
static intset* intsetResize(intset* is, uint32_t len) {
    uint32_t size = len * (is->encoding);

    // 使用zrealloc，原来的内容被保留，重分配效率提升(原内存空间后有足量空间直接分配)
    is = zrealloc(is, sizeof(intset) + size);

    return is;
}

/*
 * 在整数集合中搜索值为value的元素，pos中存value应该插入的位置，
 * 由于整数集合是有序存放，所以进行二分查找
 * O(logN)
 */
static uint8_t intsetSearch(intset* is, int64_t value, uint32_t* pos) {
    int min = 0, max = is->length - 1, mid = -1;
    int64_t cur = -1;

    // 如果是空整数集合，则应该插入到pos = 0位置
    if (is->length == 0) {
        if (pos) *pos = 0;
        return 0;
    // 如果超出最大值，则插入到pos = is->length；
    // 如果超出最小值，则插入到pos = 0；
    } else {
        if (value > _intsetGet(is, is->length - 1)) {
            if (pos) *pos = is->length;
            return 0;
        } else if (value < _intsetGet(is, 0)) {
            if (pos) *pos = 0;
            return 0;
        }
    }

    // 二分查找
    while (max >= min) {
        mid = (min + max) / 2;
        cur = _intsetGet(is, mid);
        if (value > cur) {
            min = mid + 1;
        } else if (value < cur) {
            max = mid - 1;
        } else {
            break;
        }
    }

    if (value == cur) {
        if (pos) *pos = mid;
        return 1;
    } else {
        if (pos) *pos = min;
        return 0;
    }
}

/*
 * 根据value需要的编码对整数集合进行升级，并将value插入整数集合中
 */
static intset* intsetUpgradeAndAdd(intset* is, int64_t value) {

    // 获取当前/升级前编码
    uint8_t curenc = is->encoding;

    // 指定需要升级到的编码
    uint8_t newenc = _intsetValueEncoding(value);

    // 获取当前长度
    int length = is->length;

    // 决定是将value插入到整数集合最前还是最后，
    // 由于value插入导致整数集合编码升级，因此原范围无法对value进行表示，value只会插入到最前或最后
    int prepend = value < 0 ? 1 : 0;

    // 更新整数集合编码
    is->encoding = newenc;
    // 根据新编码重新为整数集合分配空间
    is = intsetResize(is, is->length + 1);

    // 移动原来的元素，以新的编码方式存放到整数集合中
    while (length--)
        _intsetSet(is, length + prepend, _intsetGetEncoded(is, length, curenc));

    // 插入新元素
    if (prepend)
        _intsetSet(is, 0, value);
    else
        _intsetSet(is, is->length, value);

    // 整数集合存放的元素加1
    is->length = is->length + 1;

    return is;
}

/*
 * 移动from后的元素，到to位置
 */
static void intsetMoveTail(intset* is, uint32_t from, uint32_t to) {
    void* src;
    void* dst;

    uint32_t bytes = is->length - from;
    uint32_t encoding = is->encoding;

    if (encoding == INTSET_ENC_INT64) {
        src = (int64_t*)is->contents + from;
        dst = (int64_t*)is->contents + to;
        bytes *= sizeof(int64_t);
    } else if (encoding == INTSET_ENC_INT32) {
        src = (int32_t*)is->contents + from;
        dst = (int32_t*)is->contents + to;
        bytes *= sizeof(int32_t);
    } else {
        src = (int16_t*)is->contents + from;
        dst = (int16_t*)is->contents + to;
        bytes *= sizeof(int16_t);
    }

    /*
     * Copy N bytes of SRC to DEST, guaranteeing
     * correct behavior for overlapping strings.
     */
    memmove(dst, src, bytes);
}

/*
 * 向整数集合中插入元素value，success表示插入成功或失败
 */
intset* intsetAdd(intset* is, int64_t value, uint8_t* success) {

    uint8_t valenc = _intsetValueEncoding(value);
    uint32_t pos;

    if (success) *success = 1;

    // 元素value超出当前编码范围，进行升级
    if (valenc > is->encoding) {
        return intsetUpgradeAndAdd(is, value);
    // 元素value在当前编码范围内，无须升级
    } else {
        // 如果value已经在集合内，插入失败
        if (intsetSearch(is, value, &pos)) {
            if (success) *success = 0;
            return is;
        }

        // 扩大整数集合的容量
        is = intsetResize(is, is->length + 1);
        // 将pos之后的元素往后移动
        if (pos < is->length) intsetMoveTail(is, pos, pos + 1);
    }

    // 将value放在pos位置
    _intsetSet(is, pos, value);
    is->length = is->length + 1;

    return is;
}

/*
 * 从整数集合中删除元素value，success表示删除成功或失败
 */
intset* intsetRemove(intset* is, int64_t value, int* success) {

    uint8_t valenc = _intsetValueEncoding(value);
    uint32_t pos;

    if (success) *success = 0;

    if (valenc <= is->encoding && intsetSearch(is, value, &pos)) {
        uint32_t len = is->length;

        if (success) *success = 1;

        if (pos < (len - 1)) intsetMoveTail(is, pos + 1, pos);
        // 会调用zrealloc
        is = intsetResize(is, len - 1);
        is->length = len - 1;
    }

    return is;
}

/*
 * 在整数集合中查找元素value，返回value是否在集合中
 */
uint8_t intsetFind(intset* is, int64_t value) {
    uint8_t valenc = _intsetValueEncoding(value);
    // value如果不在整数集合编码表示范围内，则一定不存在，不需要进行二分查找
    return valenc <= is->encoding && intsetSearch(is, value, NULL);
}

int64_t intsetRandom(intset* is) {
    return _intsetGet(is, rand() % is->length);
}

uint8_t intsetGet(intset* is, uint32_t pos, int64_t* value) {
    if (pos < is->length) {
        *value = _intsetGet(is, pos);
        return 1;
    }

    return 0;
}

uint32_t intsetLen(intset* is) {
    return is->length;
}

size_t intsetBlobLen(intset* is) {
    return sizeof(intset) + is->length * is->encoding;
}
