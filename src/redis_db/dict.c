//
// Created by zouyi on 2021/8/15.
//

#include <assert.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/time.h>

#include "dict.h"
#include "zmalloc.h"

/*
 * 私有变量
 */

// 指示字典是否启用rehash的标识
static int dict_can_resize = 1;

// 强制rehash的阈值
static unsigned int dict_force_resize_ratio = 5;

/* 
 * rehash会进行大量的内存写入操作，为了避免不必要的内存写入，最大限度节约内存(Redis主要瓶颈)，
 * 通过 dictEnableResize() 和 dictDisableResize() 两个函数，
 * 程序可以手动地允许或阻止哈希表进行 rehash ，
 * 这在 Redis 使用子进程进行保存操作时，可以有效地利用 copy-on-write 机制。
 *
 * 需要注意的是，并非所有 rehash 都会被 dictDisableResize 阻止：
 * 如果已使用节点的数量和字典大小之间的比率，
 * 大于字典强制 rehash 比率 dict_force_resize_ratio ，
 * 那么 rehash 仍然会（强制）进行。
 */

/*
 * 私有函数
 */
static int _dictExpandIfNeeded(dict* d);
static unsigned long _dictNextPower(unsigned long size);
static int _dictKeyIndex(dict* d, const void* key);
static int _dictInit(dict* d, dictType* type, void* privDataPtr);

/*
 * 哈希函数
 */
static uint32_t dict_hash_function_seed = 5381;

void dictSetHashFunctionSeed(uint32_t seed) {
    dict_hash_function_seed = seed;
}

uint32_t dictGetHashFunctionSeed(void) {
    return dict_hash_function_seed;
}

// MurmurHash2哈希算法，在输入有规律的情况下仍然能保持良好的随机性
// hash function
/* MurmurHash2, by Austin Appleby
 * Note - This code makes a few assumptions about how your machine behaves -
 * 1. We can read a 4-byte value from any address without crashing
 * 2. sizeof(int) == 4
 *
 * And it has a few limitations -
 *
 * 1. It will not work incrementally.
 * 2. It will not produce the same results on little-endian and big-endian
 *    machines.
 */
unsigned int dictGenHashFunction(const void* key, int len) {
    /* 'm' and 'r' are mixing constants generated offline.
     They're not really 'magic', they just happen to work well.  */
    uint32_t seed = dict_hash_function_seed;
    const uint32_t m = 0x5bd1e995;
    const int r = 24;

    /* Initialize the hash to a 'random' value */
    uint32_t h = seed ^ len;

    /* Mix 4 bytes at a time into the hash */
    const unsigned char *data = (const unsigned char *)key;

    while(len >= 4) {
        uint32_t k = *(uint32_t*)data;

        k *= m;
        k ^= k >> r;
        k *= m;

        h *= m;
        h ^= k;

        data += 4;
        len -= 4;
    }

    /* Handle the last few bytes of the input array  */
    switch(len) {
        case 3: h ^= data[2] << 16;
        case 2: h ^= data[1] << 8;
        case 1: h ^= data[0]; h *= m;
    };

    /* Do a few final mixes of the hash to ensure the last few
     * bytes are well-incorporated. */
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;

    return (unsigned int)h;
}

/*
 * API实现
 */

/*
 * 重置或初始化哈希表ht的各项属性
 */
static void _dictReset(dictht* ht) {
    ht->table = NULL;
    ht->size = 0;
    ht->sizemask = 0;
    ht->used = 0;
}

/*
 * 创建一个新的字典并初始化
 */
dict* dictCreate(dictType* type, void* privDataPtr) {
    dict* d = zmalloc(sizeof(dict));

    _dictInit(d, type, privDataPtr);

    return d;
}

/*
 * 初始化字典
 */
int _dictInit(dict* d, dictType* type, void* privDataPtr) {

    // 初始化两个哈希表，注意此时并不给哈希表中的指针数组分配内存
    _dictReset(&d->ht[0]);
    _dictReset(&d->ht[1]);

    // 设置类型特定函数
    d->type = type;

    // 设置私有数据
    d->privdata = privDataPtr;

    // 初始化rehash状态为并未处于rehash中
    d->rehashidx = -1;

    // 初始化正在使用的安全迭代器数目为0
    d->iterators = 0;

    return DICT_OK;
}

/*
 * 缩小给定字典，让其负载接近1
 */
int dictResize(dict* d) {
    int minimal;

    // 不能在禁止rehash或正在rehash时调用，返回DICT_ERR错误
    if (!dict_can_resize || dictIsRehashing(d)) return DICT_ERR;

    // 新的哈希表大小至少是已有的节点数量，越接近越好
    minimal = d->ht[0].used;
    if (minimal < DICT_HT_INITIAL_SIZE)
        minimal = DICT_HT_INITIAL_SIZE;

    // 调整哈希表的大小，调用dictExpand完成实际的操作
    return dictExpand(d, minimal);
}

/*
 * 缩放字典
 */
int dictExpand(dict* d, unsigned long size) {
    // 临时变量，暂存新的哈希表信息
    dictht n;

    // 实际缩放到的大小是第一个大于等于size的2的次方
    unsigned long realsize = _dictNextPower(size);

    // 不能在字典正在进行rehash时调用，缩放到的大小不能小于已有节点数量
    if (dictIsRehashing(d) || d->ht[0].used > size)
        return DICT_ERR;

    // 初始化新的哈希表各个属性
    n.size = realsize;
    n.sizemask = realsize - 1;
    // 分配空间并将指针数组中所有元素置NULL
    n.table = zcalloc(realsize * sizeof(dictEntry*));
    n.used = 0;

    // 如果0号哈希表table属性为空，说明是初始化
    // 将新的哈希表赋给0号哈希表，然后字典就可以开始处理键值对了
    if (d->ht[0].table == NULL) { 
        d->ht[0] = n;
        return DICT_OK;
    }

    // 如果0号哈希表table字段不为空，说明要开始一次新的rehash
    // 将新的哈希表赋给1号哈希表，然后将字典的rehashidx置0，开始对字典进行rehash
    // 1号哈希表接收来自0号哈希表rehash的桶
    d->ht[1] = n;
    d->rehashidx = 0;

    return DICT_OK;
}

/*
 * n步rehash
 *
 * 实际做的工作就是将0号哈希表的n个桶rehash到1号哈希表(对每个键值对，重新计算index)
 */
int dictRehash(dict* d, int n) {

    // 只有当字典正在进行rehash(rehashidx != -1)时可以进行
    if (!dictIsRehashing(d)) return 0;

    // 循环n次，一次处理一个桶(同一index的字典节点链表)
    while (n--) {
        dictEntry* de;
        dictEntry* nextde;

        // 如果0号哈希表已有节点为空，表示rehash结束
        if (d->ht[0].used == 0) {
            // 释放0号哈希表
            zfree(d->ht[0].table);
            // 将1号哈希表设置为新的0号哈希表
            d->ht[0] = d->ht[1];
            // 重置1号哈希表
            _dictReset(&d->ht[1]);
            // 关闭rehash标识
            d->rehashidx = -1;
            return 0;
        }

        // 确保rehashidx没有越界
        assert(d->ht[0].size > (unsigned)d->rehashidx);

        // 跳过空的桶，找到下一个非空索引
        while (d->ht[0].table[d->rehashidx] == NULL) d->rehashidx++;

        // de指向该索引的链表表头节点
        de = d->ht[0].table[d->rehashidx];
        // 将链表所有节点迁移到1号哈希表
        while (de) {
            unsigned int h;

            nextde = de->next;

            // 计算当前节点在1号哈希表应该放在哪个位置(index)
            h = dictHashKey(d, de->key) & d->ht[1].sizemask;
            // 当前节点插入到1号哈希表index位置的链表，头插法
            de->next = d->ht[1].table[h];
            d->ht[1].table[h] = de;

            // 更新两个哈希表当前节点数量
            d->ht[0].used--;
            d->ht[1].used++;

            de = nextde;
        }
        // rehashidx对应的链表已经迁移完毕，将其在table中对应的指针置NULL
        d->ht[0].table[d->rehashidx] = NULL;
        // 处理下一个索引
        d->rehashidx++;
    }

    return 1;
}

/*
 * 返回毫秒精度的UNIX时间戳
 */
long long timeInMilliseconds(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (((long long)tv.tv_sec)*1000) + (tv.tv_usec / 1000);
}

/*
 * 在给定毫秒数内，以100步为单位，对字典进行rehash
 */
int dictRehashMilliseconds(dict* d, int ms) {
    long long start = timeInMilliseconds();
    int rehashes = 0;

    while (dictRehash(d, 100)) {
        rehashes += 100;
        if (timeInMilliseconds() - start > ms) break;
    }

    return rehashes;
}

/*
 * 执行单步rehash，调用dictRehash完成实际的工作
 *
 * 提供给对字典进行增、删、改、查的函数调用
 */
static void _dictRehashStep(dict* d) {
    // 只有当安全迭代器的数量为0时，允许执行单步rehash，因为安全迭代器要保证返回的键值对没有重复
    if (d->iterators == 0) dictRehash(d, 1);
}

/*
 * 向哈希表中增加一个键值对
 *
 * 如果字典rehash标识打开，会进行单步rehash
 *
 * 注意: 每次增加键值对时都会检查是否满足开始rehash条件，如果满足即开始渐进式rehash
 */
int dictAdd(dict* d, void* key, void* val) {
    // 尝试添加键到字典，并返回包含这个键的新哈希节点
    dictEntry* entry = dictAddRaw(d, key);

    // entry为NULL，指示键已经在字典中存在，添加失败
    if (!entry) return DICT_ERR;

    // 键不存在，设置节点的值
    dictSetVal(d, entry, val);

    return DICT_OK;
}

/*
 * 尝试将键插入到字典中，并为其创建关联的哈希节点
 */
dictEntry* dictAddRaw(dict* d, void* key) {
    int index;
    dictEntry* entry;
    dictht* ht;

    // 增加节点时，如果字典rehash标识打开，则进行单步rehash
    if (dictIsRehashing(d)) _dictRehashStep(d);

    // 调用_dictKeyIndex获取键在哈希表中应该存放的index，如果返回值为-1，表示键已存在，插入失败
    // 如果字典正在进行rehash，则_dictKeyIndex总是返回键在1号哈希表中应该存放的index，
    // 因为开始rehash后，所有节点新增都是插入到1号哈希表，0号哈希表已有节点只会越来越少，最后到0
    if ((index = _dictKeyIndex(d, key)) == -1)
        return NULL;

    // 字典正在rehash就选择1号哈希表插入，否则选择0号哈希表
    ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];
    // 创建一个新的哈希节点
    entry = zmalloc(sizeof(dictEntry));
    // 插入到index对应的链表中，头插法
    entry->next = ht->table[index];
    ht->table[index] = entry;
    // 更新哈希表已有节点的数量
    ht->used++;
    
    // 设置新节点的键
    dictSetKey(d, entry, key);

    return entry;
}

/*
 * 替换字典中键对应的值，如果键不存在，则插入一个新的节点
 *
 * 如果字典rehash标识打开，dictAdd和dictFind都会进行单步rehash
 */
int dictReplace(dict* d, void* key, void* val) {
    dictEntry* entry;
    dictEntry auxentry;

    // 尝试插入一个新的键值对
    if (dictAdd(d, key, val) == DICT_OK)
        return 1;

    // 修改已经有的键值对的值
    entry = dictFind(d, key);

    auxentry = *entry;
    dictSetVal(d, entry, val);
    dictFreeVal(d, &auxentry);

    return 0;
}

/*
 * 查找并删除包含给定键的节点，如果字典正在进行rehash，查找删除操作会在0号和1号哈希表进行
 * 
 * nofree决定是否调用键和值的释放函数，0:调用 1:不调用
 */
static int dictGenericDelete(dict* d, const void* key, int nofree) {
    unsigned int h, idx;
    dictEntry* he;
    dictEntry* prevHe;
    int table;

    if (d->ht[0].size == 0) return DICT_ERR;

    // 删除节点时，如果字典rehash标识打开，则进行单步rehash
    if (dictIsRehashing(d)) _dictRehashStep(d);

    // 计算键的哈希值
    h = dictHashKey(d, key);

    // 根据是否正在rehash决定是否需要在1号哈希表中查找给定键
    for (table = 0; table <= 1; table++) {

        // 计算键在table号哈希表中应该处于哪个索引对应的链表中
        idx = h & d->ht[table].sizemask;
        // he初始化为索引对应链表的头节点
        he = d->ht[table].table[idx];
        // prevHe指向he的前一个节点
        prevHe = NULL;

        // 遍历索引对应的链表
        while (he) {
            // 找到了给定键的节点
            if (dictCompareKeys(d, key, he->key)) {
                // 将该节点从链表中删除
                if (prevHe)
                    prevHe->next = he->next;
                else
                    d->ht[table].table[idx] = he->next;

                // 根据nofree决定是否要调用键和值的释放函数
                if (!nofree) {
                    dictFreeKey(d, he);
                    dictFreeVal(d, he);
                }

                // 释放该节点并将table号哈希表已有节点数量减1
                zfree(he);
                d->ht[table].used--;

                return DICT_OK;
            }

            prevHe = he;
            he = he->next;
        }

        // 如果说字典未处于rehash，则只会在0号哈希表中查找并删除指定key对应到节点
        if (!dictIsRehashing(d)) break;
    }

    return DICT_ERR;
}

/*
 * 从字典中删除包含指定键的节点，并且调用键和值的释放函数
 *
 * 如果字典rehash标识打开，会进行单步rehash
 */
int dictDelete(dict* d, const void* key) {
    return dictGenericDelete(d, key, 0);
}

/*
 * 删除哈希表上的所有节点，并重置哈希表的各项属性
 */
int _dictClear(dict* d, dictht* ht, void(callback)(void*)) {
    unsigned long i;

    // 循环进行删除，直到哈希表上已有节点为空
    for (i = 0; i < ht->size && ht->used > 0; i++) {
        dictEntry* he;
        dictEntry* nextHe;

        // 回调函数，目前没有用到
        if (callback && (i & 65535) == 0) callback(d->privdata);

        // 跳过空索引
        if ((he = ht->table[i]) == NULL) continue;

        // 遍历索引对应的链表，删除节点
        while (he) {
            nextHe = he->next;
            dictFreeKey(d, he);
            dictFreeVal(d, he);
            zfree(he);

            ht->used--;
            he = nextHe;
        }
    }

    // 释放哈希表结构
    zfree(ht->table);

    // 重置哈希表属性
    _dictReset(ht);

    return DICT_OK;
}

/*
 * 删除并释放整个字典
 */
void dictRelease(dict* d) {
    // 删除0号哈希表所有节点并释放0号哈希表
    _dictClear(d, &d->ht[0], NULL);
    // 删除1号哈希表所有节点并释放1号哈希表
    _dictClear(d, &d->ht[1], NULL);
    // 释放字典结构
    zfree(d);
}

/*
 * 查找字典中包含键key的节点，如果字典正在进行rehash，查找操作会在0号和1号哈希表进行
 *
 * 如果字典rehash标识打开，会进行单步rehash
 */
dictEntry* dictFind(dict* d, const void* key) {
    dictEntry* he;
    unsigned int h, idx, table;

    if (d->ht[0].size == 0) return NULL;

    // 查找节点时，如果字典rehash标识打开，则进行单步rehash
    if (dictIsRehashing(d)) _dictRehashStep(d);

    h = dictHashKey(d, key);

    // 根据是否正在rehash决定是否需要在1号哈希表中查找给定键
    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].sizemask;

        he = d->ht[table].table[idx];
        while (he) {
            if (dictCompareKeys(d, key, he->key))
                return he;

            he = he->next;
        }

        if (!dictIsRehashing(d)) return NULL;
    }

    return NULL;
}

/*
 * 获取包含给定键的节点的值，调用dictFind完成实际的操作
 */
void* dictFetchValue(dict* d, const void* key) {
    dictEntry* he;

    he = dictFind(d, key);

    return he ? dictGetVal(he) : NULL;
}

/*
 * 根据字典的状态计算一个64位的哈希值(指纹)
 * 
 * 用于非安全迭代器迭代过程，在初始化和释放一个非安全迭代器时调用计算得到两个指纹，
 * 若指纹发生变化，说明迭代过程中对字典进行了非法操作
 */
/* A fingerprint is a 64 bit number that represents the state of the dictionary
 * at a given time, it's just a few dict properties xored together.
 * When an unsafe iterator is initialized, we get the dict fingerprint, and check
 * the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the dictionary while iterating. */
long long dictFingerprint(dict *d) {
    long long integers[6], hash = 0;
    int j;

    integers[0] = (long) d->ht[0].table;
    integers[1] = d->ht[0].size;
    integers[2] = d->ht[0].used;
    integers[3] = (long) d->ht[1].table;
    integers[4] = d->ht[1].size;
    integers[5] = d->ht[1].used;

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    for (j = 0; j < 6; j++) {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;
}

/*
 * 创建并返回字典的非安全迭代器
 */
dictIterator* dictGetIterator(dict* d) {
    dictIterator* iter = zmalloc(sizeof(dictIterator));

    iter->d = d;
    iter->table = 0;
    // 当前迭代的桶索引，从-1开始
    iter->index = -1;
    // 指示当前是一个非安全迭代器
    iter->safe = 0;
    iter->entry = NULL;
    iter->nextEntry = NULL;

    return iter;
}

/*
 * 创建并返回字典的安全迭代器
 */
dictIterator* dictGetSafeIterator(dict* d) {
    dictIterator* i = dictGetIterator(d);
    // 指示当前是一个安全迭代器
    i->safe = 1;

    return i;
}

/*
 * 获取迭代器当前指向的节点
 *
 * 如果字典正在rehash(rehashidx != -1)，则会迭代0号和1号哈希表
 * 非安全迭代器迭代过程字典是只读的，禁止单步rehash，通过指纹
 * 安全迭代器迭代过程可以修改，禁止单步rehash，保证不会重复
 */
dictEntry* dictNext(dictIterator* iter) {
    while (1) {

        if (iter->entry == NULL) {

            // ht指向当前被迭代的哈希表
            dictht* ht = &iter->d->ht[iter->table];

            // 情况1，初次迭代
            if (iter->index == -1 && iter->table == 0) {
                // 如果是安全迭代器，更新当前使用的安全迭代器的数量
                if (iter->safe)
                    iter->d->iterators++;
                // 如果是非安全迭代器，计算初次迭代时候的指纹
                else
                    iter->fingerprint = dictFingerprint(iter->d);
            }

            // 情况2，一个桶遍历完了
            // 更新桶索引，处理下一个桶，注意iter->index初始化为-1
            iter->index++;

            // 桶索引超出当前哈希表大小，说明这个哈希表已经迭代完毕
            if (iter->index >= (signed) ht->size) {
                // 如果正在rehash并且当前迭代的哈希表是0号哈希表，则开始迭代1号哈希表
                if (dictIsRehashing(iter->d) && iter->table == 0) {
                    iter->table++;
                    iter->index = 0;
                    ht = &iter->d->ht[1];
                // 迭代已经完成
                } else {
                    break;
                }
            }

            // 指向下一个桶的头节点
            iter->entry = ht->table[iter->index];
        } else {
            // 指向同一个桶的下一个节点
            iter->entry = iter->nextEntry;
        }

        // 注意: 如果当前节点不为空，则需要记录下该节点的下一个节点
        // 这一步是必要的，因为安全迭代器可能会将迭代器返回的当前节点删除
        if (iter->entry) {
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            iter->nextEntry = iter->entry->next;
            return iter->entry;
        }
    }

    return NULL;
}

/*
 * 释放指定字典迭代器
 */
void dictReleaseIterator(dictIterator* iter) {
    if (!(iter->index == -1 && iter->table == 0)) {
        // 释放安全迭代器，安全迭代器数目减1
        if (iter->safe)
            iter->d->iterators--;
        // 释放非安全迭代器，验证指纹
        else
            // 指纹不匹配则宕机
            assert(iter->fingerprint == dictFingerprint(iter->d));
    }
    zfree(iter);
}

/*
 * 从字典中随机获取一个节点
 *
 * 如果字典rehash标识打开，会进行单步rehash
 */
dictEntry* dictGetRandomKey(dict* d) {
    dictEntry* he;
    dictEntry* orighe;
    unsigned int h;
    int listlen, listele;

    if (dictSize(d) == 0) return NULL;

    // 如果字典正在rehash，则进行单步rehash
    if (dictIsRehashing(d)) _dictRehashStep(d);

    // 如果字典正在rehash，则1号哈希表也作为随机查找的目标
    if (dictIsRehashing(d)) {
        do {
            h = random() % (d->ht[0].size + d->ht[1].size);
            he = (h >= d->ht[0].size) ? d->ht[1].table[h - d->ht[0].size] :
                                      d->ht[0].table[h];
        } while (he == NULL);
    // 否则，只从0号哈希表中随机查找
    } else {
        do {
            h = random() & d->ht[0].sizemask;
            he = d->ht[0].table[h];
        } while (he == NULL);
    }

    // 从随机获取的非空链表中再随机获取一个节点
    listlen = 0;
    orighe = he;
    while (he) {
        he = he->next;
        listlen++;
    }
    listele = random() % listlen;
    he = orighe;
    while (listele--) he = he->next;

    return he;
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
static unsigned long rev(unsigned long v) {
    unsigned long s = 8 * sizeof(v); // bit size; must be power of 2
    unsigned long mask = ~0;
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

/* dictScan() is used to iterate over the elements of a dictionary.
 *
 * dictScan() 函数用于迭代给定字典中的元素。
 *
 * Iterating works in the following way:
 *
 * 迭代按以下方式执行：
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 *    一开始，你使用 0 作为游标来调用函数。
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value that you must use in the next call.
 *    函数执行一步迭代操作，
 *    并返回一个下次迭代时使用的新游标。
 * 3) When the returned cursor is 0, the iteration is complete.
 *    当函数返回的游标为 0 时，迭代完成。
 *
 * The function guarantees that all the elements that are present in the
 * dictionary from the start to the end of the iteration are returned.
 * However it is possible that some element is returned multiple time.
 *
 * 函数保证，在迭代从开始到结束期间，一直存在于字典的元素肯定会被迭代到，
 * 但一个元素可能会被返回多次。
 *
 * For every element returned, the callback 'fn' passed as argument is
 * called, with 'privdata' as first argument and the dictionar entry
 * 'de' as second argument.
 *
 * 每当一个元素被返回时，回调函数 fn 就会被执行，
 * fn 函数的第一个参数是 privdata ，而第二个参数则是字典节点 de 。
 *
 * HOW IT WORKS.
 * 工作原理
 *
 * The algorithm used in the iteration was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits, that is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * 迭代所使用的算法是由 Pieter Noordhuis 设计的，
 * 算法的主要思路是在二进制高位上对游标进行加法计算
 * 也即是说，不是按正常的办法来对游标进行加法计算，
 * 而是首先将游标的二进制位翻转（reverse）过来，
 * 然后对翻转后的值进行加法计算，
 * 最后再次对加法计算之后的结果进行翻转。
 *
 * This strategy is needed because the hash table may be resized from one
 * call to the other call of the same iteration.
 *
 * 这一策略是必要的，因为在一次完整的迭代过程中，
 * 哈希表的大小有可能在两次迭代之间发生改变。
 *
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * always by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * 哈希表的大小总是 2 的某个次方，并且哈希表使用链表来解决冲突，
 * 因此一个给定元素在一个给定表的位置总可以通过 Hash(key) & SIZE-1
 * 公式来计算得出，
 * 其中 SIZE-1 是哈希表的最大索引值，
 * 这个最大索引值就是哈希表的 mask （掩码）。
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will be always
 * the last four bits of the hash output, and so forth.
 *
 * 举个例子，如果当前哈希表的大小为 16 ，
 * 那么它的掩码就是二进制值 1111 ，
 * 这个哈希表的所有位置都可以使用哈希值的最后四个二进制位来记录。
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 * 如果哈希表的大小改变了怎么办？
 *
 * If the hash table grows, elements can go anyway in one multiple of
 * the old bucket: for example let's say that we already iterated with
 * a 4 bit cursor 1100, since the mask is 1111 (hash table size = 16).
 *
 * 当对哈希表进行扩展时，元素可能会从一个槽移动到另一个槽，
 * 举个例子，假设我们刚好迭代至 4 位游标 1100 ，
 * 而哈希表的 mask 为 1111 （哈希表的大小为 16 ）。
 *
 * If the hash table will be resized to 64 elements, and the new mask will
 * be 111111, the new buckets that you obtain substituting in ??1100
 * either 0 or 1, can be targeted only by keys that we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 *
 * 如果这时哈希表将大小改为 64 ，那么哈希表的 mask 将变为 111111 ，
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger, and will
 * just continue iterating with cursors that don't have '1100' at the end,
 * nor any other combination of final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, If a combination of the lower three bits (the mask for size 8
 * is 111) was already completely explored, it will not be visited again
 * as we are sure that, we tried for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 * 等等。。。在 rehash 的时候可是会出现两个哈希表的阿！
 *
 * Yes, this is true, but we always iterate the smaller one of the tables,
 * testing also all the expansions of the current cursor into the larger
 * table. So for example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 * 限制
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 * 这个迭代器是完全无状态的，这是一个巨大的优势，
 * 因为迭代可以在不使用任何额外内存的情况下进行。
 *
 * The disadvantages resulting from this design are:
 * 这个设计的缺陷在于：
 *
 * 1) It is possible that we return duplicated elements. However this is usually
 *    easy to deal with in the application level.
 *    函数可能会返回重复的元素，不过这个问题可以很容易在应用层解决。
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving.
 *    为了不错过任何元素，
 *    迭代器需要返回给定桶上的所有键，
 *    以及因为扩展哈希表而产生出来的新表，
 *    所以迭代器必须在一次迭代中返回多个元素。
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 *    对游标进行翻转（reverse）的原因初看上去比较难以理解，
 *    不过阅读这份注释应该会有所帮助。
 */
unsigned long dictScan(dict *d,
                       unsigned long v,
                       dictScanFunction *fn,
                       void *privdata)
{
    dictht *t0, *t1;
    const dictEntry *de;
    unsigned long m0, m1;

    // 跳过空字典
    if (dictSize(d) == 0) return 0;

    // 迭代只有一个哈希表的字典
    if (!dictIsRehashing(d)) {

        // 指向哈希表
        t0 = &(d->ht[0]);

        // 记录 mask
        m0 = t0->sizemask;

        /* Emit entries at cursor */
        // 指向哈希桶
        de = t0->table[v & m0];
        // 遍历桶中的所有节点
        while (de) {
            fn(privdata, de);
            de = de->next;
        }

        // 迭代有两个哈希表的字典
    } else {

        // 指向两个哈希表
        t0 = &d->ht[0];
        t1 = &d->ht[1];

        /* Make sure t0 is the smaller and t1 is the bigger table */
        // 确保 t0 比 t1 要小
        if (t0->size > t1->size) {
            t0 = &d->ht[1];
            t1 = &d->ht[0];
        }

        // 记录掩码
        m0 = t0->sizemask;
        m1 = t1->sizemask;

        /* Emit entries at cursor */
        // 指向桶，并迭代桶中的所有节点
        de = t0->table[v & m0];
        while (de) {
            fn(privdata, de);
            de = de->next;
        }

        /* Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table */
        // Iterate over indices in larger table             // 迭代大表中的桶
        // that are the expansion of the index pointed to   // 这些桶被索引的 expansion 所指向
        // by the cursor in the smaller table               //
        do {
            /* Emit entries at cursor */
            // 指向桶，并迭代桶中的所有节点
            de = t1->table[v & m1];
            while (de) {
                fn(privdata, de);
                de = de->next;
            }

            /* Increment bits not covered by the smaller mask */
            v = (((v | m0) + 1) & ~m0) | (v & m0);

            /* Continue while bits covered by mask difference is non-zero */
        } while (v & (m0 ^ m1));
    }

    /* Set unmasked bits so incrementing the reversed cursor
     * operates on the masked bits of the smaller table */
    v |= ~m0;

    /* Increment the reverse cursor */
    v = rev(v);
    v++;
    v = rev(v);

    return v;
}

/*
 * 私有函数定义
 */

/*
 * 根据需要，初始化字典的哈希表(0号)，或对字典的哈希表(1号)进行扩展
 *
 * 每次增加节点时都会调用
 * dictAddRaw() -> _dictKeyIndex() -> _dictExpandIfNeeded()
 */
static int _dictExpandIfNeeded(dict* d) {
    if (dictIsRehashing(d)) return DICT_OK;

    // 初始化字典的0号哈希表
    if (d->ht[0].size == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE);

    // 创建更大的1号哈希表，并开始渐进式rehash
    // 负载因子大于1且dict_can_resize置位或者负载因子大于5时触发，扩展后的大小至少是目前已有节点数的两倍
    if (d->ht[0].used >= d->ht[0].size &&
        (dict_can_resize || d->ht[0].used / d->ht[0].size > dict_force_resize_ratio)) {
        return dictExpand(d, d->ht[0].used * 2);
    }

    return DICT_OK;
}

/*
 * 计算第一个大于等于size的2的次方，用作哈希表扩展后的大小
 */
static unsigned long _dictNextPower(unsigned long size) {
    unsigned long i = DICT_HT_INITIAL_SIZE;

    if (size >= LONG_MAX) return LONG_MAX;
    while (1) {
        if (i >= size)
            return i;
        i *= 2;
    }
}

/*
 * 返回将key插入到哈希表的索引，如果key已经存在，则返回-1
 * 每次增加节点时都会调用
 * dictAddRaw() -> _dictKeyIndex() -> _dictExpandIfNeeded()
 */
static int _dictKeyIndex(dict* d, const void* key) {
    unsigned int h, idx, table;
    dictEntry* he;

    // 调用_dictExpandIfNeeded，判断是否需要开始rehash
    if (_dictExpandIfNeeded(d) == DICT_ERR)
        return -1;

    // 如果是一个新key，且正在rehash，则返回插入到1号哈希表中的索引(在rehash过程中所有新节点都插入到1号哈希表)
    h = dictHashKey(d, key);
    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].sizemask;
        he = d->ht[table].table[idx];
        while (he) {
            if (dictCompareKeys(d, key, he->key))
                return -1;
            he = he->next;
        }

        // 未在rehash，返回插入到0号哈希表中的索引
        if (!dictIsRehashing(d)) break;
    }

    return idx;
}

/*
 * 全局变量，开启自动rehash(在插入节点时检查条件，如果满足开启rehash)
 */
void dictEnableResize(void) {
    dict_can_resize = 1;
}

/*
 * 全局变量，关闭自动rehash
 */
void dictDisableResize(void) {
    dict_can_resize = 0;
}
