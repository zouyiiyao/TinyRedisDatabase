//
// Created by zouyi on 2021/8/15.
//

#ifndef TINY_REDIS_DICT_H
#define TINY_REDIS_DICT_H

#include <stdint.h>

/*
 * 表示字典操作的状态，0: 操作成功 1: 操作失败
 */
#define DICT_OK 0
#define DICT_ERR 1

/*
 * 如果字典的私有数据不使用，用这个宏定义来避免编译器错误
 */
#define DICT_NOTUSED(V) ((void) V)

/*
 * 哈希表节点结构定义
 */
typedef struct dictEntry {

    // 指向键
    void* key;

    // 指向值
    // C语言技巧，通过联合体共享内存
    union {
        void* val;
        uint64_t u64;
        int64_t s64;
    } v;

    // 指向下一个哈希冲突的节点
    // 通过开链法解决冲突
    struct dictEntry* next;

} dictEntry;

/*
 * 字典类型特定函数
 */
typedef struct dictType {

    // 计算哈希值的特定函数
    unsigned int (*hashFunction)(const void* key);

    // 复制键的特定函数
    void* (*keyDup)(void* privdata, const void* key);

    // 复制值的特定函数
    void* (*valDup)(void* privdata, const void* obj);

    // 对比键的特定函数
    int (*keyCompare)(void* privdata, const void* key1, const void* key2);

    // 销毁键的特定函数
    void (*keyDestructor)(void* privdata, void* key);

    // 销毁值的特定函数
    void (*valDestructor)(void* privdata, void* obj);

} dictType;

/*
 * 哈希表
 */
typedef struct dictht {

    // 指针数组，每一项是一个桶，表示一个哈希节点的链表
    dictEntry** table;

    // 哈希表大小，即桶的数目
    unsigned long size;

    // 哈希表大小掩码，用来计算链接到哪一个桶的索引，(除了初始化时)总是应该等于size - 1
    unsigned long sizemask;

    // 哈希表已有节点的数目
    unsigned long used;

} dictht;
/*
 * 拓展
 * 
 * 负载因子 = 哈希表已有节点的数目 / 哈希表大小，负载因子衡量了一个哈希表存放键值对发生冲突的严重程度，
 * 负载因子越大，搜索一个键值对所花的平均时间就越长，通过key计算哈希值->通过&计算索引值->搜索整个桶，比对键值是否相同，
 * 一般负载因子 > 1时，就需要考虑为字典扩容了
 */

/*
 * 字典
 */
typedef struct dict {

    // 特定函数
    dictType* type;

    // 私有数据，目前不使用
    void* privdata;

    // 每个字典使用两个哈希表，在需要的时候进行渐进式rehash
    dictht ht[2];

    // 索引，标志渐进式rehash进度，-1: 未进行rehash
    int rehashidx;

    // 目前正在运行的安全迭代器数量，指在迭代过程中可能增加删除节点，对字典进行修改
    int iterators;

} dict;


/*
 * 字典迭代器
 *
 * 其中safe属性指示了是否是安全迭代器，0:非安全/只读 1:安全
 * 如果safe属性为1，那么在迭代进行的过程中，程序仍然可以执行对字典进行查找或修改的函数，会禁止rehash
 * 如果safe属性为0，那么在迭代进行的过程中，程序只会调用dictNext对字典进行迭代，而不进行修改，不禁止rehash
 * 注意: 允许rehash带来的副作用是迭代过程中可能返回重复元素，即在table[0]中遍历过的桶rehash到table[1]又被重新遍历一次
 */
typedef struct dictIterator {

    // 指向被迭代的字典
    dict* d;

    // table: 正在被迭代的哈希表索引，枚举值0或1
    // index: 迭代器当前所指向的哈希表索引位置
    // safe : 标识这个迭代器是否安全
    int table, index, safe;

    // 指向当前迭代到的节点
    dictEntry* entry;

    // 指向当前迭代到的节点的下一节点，
    // 该属性是必要的，因为安全迭代器迭代过程中可能删除当前节点，则下一节点指针丢失
    dictEntry* nextEntry;

    // 指纹，不安全迭代器使用，以防在迭代过程中误用对字典修改的函数
    long long fingerprint;

} dictIterator;

/*
 * 函数类型
 */
typedef void (dictScanFunction)(void* privdata, const dictEntry* de);

/*
 * 宏定义
 */

// 哈希表的初始大小
#define DICT_HT_INITIAL_SIZE 4

// 释放字典节点的值
#define dictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((d)->privdata, (entry)->v.val)

// 设置字典节点的值
// C语言技巧，使用do { ... } while (0)来定义复杂的宏
#define dictSetVal(d, entry, _val_) do { \
    if ((d)->type->valDup) \
        entry->v.val = (d)->type->valDup((d)->privdata, _val_); \
    else \
        entry->v.val = (_val_); \
} while (0)

#define dictSetSignedIntegerVal(entry, _val_) \
    do { entry->v.s64 = _val_; } while (0)

#define dictSetUnsignedIntegerVal(entry, _val_) \
    do { entry->v.u64 = _val_; } while (0)

// 释放字典节点的键
#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d)->privdata, (entry)->key)

// 设置字典节点的值
#define dictSetKey(d, entry, _key_) do { \
    if ((d)->type->keyDup) \
        entry->key = (d)->type->keyDup((d)->privdata, _key_); \
    else \
        entry->key = (_key_); \
} while (0)

// 对比两个键是否相等
#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d)->privdata, key1, key2) : \
        (key1) == (key2))

// 计算给定键的哈希值
#define dictHashKey(d, key) (d)->type->hashFunction(key)

#define dictGetKey(he) ((he)->key)

#define dictGetVal(he) ((he)->v.val)

#define dictGetSignedIntegerVal(he) ((he)->v.s64)

#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)

// 返回字典的大小
#define dictSlots(d) ((d)->ht[0].size + (d)->ht[1].size)

// 返回字典的已有节点数量
#define dictSize(d) ((d)->ht[0].used + (d)->ht[1].used)

// 返回字典是否正在进行rehash
#define dictIsRehashing(d) ((d)->rehashidx != -1)

/*
 * API定义
 */
dict* dictCreate(dictType* type, void* privDataPtr);
int dictExpand(dict* d, unsigned long size);
int dictAdd(dict* d, void* key, void* val);
dictEntry* dictAddRaw(dict* d, void* key);
int dictReplace(dict* d, void* key, void* val);
int dictDelete(dict* d, const void* key);
void dictRelease(dict* d);
dictEntry* dictFind(dict* d, const void* key);
void* dictFetchValue(dict* d, const void* key);
int dictResize(dict* d);

dictIterator* dictGetIterator(dict* d);
dictIterator* dictGetSafeIterator(dict* d);
dictEntry* dictNext(dictIterator* iter);
void dictReleaseIterator(dictIterator* iter);

dictEntry* dictGetRandomKey(dict* d);
unsigned int dictGenHashFunction(const void* key, int len);
void dictEnableResize(void);
void dictDisableResize(void);
int dictRehash(dict* d, int n);
int dictRehashMilliseconds(dict* d, int ms);

#endif //TINY_REDIS_DICT_H
