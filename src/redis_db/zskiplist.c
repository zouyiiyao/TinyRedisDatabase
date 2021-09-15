//
// Created by zouyi on 2021/8/23.
//

#include <math.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "zskiplist.h"
#include "zmalloc.h"
#include "redis.h"

/*
 * 创建一个层数为level的跳跃表节点，
 * 并将节点的成员对象设置为obj，分值设置为score
 */
zskiplistNode* zslCreateNode(int level, double score, robj* obj) {

    zskiplistNode* zn = zmalloc(sizeof(zskiplistNode) + level * sizeof(struct zskiplistLevel));

    zn->score = score;
    zn->obj = obj;

    return zn;
}

/*
 * 创建一个新的跳跃表
 */
zskiplist* zslCreate(void) {
    int j;
    zskiplist* zsl;

    // 分配跳跃表结构空间
    zsl = zmalloc(sizeof(zskiplist));

    // 设置起始最大层数为1，拥有的节点数量是0
    zsl->level = 1;
    zsl->length = 0;

    // 创建头节点，注意头节点不包含在跳跃表节点数目统计内
    zsl->header = zslCreateNode(ZSKIPLIST_MAXLEVEL, 0, NULL);
    // 头节点拥有最大层数，初始化头节点level属性
    for (j = 0; j < ZSKIPLIST_MAXLEVEL; j++) {
        zsl->header->level[j].forward = NULL;
        zsl->header->level[j].span = 0;
    }
    zsl->header->backward = NULL;

    zsl->tail = NULL;

    return zsl;
}

/*
 * 释放给定的跳跃表节点
 */
void zslFreeNode(zskiplistNode* node) {

    decrRefCount(node->obj);

    // 释放跳跃表节点空间
    zfree(node);
}

/*
 * 释放给定跳跃表，以及表中所有节点
 */
void zslFree(zskiplist* zsl) {

    zskiplistNode* node = zsl->header->level[0].forward;
    zskiplistNode* next;

    // 释放表头节点
    zfree(zsl->header);

    // 释放表中所有节点，由于每一个节点至少有一层，所以第一层的跨度都是1，
    // 遍历所有节点只需要沿着节点第一层forward指针走到NULL即可
    while (node) {

        next = node->level[0].forward;

        zslFreeNode(node);

        node = next;
    }

    // 释放跳跃表结构
    zfree(zsl);
}

/*
 * 返回一个随机值，用作新跳跃表节点的层数
 *
 * 返回值介于1到ZSKIPLIST_MAXLEVEL，越大的值被返回的概率越小(幂次定律)
 */
int zslRandomLevel(void) {
    // 至少为1
    int level = 1;

    // ZSKIPLIST_P ^ k
    while ((random() & 0xFFFF) < (ZSKIPLIST_P * 0xFFFF))
        level += 1;

    // 保证不会超过ZSKIPLIST_MAXLEVEL
    return (level < ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL;
}

/*
 * 创建一个成员为obj，分值为score的新跳跃表节点，并插入到跳跃表zsl中 
 *
 * 调用者需要保证同分值且同成员的元素不会出现
 * T_WORST = O(N), T_AVG = O(logN)
 */
zskiplistNode* zslInsert(zskiplist* zsl, double score, robj* obj) {
    // 存每一层的前置节点
    zskiplistNode* update[ZSKIPLIST_MAXLEVEL];
    // 临时变量
    zskiplistNode* x;
    // 记录每一层的跨度
    unsigned int rank[ZSKIPLIST_MAXLEVEL];
    int i, level;

    // 从高到低迭代每一层，寻找插入位置
    // 查找过程就是下阶梯，向右走到头，然后向下，再向右
    x = zsl->header;
    for (i = zsl->level - 1; i >= 0; i--) {

        // 累加处理，rank[0]最后存放新节点总共跨越的节点数目
        // rank[i]表示第i层插入节点的排名，rank[0] - rank[i] + 1就是第i层插入节点到新节点的跨度
        rank[i] = i == (zsl->level - 1) ? 0 : rank[i + 1];

        // 同层遍历，沿forward指针向前查找
        while (x->level[i].forward &&
            (x->level[i].forward->score < score
            || (x->level[i].forward->score == score && compareStringObjects(x->level[i].forward->obj, obj) < 0))) {

            // 在同一层往前走一步，跨越的节点累加
            rank[i] += x->level[i].span;

            // 前进一步
            x = x->level[i].forward;
        }
        // 记录要和新节点第i层的插入位置
        update[i] = x;
    }

    // 获取一个随机值作为新节点的层数
    level = zslRandomLevel();

    /*
     * 如果新节点的层数超过了之前的最高层数，
     * 则初始化表头节点中未使用的层，并记录在zsl->level到level之间的层需要插入到表头节点后
     */
    if (level > zsl->level) {
        for (i = zsl->level; i < level; i++) {
            rank[i] = 0;
            update[i] = zsl->header;
            // 这里的初始化有一定的迷惑性
            update[i]->level[i].span = zsl->length;
        }

        // 更新跳跃表中节点最大层数
        zsl->level = level;
    }

    // 创建新节点
    x = zslCreateNode(level, score, obj);


    /* printf("rank[0]: %d\n", rank[0]); */
    // 将update数组记录的节点对应层的指针指向新节点，并设置跨度
    for (i = 0; i < level; i++) {

        x->level[i].forward = update[i]->level[i].forward;

        update[i]->level[i].forward = x;

        // 总跨度增加1
        x->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]);

        update[i]->level[i].span = (rank[0] - rank[i]) + 1;

        /* printf("i: %d rank[%d]: %d x->level[%d].span: %d update[%d]->level[%d].span: %d\n", i, i, rank[i], i, x->level[i].span, i, i, update[i]->level[i].span); */
    }
    /* printf("\n"); */

    // 未接触层span要增1，中间插入了一个节点(跨越)
    for (i = level; i < zsl->level; i++) {
        update[i]->level[i].span++;
    }

    // 设置回退指针
    x->backward = (update[0] == zsl->header) ? NULL : update[0];
    if (x->level[0].forward)
        x->level[0].forward->backward = x;
    else
        zsl->tail = x;

    // 跳跃表的节点数目增1
    zsl->length++;

    return x;
}

/*
 * 内部使用，删除给定节点
 */
void zslDeleteNode(zskiplist* zsl, zskiplistNode* x, zskiplistNode** update) {
    int i;

    // 更新所有和被删除节点x有关的节点的指针，更新跨度
    for (i = 0; i < zsl->level; i++) {
        if (update[i]->level[i].forward == x) {
            update[i]->level[i].span += x->level[i].span - 1;
            update[i]->level[i].forward = x->level[i].forward;
        } else {
            update[i]->level[i].span -= 1;
        }
    }

    // 更新被删除节点x的下一个节点的回退指针或更新跳表尾指针
    if (x->level[0].forward)
        x->level[0].forward->backward = x->backward;
    else
        zsl->tail = x->backward;

    // 如果被删除节点是跳跃表中层数最高的节点，则更新跳跃表最大层数
    while (zsl->level > 1 && zsl->header->level[zsl->level - 1].forward == NULL)
        zsl->level--;

    zsl->length--;
}

/*
 * 从跳跃表zsl中删除给定节点(分值为score，成员对象为obj)
 */
int zslDelete(zskiplist* zsl, double score, robj* obj) {
    // 存放每一层的前置节点，用于调用zslDeleteNode
    zskiplistNode* update[ZSKIPLIST_MAXLEVEL];
    zskiplistNode* x;
    int i;

    // 调用该函数时，传入score参数是必要的，用于查询前置节点(从最高层开始，下阶梯)
    x = zsl->header;
    for (i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward &&
            (x->level[i].forward->score < score
            || (x->level[i].forward->score == score && compareStringObjects(x->level[i].forward->obj, obj) < 0)))

            x = x->level[i].forward;

        update[i] = x;
    }

    // 找到第1层的前置节点的下一个节点，如果是目标节点，则删除之；如果不是目标节点，则目标节点不存在于跳跃表内
    x = x->level[0].forward;
    if (x && score == x->score && equalStringObjects(x->obj, obj)) {
        zslDeleteNode(zsl, x, update);
        zslFreeNode(x);
        return 1;
    } else {
        return 0; /* not found */
    }

    return 0; /* not found */
}

/*
 * 检查value是否大于范围内的最小值
 */
static int zslValueGteMin(double value, zrangespec* spec) {
    return spec->minex ? (value > spec->min) : (value >= spec->min);
}

/*
 * 检查value是否小于范围内的最大值
 */
static int zslValueLteMax(double value, zrangespec* spec) {
    return spec->maxex ? (value < spec->max) : (value <= spec->max);
}

/*
 * 检查跳表所含节点分值范围与给定范围是否有交集，有返回1，否则返回0
 */
int zslIsInRange(zskiplist* zsl, zrangespec* range) {
    zskiplistNode* x;

    // 空的范围值，直接返回0
    if (range->min > range->max || (range->min == range->max && (range->minex || range->maxex)))
        return 0;

    // 检查最后一个节点，最大分值
    x = zsl->tail;
    if (x == NULL || !zslValueGteMin(x->score, range))
        return 0;

    // 检查第一个节点，最小分值
    x = zsl->header->level[0].forward;
    if (x == NULL || !zslValueLteMax(x->score, range))
        return 0;

    return 1;
}

/*
 * 返回跳跃表zsl中第一个处于range范围内的节点
 */
zskiplistNode* zslFirstInRange(zskiplist* zsl, zrangespec* range) {
    zskiplistNode* x;
    int i;

    // 给定范围与跳跃表节点分值范围无交集，直接返回空指针
    if (!zslIsInRange(zsl, range)) return NULL;

    // 只要没达到范围内的最小值，就往前查找
    x = zsl->header;
    for (i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward && !zslValueGteMin(x->level[i].forward->score, range))
            x = x->level[i].forward;
    }

    // 找到待定节点x
    x = x->level[0].forward;
    assert(x != NULL);

    // 待定节点x分值大于范围内的最大值，返回空指针
    if (!zslValueLteMax(x->score, range)) return NULL;

    // 确认待定节点x分值处于范围内，返回x
    return x;
}

/*
 * 返回跳跃表zsl中最后一个处于range范围内的节点
 */
zskiplistNode* zslLastInRange(zskiplist* zsl, zrangespec* range) {
    zskiplistNode* x;
    int i;

    // 给定范围与跳跃表节点分值范围无交集，直接返回空指针
    if (!zslIsInRange(zsl, range)) return NULL;

    // 只要没达到范围内的最大值，就往前查找
    x = zsl->header;
    for (i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward && zslValueLteMax(x->level[i].forward->score, range))
            x = x->level[i].forward;
    }

    assert(x != NULL);

    // 待定节点x分值不大于范围内的最小值，返回空指针
    if (!zslValueGteMin(x->score, range)) return NULL;

    // 确认待定节点x分值处于范围内，返回x
    return x;
}

/*
 * 删除给定Score范围内的跳跃表节点，返回删除的节点个数
 */
unsigned long zslDeleteRangeByScore(zskiplist* zsl, zrangespec* range , dict* dict) {
    zskiplistNode* update[ZSKIPLIST_MAXLEVEL];
    zskiplistNode* x;
    unsigned long removed = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward && (range->minex ?
            x->level[i].forward->score <= range->min :
            x->level[i].forward->score < range->min))

            x = x->level[i].forward;
        update[i] = x;
    }

    x = x->level[0].forward;

    while (x && (range->maxex ? x->score < range->max : x->score <= range->max)) {
        zskiplistNode* next = x->level[0].forward;
        zslDeleteNode(zsl, x, update);
        dictDelete(dict, x->obj);
        zslFreeNode(x);
        removed++;
        x = next;
    }

    return removed;
}

/*
 * 删除给定rank范围内的跳跃表节点，返回删除节点的个数
 */
unsigned long zslDeleteRangeByRank(zskiplist* zsl, unsigned int start, unsigned int end , dict* dict) {
    zskiplistNode* update[ZSKIPLIST_MAXLEVEL];
    zskiplistNode* x;
    unsigned long traversed = 0;
    unsigned long removed = 0;
    int i;

    // 沿着前进指针移动到排位的起始位置，并记录每一层前置节点
    x = zsl->header;
    for (i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward && (traversed + x->level[i].span) < start) {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    // x指向排位的起始第一个节点
    traversed++;
    x = x->level[0].forward;

    // 删除所有在给定排位范围内的节点
    while (x && traversed <= end) {

        zskiplistNode* next = x->level[0].forward;

        zslDeleteNode(zsl, x, update);

        dictDelete(dict, x->obj);

        zslFreeNode(x);

        removed++;

        traversed++;

        x = next;
    }

    return removed;
}

/*
 * 查找给定分值和成员对象的节点在跳跃表zsl中的排位
 */
unsigned long zslGetRank(zskiplist* zsl, double score, robj* obj) {
    zskiplistNode* x;
    unsigned long rank = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward &&
            (x->level[i].forward->score < score
            || (x->level[i].forward->score == score && compareStringObjects(x->level[i].forward->obj, obj) <= 0))) {

            rank += x->level[i].span;
            x = x->level[i].forward;
        }

        if (x->obj && equalStringObjects(x->obj, obj)) {
            return rank;
        }
    }

    return 0;
}

/*
 * 根据排位在跳跃表中查找元素
 */
zskiplistNode* zslGetElementByRank(zskiplist* zsl, unsigned long rank) {
    zskiplistNode* x;
    unsigned long traversed = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward && (traversed + x->level[i].span) <= rank) {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }

        if (traversed == rank) {
            return x;
        }
    }

    return NULL;
}
