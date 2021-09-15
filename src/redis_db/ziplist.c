//
// Created by zouyi on 2021/8/26.
//

/*
 *  Ziplist 是为了尽可能地节约内存而设计的特殊编码双端链表
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include "zmalloc.h"
#include "utils.h"
#include "ziplist.h"

/*
 * ziplist末端标识符
 */
#define ZIP_END 255

/*
 * ziplist节点内存布局(左边为低内存地址，右边为高内存地址)
 *  
 * previous_entry_length: 保存前一个节点的长度，使用1字节或5字节(根据前一节点长度是否超过254字节决定)
 * encoding             : 编码方式，如果content是保存字节数组，还需要记录长度
 * content              : 保存实际的内容
 *
 * content保存字节数组:
 *
 * size                 1/5 bytes          1/2/5 bytes   13(01011) bytes
 *             +-----------------------+----------------+---------------+
 * component   | previous_entry_length |    encoding    |    content    |
 *             |                       |                |               |
 * value       |         ...           |    00001011    | "hello world" |
 *             +-----------------------+----------------+---------------+
 * 
 * content保存整数:
 *
 * size                 1/5 bytes            1 bytes         2 bytes
 *             +-----------------------+----------------+---------------+
 * component   | previous_entry_length |    encoding    |    content    |
 *             |                       |                |               |
 * value       |         ...           |    11000000    |     10086     |
 *             +-----------------------+----------------+---------------+
 */

/*
 * ziplist节点使用5字节前一节点长度标识阈值，注意不能与ziplist末端标识符冲突，因此最大到254
 *
 * 如果前一节点的长度小于254，previous_entry_length属性为1字节，保存前一节点的长度；
 * 如果前一节点的长度大于等于254，previous_entry_length属性为5字节，第一个字节设置为0xFE(254)，
 * 后面的四个字节保存前一节点的长度；
 */
#define ZIP_BIGLEN 254

/*
 * ziplist节点可以保存字节数组或整数，对应不同的编码方式encoding属性
 */

/*
 * 字符串编码掩码
 *
 * 11000000
 */
#define ZIP_STR_MASK 0xc0

/*
 * 字符串编码类型encoding
 *
 * 共有三种情况:
 *
 * 00bbbbbb                                    : 1字节，低 6位记录字节数组长度
 * 01bbbbbb xxxxxxxx                           : 2字节，低14位记录字节数组长度
 * 10______ aaaaaaaa bbbbbbbb cccccccc dddddddd: 5字节，低32位记录字节数组长度
 */
#define ZIP_STR_06B (0 << 6)    /* 00000000 */
#define ZIP_STR_14B (1 << 6)    /* 01000000 */
#define ZIP_STR_32B (2 << 6)    /* 10000000 */

/*
 * 整数编码类型encoding
 *
 * 共有六种情况:
 *
 * 11000000: 1字节，不需要记录长度，content属性保存int16_t类型的整数
 * 11010000: 1字节，不需要记录长度，content属性保存int32_t类型的整数
 * 11100000: 1字节，不需要记录长度，content属性保存int64_t类型的整数
 * 11110000: 1字节，不需要记录长度，content属性保存int24_t类型的整数
 * 11111110: 1字节，不需要记录长度，content属性保存int8_t 类型的整数
 */
#define ZIP_INT_16B (0xc0 | 0 << 4)    /* 11000000 */
#define ZIP_INT_32B (0xc0 | 1 << 4)    /* 11010000 */
#define ZIP_INT_64B (0xc0 | 2 << 4)    /* 11100000 */
#define ZIP_INT_24B (0xc0 | 3 << 4)    /* 11110000 */
#define ZIP_INT_8B 0xfe                /* 11111110 */

/*
 * 最后一种情况没有content属性，内容(一个介于0和12之间的值)直接保存在encoding的低4bit，
 * 注意不能与其他整数编码类型或末端标识符冲突，实际值等于encodign低4位二进制值减1
 *
 * 4 bit integer immediate encoding
 */
#define ZIP_INT_IMM_MASK 0x0f   /* 00001111 */
#define ZIP_INT_IMM_MIN 0xf1    /* 11110001 */
#define ZIP_INT_IMM_MAX 0xfd    /* 11111101 */

/*
 * 24位有符号整数表示范围
 */
#define INT24_MAX 0x7fffff
#define INT24_MIN (-INT24_MAX - 1)

/*
 * 参数为1个字节的编码类型，计算是否是字符串编码: 0: 不是 1: 是
 */
#define ZIP_IS_STR(enc) (((enc) & ZIP_STR_MASK) < ZIP_STR_MASK)

/*
 * ziplist内存布局
 *
 * zlbytes: 保存压缩列表使用的内存总字节数目
 * zltail : 保存压缩列表起始地址到尾节点地址的距离，通过zl + zltail直接可以定位到尾节点地址
 * zllen  : 当压缩列表中总节点数目小于UINT16_MAX时，保存压缩列表总节点数目
 * zlend  : 压缩列表结束标识符
 *
 * 注意: 当压缩列表中总节点数目大于等于UINT16_MAX(65535)时，总节点数目需要遍历整个压缩列表才能计算出来
 *
 * 空白 ziplist 示例图
 * 
 * area        |<---- ziplist header ---->|<-- end -->|
 * 
 * size          4 bytes   4 bytes 2 bytes  1 byte
 *             +---------+--------+-------+-----------+
 * component   | zlbytes | zltail | zllen | zlend     |
 *             |         |        |       |           |
 * value       |  1011   |  1010  |   0   | 1111 1111 |
 *             +---------+--------+-------+-----------+
 *                                        ^
 *                                        |
 *                                ZIPLIST_ENTRY_HEAD
 *                                        &
 * address                        ZIPLIST_ENTRY_TAIL
 *                                        &
 *                                ZIPLIST_ENTRY_END
 * 
 * 非空 ziplist 示例图
 * 
 * area        |<---- ziplist header ---->|<----------- entries ------------->|<-end->|
 * 
 * size          4 bytes  4 bytes  2 bytes    ?        ?        ?        ?     1 byte
 *             +---------+--------+-------+--------+--------+--------+--------+-------+
 * component   | zlbytes | zltail | zllen | entry1 | entry2 |  ...   | entryN | zlend |
 *             +---------+--------+-------+--------+--------+--------+--------+-------+
 *                                        ^                          ^        ^
 * address                                |                          |        |
 *                                 ZIPLIST_ENTRY_HEAD                |   ZIPLIST_ENTRY_END
 *                                                                   |
 *                                                         ZIPLIST_ENTRY_TAIL
 */

/*
 * 工具宏 
 *
 * Utility macros
 */
// 取压缩列表zlbytes属性
#define ZIPLIST_BYTES(zl) (*((uint32_t*)(zl)))
// 取压缩列表zltail属性
#define ZIPLIST_TAIL_OFFSET(zl) (*((uint32_t*)((zl) + sizeof(uint32_t))))
// 取压缩列表zllen属性
#define ZIPLIST_LENGTH(zl) (*((uint16_t*)((zl) + sizeof(uint32_t) * 2)))
// 常量，表示ziplist header所需字节数目，其实就是前面三个属性所占字节数的和
#define ZIPLIST_HEADER_SIZE (sizeof(uint32_t) * 2 + sizeof(uint16_t))
// 取压缩列表头节点起始地址
#define ZIPLIST_ENTRY_HEAD(zl) ((zl) + ZIPLIST_HEADER_SIZE)
// 取压缩列表尾节点起始地址
#define ZIPLIST_ENTRY_TAIL(zl) ((zl) + ZIPLIST_TAIL_OFFSET(zl))
// 取压缩列表zlend属性起始地址
#define ZIPLIST_ENTRY_END(zl) ((zl) + ZIPLIST_BYTES(zl) - 1)

/*
 * 如果当前节点数目小于UINT16_MAX，则增加压缩列表的节点数目
 */
#define ZIPLIST_INCR_LENGTH(zl, incr) { \
    if (ZIPLIST_LENGTH(zl) < UINT16_MAX) \
        ZIPLIST_LENGTH(zl) = ZIPLIST_LENGTH(zl) + incr; \
}

/*
 * 保存ziplist节点信息的结构体，解析出来的ziplist节点信息存放在这个数据结构中
 */
typedef struct zlentry {

    unsigned int prevrawlensize, prevrawlen;

    unsigned int lensize, len;

    unsigned int headersize;

    unsigned char encoding;

    unsigned char* p;

} zlentry;

/*
 * 从ptr中解析出节点值的编码类型，并存在encoding中，调用时ptr应该指向节点encoding属性的起始地址
 */
#define ZIP_ENTRY_ENCODING(ptr, encoding) do { \
    (encoding) = (ptr[0]); \
    if ((encoding) < ZIP_STR_MASK) (encoding) &= ZIP_STR_MASK; \
} while (0)

/*
 * 将整数编码类型映射到该类型编码下content需要的字节数
 */
static unsigned int zipIntSize(unsigned char encoding) {

    switch (encoding) {
        case ZIP_INT_8B:  return 1;
        case ZIP_INT_16B: return 2;
        case ZIP_INT_24B: return 3;
        case ZIP_INT_32B: return 4;
        case ZIP_INT_64B: return 8;
        default: return 0;    /* 4 bit immediate */
    }
}

/*
 * 带类型对content长度进行编码，写入起始地址为p的内存中，即编码encoding属性到p，p应该指向encoding属性起始地址，
 * 如果p为NULL，则不写入，只返回编码encoding属性需要的长度
 * encoding参数指示了content是否存放的是字节数组，rawlen指示了字节数组的长度
 */
static unsigned int zipEncodeLength(unsigned char* p, unsigned char encoding, unsigned int rawlen) {
    // 这个len是编码encoding属性需要的字节数目，buf是临时缓冲区
    unsigned char len = 1, buf[5];

    // 参数encoding是1个字节的编码类型，如果是字节数组
    if (ZIP_IS_STR(encoding)) {
        // content长度小于等于63字节，需要1个字节表示encoding属性，并且需要保存长度
        if (rawlen <= 0x3f) {
            if (!p) return len;
            // 低6位存content长度
            buf[0] = ZIP_STR_06B | rawlen;
        // content长度小于等于16383字节，需要2个字节表示encoding属性，并且需要保存长度
        } else if (rawlen <= 0x3fff) {
            len += 1;
            if (!p) return len;
            // 低14位存content长度
            buf[0] = ZIP_STR_14B | ((rawlen >> 8) & 0x3f);
            buf[1] = rawlen & 0xff;
        // content长度小于等于4294967295字节，需要5个字节表示encoding属性，并且需要保存长度
        } else {
            len += 4;
            if (!p) return len;
            // 低32位存content长度，注意对应的是高内存地址
            buf[0] = ZIP_STR_32B;
            buf[1] = (rawlen >> 24) & 0xff;
            buf[2] = (rawlen >> 16) & 0xff;
            buf[3] = (rawlen >> 8) & 0xff;
            buf[4] = rawlen & 0xff;
        }

    // 如果是整数
    } else {
        // 只需要1个字节表示encoding属性，并且不需要保存长度
        /* Implies integer encoding, so length is always 1. */
        if (!p) return len;
        buf[0] = encoding;
    }

    // 写入起始地址为p的内存中
    memcpy(p, buf, len);

    return len;
}

/*
 * 从ptr解码encoding属性，ptr应该指向encoding属性起始地址，
 * encoding保存1字节的编码类型，lensize保存对应的编码类型需要多少个字节保存content长度，len保存content长度
 */
#define ZIP_DECODE_LENGTH(ptr, encoding, lensize, len) do { \
    ZIP_ENTRY_ENCODING((ptr), (encoding));                  \
                                                            \
    if ((encoding) < ZIP_STR_MASK) {                        \
        if ((encoding) == ZIP_STR_06B) {                    \
            (lensize) = 1;                                  \
            (len) = (ptr)[0] & 0x3f;                        \
        } else if ((encoding) == ZIP_STR_14B) {             \
            (lensize) = 2;                                  \
            (len) = (((ptr)[0] & 0x3f) << 8) | (ptr)[1];    \
        } else if ((encoding) == ZIP_STR_32B) {             \
            (lensize) = 5;                                  \
            (len) = ((ptr)[1] << 24) |                      \
                    ((ptr)[2] << 16) |                      \
                    ((ptr)[3] <<  8) |                      \
                    ((ptr)[4]);                             \
        }                                                   \
        else {                                              \
            assert(NULL);                                   \
        }                                                   \
    } else {                                                \
        (lensize) = 1;                                      \
        (len) = zipIntSize(encoding);                       \
    }                                                       \
} while (0)

/*
 * 编码前一节点长度len(即previous_entry_length属性)到p，
 * 如果p为NULL，则不写入，只返回编码需要的长度
 */
static unsigned int zipPrevEncodeLength(unsigned char* p, unsigned int len) {

    if (p == NULL) {
        return (len < ZIP_BIGLEN) ? 1 : sizeof(len) + 1;
    } else {
        // 前一节点长度小于254字节，只需要1 byte
        if (len < ZIP_BIGLEN) {
            p[0] = len;
            return 1;
        // 前一节点长度大于等于254字节，需要5 byte
        } else {
            // 设置使用5字节编码前一节点长度标识，位于previous_entry_length的第1个字节
            p[0] = ZIP_BIGLEN;
            // 剩下的4个字节用来存放前一节点的长度
            memcpy(p + 1, &len, sizeof(len));
            return 1 + sizeof(len);
        }
    }
}

/*
 * 将原本只需要1个字节编码的previous_entry_length属性改为5字节编码
 */
static void zipPrevEncodeLengthForceLarge(unsigned char* p, unsigned int len) {

    if (p == NULL) return;

    p[0] = ZIP_BIGLEN;

    memcpy(p + 1, &len, sizeof(len));
}

/*
 * 从ptr解码出previous_entry_length属性编码长度，ptr应该指向previous_entry_length属性的第一个字节(即节点起始地址)
 */
#define ZIP_DECODE_PREVLENSIZE(ptr, prevlensize) do { \
    if ((ptr)[0] < ZIP_BIGLEN) {                      \
        (prevlensize) = 1;                            \
    } else {                                          \
        (prevlensize) = 5;                            \
    }                                                 \
} while (0)

/*
 * 从ptr解码出previous_entry_length属性前一节点长度，ptr应该指向previous_entry_length属性的第一个字节
 */
#define ZIP_DECODE_PREVLEN(ptr, prevlensize, prevlen) do { \
    ZIP_DECODE_PREVLENSIZE(ptr, prevlensize);              \
                                                           \
    if ((prevlensize) == 1) {                              \
        (prevlen) = (ptr)[0];                              \
    } else if ((prevlensize) == 5) {                       \
        assert(sizeof((prevlensize)) == 4);                \
        memcpy(&(prevlen), ((char*)(ptr)) + 1, 4);         \
    }                                                      \
} while (0)

/*
 * 计算编码新的前置节点长度len所需字节数目与编码p(p应该指向节点的previous_entry_length属性的第一个字节)原来
 * 前置节点长度len所需字节数目之差
 */
static int zipPrevLenByteDiff(unsigned char* p, unsigned int len) {
    unsigned int prevlensize;

    ZIP_DECODE_PREVLENSIZE(p, prevlensize);

    return zipPrevEncodeLength(NULL, len) - prevlensize;
}

/*
 * 返回p指向的ziplist节点编码的总长度，包括3部分: 
 * 编码前一个节点长度prevlensize所需字节数；
 * 编码content长度lensize所需字节数；
 * 保存content所需字节数len；
 */
static unsigned int zipRawEntryLength(unsigned char* p) {
    unsigned int prevlensize, encoding, lensize, len;

    ZIP_DECODE_PREVLENSIZE(p, prevlensize);

    ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);

    return prevlensize + lensize + len;
}

/*
 * 检查entry指向的字符串能否编码为整数，这里entry并不是表示节点，
 * 将编码后的整数保存在指针v中，将encoding属性写入指针encoding
 */
static int zipTryEncoding(unsigned char* entry, unsigned int entrylen, long long* v, unsigned char* encoding) {
    long long value;

    if (entrylen >= 32 || entrylen == 0) return 0;

    if (string2ll((char*)entry, entrylen, &value)) {
        /* Great, the string can be encoded. Check what's the smallest
         * of our encoding types that can hold this value. */
        if (value >= 0 && value <= 12) {
            *encoding = ZIP_INT_IMM_MIN + value;
        } else if (value >= INT8_MIN && value <= INT8_MAX) {
            *encoding = ZIP_INT_8B;
        } else if (value >= INT16_MIN && value <= INT16_MAX) {
            *encoding = ZIP_INT_16B;
        } else if (value >= INT24_MIN && value <= INT24_MAX) {
            *encoding = ZIP_INT_24B;
        } else if (value >= INT32_MIN && value <= INT32_MAX) {
            *encoding = ZIP_INT_32B;
        } else {
            *encoding = ZIP_INT_64B;
        }

        *v = value;

        return 1;
    }

    return 0;
}

/*
 * 以encoding指定的编码方式，将整数值value写入指针p，p应该指向content属性起始地址
 */
static void zipSaveInteger(unsigned char* p, int64_t value, unsigned char encoding) {
    int16_t i16;
    int32_t i32;
    int64_t i64;

    if (encoding == ZIP_INT_8B) {
        ((int8_t*)p)[0] = (int8_t)value;
    } else if (encoding == ZIP_INT_16B) {
        i16 = value;
        memcpy(p, &i16, sizeof(i16));
    } else if (encoding == ZIP_INT_24B) {
        // 小端法，低字节在低地址处
        i32 = value << 8;
        memcpy(p, ((uint8_t*)&i32) + 1, sizeof(i32) - sizeof(uint8_t));
    } else if (encoding == ZIP_INT_32B) {
        i32 = value;
        memcpy(p, &i32, sizeof(i32));
    } else if (encoding == ZIP_INT_64B) {
        i64 = value;
        memcpy(p, &i64, sizeof(i64));
    } else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
        /* Nothing to do, the value is stored in the encoding itself. */
        // 内容在encoding属性中
    } else {
        assert(NULL);
    }
}

/*
 * 以encoding编码方式，从指针p处读取整数值，p应该指向content属性起始地址
 */
static int64_t zipLoadInteger(unsigned char* p, unsigned char encoding) {
    int16_t i16;
    int32_t i32;
    int64_t i64;
    int64_t ret = 0;

    if (encoding == ZIP_INT_8B) {
        ret = ((int8_t*)p)[0];
    } else if (encoding == ZIP_INT_16B) {
        memcpy(&i16, p, sizeof(i16));
        ret = i16;
    } else if (encoding == ZIP_INT_32B) {
        memcpy(&i32, p, sizeof(i32));
        ret = i32;
    } else if (encoding == ZIP_INT_24B) {
        i32 = 0;
        memcpy(((uint8_t*)&i32) + 1, p, sizeof(i32) - sizeof(uint8_t));
        ret = i32 >> 8;
    } else if (encoding == ZIP_INT_64B) {
        memcpy(&i64, p, sizeof(i64));
        ret = i64;
    } else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
        // 内容在encoding属性中
        ret = (encoding & ZIP_INT_IMM_MASK) - 1;
    } else {
        assert(NULL);
    }

    return ret;
}

/*
 * 从指针p处读取一个ziplist节点，p应该指向一个ziplist节点起始地址
 */
static zlentry zipEntry(unsigned char* p) {
    zlentry e;

    ZIP_DECODE_PREVLEN(p, e.prevrawlensize, e.prevrawlen);

    ZIP_DECODE_LENGTH(p + e.prevrawlensize, e.encoding, e.lensize, e.len);

    e.headersize = e.prevrawlensize + e.lensize;

    e.p = p;

    return e;
}

/*
 * 创建一个空的ziplist
 */
unsigned char* ziplistNew(void) {
    unsigned int bytes = ZIPLIST_HEADER_SIZE + 1;

    unsigned char* zl = zmalloc(bytes);

    ZIPLIST_BYTES(zl) = bytes;
    ZIPLIST_TAIL_OFFSET(zl) = ZIPLIST_HEADER_SIZE;
    ZIPLIST_LENGTH(zl) = 0;

    zl[bytes - 1] = ZIP_END;

    return zl;
}

/*
 * 将指定的压缩列表zl重新resize到len字节大小，返回新的起始地址
 */
static unsigned char* ziplistResize(unsigned char* zl, unsigned int len) {
    // 这里的len指的是字节数size，使用zrealloc，所以内容已经拷贝
    zl = zrealloc(zl, len);

    // 更新zl使用的总字节数
    ZIPLIST_BYTES(zl) = len;

    // 设置zl结束标识
    zl[len - 1] = ZIP_END;

    return zl;
}

/*
 * 压缩列表级联更新
 *
 * 当将一个新节点添加到某个节点之前时，如果原节点的prevlen属性的长度不够保存新节点的长度
 * (如当前1字节，需要5字节)，则需要对prevlen属性进行扩展，但是对原节点进行扩展后，原节点
 * 的下一个节点也可能出现prevlen属性长度不够的问题，这种情况在多个连续节点的长度都接近
 * ZIP_BIGLEN的时候有可能会发生，这个函数用来检查并修复后续节点的空间不足问题；
 *
 * 反过来说，因为新节点的长度更小而导致的连续缩小也有可能出现，不过，为了避免扩展->缩小->
 * 扩展->缩小这种情况出现(抖动)，我们不处理这种情况，即任由prevlen属性的长度比所需的更长；
 *
 * 注意: 程序的检查是针对p的后续节点，而不是p所指向的节点，因为节点p在传入之前已经完成了
 * 所需的空间扩展工作
 */
static unsigned char* __ziplistCascadeUpdate(unsigned char* zl, unsigned char* p) {
    // 获取当前压缩列表使用字节数curlen
    size_t curlen = ZIPLIST_BYTES(zl);
    size_t rawlen, rawlensize;
    size_t offset, noffset, extra;
    unsigned char* np;
    zlentry cur, next;

    // 从p指向的节点开始遍历
    while (p[0] != ZIP_END) {
        // 取当前节点cur
        cur = zipEntry(p);
        // 取当前节点总字节数rawlen
        rawlen = cur.headersize + cur.len;
        // 取rawlen编码到prevlen属性所需的字节数rawlensize
        rawlensize = zipPrevEncodeLength(NULL, rawlen);

        // 如果没有后续节点了，跳出循环
        /* Abort if there is no next entry. */
        if (p[rawlen] == ZIP_END) break;

        // 取cur的下一节点next
        next = zipEntry(p + rawlen);

        // 如果下一节点next的prevlen属性刚好与rawlen相等，则后续不用更新，直接跳出
        if (next.prevrawlen == rawlen) break;

        // 如果下一节点next的prevlen属性长度小于rawlensize，说明需要扩展
        if (next.prevrawlensize < rawlensize) {
            // 取当前节点起始地址到压缩列表起始地址的偏移offset
            offset = p - zl;
            // 计算需要增加的字节数extra
            extra = rawlensize - next.prevrawlensize;
            // 对压缩列表内存进行扩展，由于调用的是zrealloc，所以内容已被拷贝，
            // 而且这一步会进行压缩列表的zlbytes属性和zlend属性的赋值
            zl = ziplistResize(zl, curlen + extra);
            // p重新指向新分配内存空间的压缩列表中当前节点的起始地址
            p = zl + offset;

            // np指向下一节点的起始地址
            np = p + rawlen;
            // 取下一节点起始地址到新分配内存空间的压缩列表起始地址的偏移noffset
            noffset = np - zl;

            // 如果下一节点np不是尾节点(则尾节点的偏移要发生变化)，
            // 扩展的是np，需不需要改尾节点的偏移取决于尾节点起始地址之前的内存字节数目有没有变化
            if ((zl + ZIPLIST_TAIL_OFFSET(zl)) != np) {
                // 更新尾节点的偏移
                ZIPLIST_TAIL_OFFSET(zl) = ZIPLIST_TAIL_OFFSET(zl) + extra;
            }

            // 进行内存拷贝，即把下一节点prevlen属性之后的内容向后移动，为存放新的前一节点长度rawlen腾出位置
            memmove(np + rawlensize, np + next.prevrawlensize, curlen - noffset - next.prevrawlensize - 1);
            // 将rawlen编码到下一节点起始地址np(即下一节点np的prevlen属性起始地址)
            zipPrevEncodeLength(np, rawlen);

            // 处理下一节点
            p += rawlen;
            // 更新当前压缩链表使用总字节数
            curlen += extra;
        } else {
            // 如果下一节点的prevlen属性长度大于rawlensize，说明其存放rawlen绰绰有余，
            // 注意，为了防止抖动，不对其进行缩小操作
            if (next.prevrawlensize > rawlensize) {
                // 把rawlen(1字节)编码到更大(5字节)的下一节点的prevlen属性中
                zipPrevEncodeLengthForceLarge(p + rawlen, rawlen);
            // 如果下一节点的prevlen属性长度等于rawlensize，说明其存放rawlen刚刚好
            } else {
                // 把rawlen编码到下一节点的prevlen属性中
                zipPrevEncodeLength(p + rawlen, rawlen);
            }
            
            // 走到这里，说明内存已经调整完毕，可以跳出循环
            break;
        }
    }

    return zl;
}

/*
 * 从位置p开始，连续删除num个节点
 */
static unsigned char* __ziplistDelete(unsigned char* zl, unsigned char* p, unsigned int num) {
    unsigned int i, totlen, deleted = 0;
    size_t offset;
    int nextdiff = 0;
    zlentry first, tail;

    // 取第一个被删除的节点first
    first = zipEntry(p);
    for (i = 0; p[0] != ZIP_END && i < num; i++) {
        p += zipRawEntryLength(p);
        deleted++;
    }
    // deleted表示实际删除的节点数目

    // totlen表示删除的字节数目
    totlen = p - first.p;
    if (totlen > 0) {
        // 删除后，被删除节点后至少还有一个节点
        if (p[0] != ZIP_END) {

            // 计算被删除节点后第一个节点的prevlen属性字节数目与编码被删除节点的前一个节点的长度所需字节数目之差
            nextdiff = zipPrevLenByteDiff(p, first.prevrawlen);
            // p指针指向被删除节点后第一个节点的首地址，需要的话对p指针进行移动，为保存被删除节点的前一个节点的长度
            // 腾出空间，这里是安全的，考虑nextdiff不为0的情况，只有可能为4，并且被删除节点后的第一个节点的prevlen
            // 属性字节数目为1而编码被删除节点的前一个节点的长度所需字节数目为5，那么被删除节点的长度至少为6，因为
            // prevlen属性字节数目为5，encoding属性至少为1
            p -= nextdiff;
            // 将被删除节点的前一个节点的长度编码进被删除节点后第一个节点的prevlen属性
            zipPrevEncodeLength(p, first.prevrawlen);
 
            // 更新到尾节点的偏移量
            ZIPLIST_TAIL_OFFSET(zl) = ZIPLIST_TAIL_OFFSET(zl) - totlen;

            // 取被删除节点后第一个节点
            tail = zipEntry(p);
            
            // 如果被删除节点后的第一个节点不是尾节点(则尾节点的偏移要发生变化)
            if (p[tail.headersize + tail.len] != ZIP_END) {
                // 更新尾节点的偏移
                ZIPLIST_TAIL_OFFSET(zl) = ZIPLIST_TAIL_OFFSET(zl) + nextdiff;
            }

            // 进行内存拷贝
            memmove(first.p, p, ZIPLIST_BYTES(zl) - (p - zl) - 1);
        // 删除后，被删除节点后没有节点
        } else {
            // 只需要修改尾节点的偏移，不需要进行内存拷贝
            ZIPLIST_TAIL_OFFSET(zl) = (first.p - zl) - first.prevrawlen;
        }

        // 取被删除第一个节点起始地址(现在已经被覆盖，是被删除节点后的第一个节点)到压缩列表zl起始地址偏移量offset
        offset = first.p - zl;
        // 内存缩小
        zl = ziplistResize(zl, ZIPLIST_BYTES(zl) - totlen + nextdiff);
        // 修改压缩列表zllen属性，节点数减少了deleted
        ZIPLIST_INCR_LENGTH(zl, -deleted);
        // p指向重新分配内存的压缩列表的被删除节点后的第一个节点的起始地址
        p = zl + offset;

        // p的空间已经分配，如果nextdiff != 0，说明p指向的节点prevlen属性字节数进行了扩展，
        // 那么p之后的节点都可能需要扩展(删除节点时可能产生级联更新)
        if (nextdiff != 0)
            zl = __ziplistCascadeUpdate(zl, p);
    }

    return zl;
}

/*
 * 插入一个元素(由长度为slen的字符串表示，起始地址是s)到p指向的元素前，
 * 如果能够用整数编码，则转化为整数编码，然后插入
 */
static unsigned char* __ziplistInsert(unsigned char* zl, unsigned char* p, unsigned char* s, unsigned int slen) {
    size_t curlen = ZIPLIST_BYTES(zl), reqlen, prevlen = 0;
    size_t offset;
    int nextdiff = 0;
    // 初始化为字符串编码
    unsigned char encoding = 0;
    long long value = 123456789; /* initialized to avoid warning. Using a value
                                    that is easy to see if for some reason
                                    we use it uninitialized. */
    zlentry entry, tail;

    // 如果列表非空，且p正指向列表中的一个节点
    if (p[0] != ZIP_END) {
        // 取p指向的节点entry
        entry = zipEntry(p);
        // 取p指向的节点的前一个节点长度
        prevlen = entry.prevrawlen;
    } else {
        unsigned char* ptail = ZIPLIST_ENTRY_TAIL(zl);
        if (ptail[0] != ZIP_END) {
            // 如果p指向列表终止标识，且列表非空，取p的前一个节点(列表尾元素)长度
            prevlen = zipRawEntryLength(ptail);
        }
    }

    // 尝试将s编码为整数
    if (zipTryEncoding(s, slen, &value, &encoding)) {
        // 如果编码成功，value保存编码后的整数，encoding保存编码方式
        reqlen = zipIntSize(encoding);
    } else {
        reqlen = slen;
    }
    // 无论编码成功与否，reqlen都保存新节点的content属性字节数目

    // reqlen加上编码插入的前一个节点长度所需字节数(新节点的prevlen属性字节数目)
    reqlen += zipPrevEncodeLength(NULL, prevlen);
    // reqlen加上编码使用encoding编码方式编码长度为slen内容的长度所需字节数(新节点的encoding属性字节数目)
    reqlen += zipEncodeLength(NULL, encoding, slen);

    // 若p不指向列表终止标识，新节点需要插入到p指向的节点之前，检查p的prevlen属性是否需要扩展
    nextdiff = (p[0] != ZIP_END) ? zipPrevLenByteDiff(p, reqlen) : 0;

    // 为压缩列表重新分配足量的内存空间，新节点插入到p指向的节点之前
    offset = p - zl;
    zl = ziplistResize(zl, curlen + reqlen + nextdiff);
    p = zl + offset;

    // 如果p指向的是压缩列表中的节点
    if (p[0] != ZIP_END) {
        // 内存拷贝，从下一节点起始地址开始的内容拷贝到p + reqlen处，新节点起始地址从p开始
        memmove(p + reqlen, p - nextdiff, curlen - offset - 1 + nextdiff);
        // 编码新节点的下一节点的prevlen属性
        zipPrevEncodeLength(p + reqlen, reqlen);

        // 更新尾节点的偏移
        ZIPLIST_TAIL_OFFSET(zl) = ZIPLIST_TAIL_OFFSET(zl) + reqlen;

        // tail指向新节点的下一节点
        tail = zipEntry(p + reqlen);
        // 如果tail不是尾节点，需要更新尾节点的偏移
        if (p[reqlen + tail.headersize + tail.len] != ZIP_END) {
            ZIPLIST_TAIL_OFFSET(zl) = ZIPLIST_TAIL_OFFSET(zl) + nextdiff;
        }
    // 如果p指向的是压缩列表的终止标识
    } else {
        // 新节点作为新的尾节点，更新尾节点的偏移
        ZIPLIST_TAIL_OFFSET(zl) = p - zl;
    }

    // 新节点的下一节点的prevlen属性的长度需要扩展
    if (nextdiff != 0) {
        offset = p - zl;
        // 增加节点可能导致级联更新
        zl = __ziplistCascadeUpdate(zl, p + reqlen);
        p = zl + offset;
    }

    // 编码新节点的前一节点长度(prevlen属性)
    p += zipPrevEncodeLength(p, prevlen);
    // 编码新节点的encoding属性
    p += zipEncodeLength(p, encoding, slen);
    // 编码新节点的content属性
    if (ZIP_IS_STR(encoding)) {
        memcpy(p, s, slen);
    } else {
        zipSaveInteger(p, value, encoding);
    }

    // 压缩列表节点数目加1
    ZIPLIST_INCR_LENGTH(zl, 1);

    return zl;
}

/*
 * 向压缩列表zl中推入元素，可以选择左边或右边，
 * 调用__ziplistInsert完成实际的工作
 */
unsigned char* ziplistPush(unsigned char* zl, unsigned char* s, unsigned int slen, int where) {
    unsigned char* p;
    p = (where == ZIPLIST_HEAD) ? ZIPLIST_ENTRY_HEAD(zl) : ZIPLIST_ENTRY_END(zl);

    return __ziplistInsert(zl, p, s, slen);
}

/*
 * 遍历列表，返回索引指定节点的指针
 * 支持正向迭代和反向迭代
 */
unsigned char* ziplistIndex(unsigned char* zl, int index) {
    unsigned char* p;

    zlentry entry;

    if (index < 0) {
        index = (-index) - 1;
        p = ZIPLIST_ENTRY_TAIL(zl);

        if (p[0] != ZIP_END) {
            entry = zipEntry(p);
            while (entry.prevrawlen > 0 && index--) {
                p -= entry.prevrawlen;
                entry = zipEntry(p);
            }
        }
    } else {
        p = ZIPLIST_ENTRY_HEAD(zl);

        while (p[0] != ZIP_END && index--) {
            p += zipRawEntryLength(p);
        }
    }

    return (p[0] == ZIP_END || index > 0) ? NULL : p;
}

/*
 * 返回p指向节点的下一节点的指针
 */
unsigned char* ziplistNext(unsigned char* zl, unsigned char* p) {
    // 避免编译器错误
    ((void) zl);

    if (p[0] == ZIP_END) {
        return NULL;
    }

    p += zipRawEntryLength(p);
    if (p[0] == ZIP_END) {
        return NULL;
    }

    return p;
}

/*
 * 返回p指向节点的上一节点的指针
 */
unsigned char* ziplistPrev(unsigned char* zl, unsigned char* p) {
    zlentry entry;

    if (p[0] == ZIP_END) {
        p = ZIPLIST_ENTRY_TAIL(zl);
        return (p[0] == ZIP_END) ? NULL : p;
    } else if (p == ZIPLIST_ENTRY_HEAD(zl)) {
        return NULL;
    } else {
        entry = zipEntry(p);
        assert(entry.prevrawlen > 0);
        return p - entry.prevrawlen;
    }
}

/*
 * 取出p指向节点的值
 * 如果节点保存的是字符串，则将字符串首地址保存在*sstr中，字符串长度保存在*slen中；
 * 如果节点保存的是整数，那么将整数保存到*sval；
 * 通过检查*sstr是否为NULL来确认值是字符串还是整数
 */
unsigned int ziplistGet(unsigned char* p, unsigned char** sstr, unsigned int* slen, long long* sval) {

    zlentry entry;
    if (p == NULL || p[0] == ZIP_END) return 0;
    if (sstr) *sstr = NULL;

    entry = zipEntry(p);

    if (ZIP_IS_STR(entry.encoding)) {
        if (sstr) {
            *slen = entry.len;
            *sstr = p + entry.headersize;
        }
    } else {
        if (sval) {
            *sval = zipLoadInteger(p + entry.headersize, entry.encoding);
        }
    }

    return 1;
}

/*
 * 将一个新元素插入到zl中p所指向的节点之前
 */
unsigned char* ziplistInsert(unsigned char* zl, unsigned char* p, unsigned char* s, unsigned int slen) {
    return __ziplistInsert(zl, p, s, slen);
}

/*
 * 从zl中删除*p指向的节点，并且原地更新*p指向的位置，使得可以在迭代列表的过程中对节点进行删除
 */
unsigned char* ziplistDelete(unsigned char* zl, unsigned char** p) {
    size_t offset = *p - zl;
    zl = __ziplistDelete(zl, *p, 1);

    *p = zl + offset;

    return zl;
}

/*
 * 从zl中删除从index索引指向的节点开始的连续num个节点
 */
unsigned char* ziplistDeleteRange(unsigned char* zl, unsigned int index, unsigned int num) {
    unsigned char* p = ziplistIndex(zl, index);

    return (p == NULL) ? zl : __ziplistDelete(zl, p, num);
}

/*
 * 将p所指向的节点值与sstr进行对比
 */
unsigned int ziplistCompare(unsigned char* p, unsigned char* sstr, unsigned int slen) {
    zlentry entry;
    unsigned char sencoding;
    long long zval, sval;
    if (p[0] == ZIP_END) return 0;

    entry = zipEntry(p);
    if (ZIP_IS_STR(entry.encoding)) {
        if (entry.len == slen) {
            return memcmp(p + entry.headersize, sstr, slen) == 0;
        } else {
            return 0;
        }
    } else {
        if (zipTryEncoding(sstr, slen, &sval, &sencoding)) {
            zval = zipLoadInteger(p + entry.headersize, entry.encoding);
            return zval == sval;
        }
    }

    return 0;
}

/*
 * 从p指向的节点开始迭代，寻找节点值与vstr相等的列表节点，返回指向该节点的指针
 * 每次跳过skip个节点
 */
unsigned char* ziplistFind(unsigned char* p, unsigned char* vstr, unsigned int vlen, unsigned int skip) {
    int skipcnt = 0;
    unsigned char vencoding = 0;
    long long vll = 0;

    // 迭代直到达到链表尾
    while (p[0] != ZIP_END) {
        unsigned int prevlensize, encoding, lensize, len;
        unsigned char* q;

        ZIP_DECODE_PREVLENSIZE(p, prevlensize);
        ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
        // q指向节点的content属性起始地址
        q = p + prevlensize + lensize;

        if (skipcnt == 0) {
            // 当前节点是字符串编码
            if (ZIP_IS_STR(encoding)) {
                if (len == vlen && memcmp(q, vstr, vlen) == 0) {
                    return p;
                }
            // 当前节点是整数编码
            } else {
                if (vencoding == 0) {
                    if (!zipTryEncoding(vstr, vlen, &vll, &vencoding)) {
                        /* If the entry can't be encoded we set it to
                         * UCHAR_MAX so that we don't retry again the next
                         * time. */
                        vencoding = UCHAR_MAX;
                    }
                    assert(vencoding);
                }

                // 不用每次都比较，只需要尝试一次将vstr编码为整数，如果失败把vencoding设为UCAHR_MAX
                if (vencoding != UCHAR_MAX) {
                    long long ll = zipLoadInteger(q, encoding);
                    if (ll == vll) {
                        return p;
                    }
                }
            }

            skipcnt = skip;
        } else {
            skipcnt--;
        }
        p = q + len;
    }

    return NULL;
}

/*
 * 返回ziplist的总节点数目，注意如果zllen属性等于UINT16_MAX，真正的长度需要遍历整个ziplist才能得到
 */
unsigned int ziplistLen(unsigned char* zl) {

    unsigned int len = 0;

    if (ZIPLIST_LENGTH(zl) < UINT16_MAX) {
        len = ZIPLIST_LENGTH(zl);
    } else {
        unsigned char* p = zl + ZIPLIST_HEADER_SIZE;
        while (*p != ZIP_END) {
            p += zipRawEntryLength(p);
            len++;
        }

        // 如果发现总节点数目可以用zllen属性(2字节)表示，则修改之
        if (len < UINT16_MAX) ZIPLIST_LENGTH(zl) = len;
    }

    return len;
}

/*
 * 返回ziplist使用的字节总数，就是zlbytes属性
 */
size_t ziplistBlobLen(unsigned char* zl) {
    return ZIPLIST_BYTES(zl);
}

/*
 * 打印ziplist内容
 */
void ziplistRepr(unsigned char *zl) {
    unsigned char *p;
    int index = 0;
    zlentry entry;

    printf(
            "{total bytes %d} "
            "{length %u}\n"
            "{tail offset %u}\n",
            ZIPLIST_BYTES(zl),
            ZIPLIST_LENGTH(zl),
            ZIPLIST_TAIL_OFFSET(zl));
    p = ZIPLIST_ENTRY_HEAD(zl);
    while(*p != ZIP_END) {
        entry = zipEntry(p);
        printf(
                "{"
                "addr 0x%08lx, "
                "index %2d, "
                "offset %5ld, "
                "rl: %5u, "
                "hs %2u, "
                "pl: %5u, "
                "pls: %2u, "
                "payload %5u"
                "} ",
                (long unsigned)p,
                index,
                (unsigned long) (p-zl),
                entry.headersize+entry.len,
                entry.headersize,
                entry.prevrawlen,
                entry.prevrawlensize,
                entry.len);
        p += entry.headersize;
        if (ZIP_IS_STR(entry.encoding)) {
            if (entry.len > 256) {
                if (fwrite(p,256,1,stdout) == 0) perror("fwrite");
                printf("...");
            } else {
                if (entry.len &&
                    fwrite(p,entry.len,1,stdout) == 0) perror("fwrite");
            }
        } else {
            printf("%lld", (long long) zipLoadInteger(p,entry.encoding));
        }
        printf("\n");
        p += entry.len;
        index++;
    }
    printf("{end}\n\n");
}
