//
// Created by zouyi on 2021/8/14.
//

#ifndef TINY_REDIS_ADLIST_H
#define TINY_REDIS_ADLIST_H

/*
 * 链表节点定义
 */
typedef struct listNode {

    // 前置节点
    struct listNode* prev;

    // 后置节点
    struct listNode* next;

    // 保存的实际内容的指针
    void* value;

} listNode;

/*
 * 链表迭代器定义
 */
typedef struct listIter {

    // 当前遍历到的节点
    listNode* next;

    // 遍历方向: 正向或反向
    int direction;

} listIter;

/*
 * 链表定义: 多态实现
 */
typedef struct list {

    // 头节点，不实际分配内存，指向链表中第一个节点
    listNode* head;

    // 尾节点，不实际分配内存，指向链表中最后一个节点
    listNode* tail;

    // 函数指针，定义该链表复制节点时如何复制节点的value字段
    void* (*dup)(void* ptr);

    // 函数指针，定义该链表释放节点时如何释放节点的value字段
    void (*free)(void* ptr);

    // 函数指针，定义该链表如何定义两个节点相同，通过value字段判断
    int (*match)(void* ptr, void* key);

    unsigned long len;

} list;

#define listLength(l) ((l)->len)

#define listFirst(l) ((l)->head)

#define listLast(l) ((l)->tail)

#define listPrevNode(n) ((n)->prev)

#define listNextNode(n) ((n)->next)

#define listNodeValue(n) ((n)->value)

// 设置链表的dup函数指针
#define listSetDupMethod(l, m) ((l)->dup = (m))

// 设置链表的free函数指针
#define listSetFreeMethod(l, m) ((l)->free = (m))

// 设置链表的match函数指针
#define  listSetMatchMethod(l, m) ((l)->match = (m))

list* listCreate(void);
void listRelease(list* list);

list* listAddNodeHead(list* list, void* value);
list* listAddNodeTail(list* list, void* value);
list* listInsertNode(list* list, listNode* old_node, void* value, int after);
void listDelNode(list* list, listNode* node);

listIter* listGetIterator(list* list, int direction);
listNode* listNext(listIter* iter);
void listReleaseIterator(listIter* iter);

list* listDup(list* orig);
listNode* listSearchKey(list* list, void* key);
listNode* listIndex(list* list, long index);
void listRewind(list* list, listIter* li);
void listRewindTail(list* list, listIter* li);
void listRotate(list* list);

#define AL_START_HEAD 0
#define AL_START_TAIL 1

#endif //TINY_REDIS_ADLIST_H
