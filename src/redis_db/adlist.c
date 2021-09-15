#include <stdlib.h>
#include "adlist.h"
#include "zmalloc.h"

/*
 * 创建一个空链表，不含任何节点
 */
list* listCreate(void) {
    struct list* list;

    // 分配内存
    if ((list = zmalloc(sizeof(struct list))) == NULL)
        return NULL;

    // 初始化
    list->head = list->tail = NULL;
    list->len = 0;
    list->dup = NULL;
    list->free = NULL;
    list->match = NULL;

    return list;
}

/*
 * 释放一个链表，并释放所有节点
 * T: O(N)
 */
void listRelease(list* list) {
    unsigned long len;
    listNode* current;
    listNode* next;

    current = list->head;
    len = list->len;
    while (len--) {
        next = current->next;

        // 如果定义了value释放函数，则调用它
        if (list->free) list->free(current->value);

        // 释放节点结构
        zfree(current);

        current = next;
    }

    // 释放链表结构
    zfree(list);
}

/*
 * 插入一个包含给定值指针value的新元素作为链表头节点
 * T: O(1)
 */
list* listAddNodeHead(list* list, void* value) {
    listNode* node;

    // 分配节点内存
    if ((node = zmalloc(sizeof(struct listNode))) == NULL)
        return NULL;

    // 值指针初始化
    node->value = value;

    if (list->len == 0) {
        // 添加节点到空链表
        // 链表头尾指针都指向该节点
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        // 添加节点到非空链表
        node->prev = NULL;
        node->next = list->head;
        list->head->prev = node;
        list->head = node;
    }

    // 修改链表节点数目
    list->len++;

    return list;
}

/*
 * 插入一个包含给定值指针value的新元素作为链表尾节点
 * T: O(1)
 */
list* listAddNodeTail(list* list, void* value) {
    listNode* node;

    // 分配节点内存
    if ((node = zmalloc(sizeof(struct listNode))) == NULL)
        return NULL;

    // 值指针初始化
    node->value = value;

    if (list->len == 0) {
        // 添加节点到空链表
        // 链表头尾指针都指向该节点
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        // 添加节点到非空链表
        node->prev = list->tail;
        node->next = NULL;
        list->tail->next = node;
        list->tail = node;
    }

    // 修改链表节点数目
    list->len++;

    return list;
}

/*
 * 将一个包含给定值指针value的新元素插入到old_node的前面或后面
 * T: O(1)
 */
list* listInsertNode(list* list, listNode* old_node, void* value, int after) {
    listNode* node;

    // 分配节点内存
    if ((node = zmalloc(sizeof(struct listNode))) == NULL)
        return NULL;

    // 值指针初始化
    node->value = value;

    if (after) {
        // 插入old_node之后
        node->prev = old_node;
        node->next = old_node->next;
        // old_node是表尾节点
        if (list->tail == old_node) {
            list->tail = node;
        }
    } else {
        // 插入old_node之前
        node->next = old_node;
        node->prev = old_node->prev;
        // old_node是表头节点
        if (list->head == old_node) {
            list->head = node;
        }
    }

    // 修改node前置节点的next指针
    if (node->prev != NULL) {
        node->prev->next = node;
    }

    // 修改node后置节点的prev指针
    if (node->next != NULL) {
        node->next->prev = node;
    }

    // 修改链表节点数目
    list->len++;

    return list;
}

/*
 * 从链表中删除一个节点
 * T: O(1)
 */
void listDelNode(list* list, listNode* node) {
    if (node->prev)
        node->prev->next = node->next;
    else
        list->head = node->next;

    if (node->next)
        node->next->prev = node->prev;
    else
        list->tail = node->prev;

    // 如果定义了value释放函数，则调用它
    if (list->free) list->free(node->value);

    // 释放节点结构
    zfree(node);

    // 修改链表节点数目
    list->len--;
}

/*
 * 创建一个链表迭代器，正向或反向
 */
listIter* listGetIterator(list* list, int direction) {
    listIter* iter;
    // 分配迭代器内存
    if ((iter = zmalloc(sizeof(struct listIter))) == NULL) return NULL;

    if (direction == AL_START_HEAD)
        iter->next = list->head;
    else
        iter->next = list->tail;

    iter->direction = direction;

    return iter;
}

/*
 * 释放一个链表迭代器
 */
void listReleaseIterator(listIter* iter) {
    zfree(iter);
}

/*
 * 重置链表迭代器，正向
 */
void listRewind(list* list, listIter* li) {
    li->next = list->head;
    li->direction = AL_START_HEAD;
}

/*
 * 重置链表迭代器，反向
 */
void listRewindTail(list* list, listIter* li) {
    li->next = list->tail;
    li->direction = AL_START_TAIL;
}

/*
 * 取当前迭代器指向的节点
 */
listNode* listNext(listIter* iter) {
    listNode* current = iter->next;

    if (current != NULL) {
        if (iter->direction == AL_START_HEAD)
            iter->next = current->next;
        else
            iter->next = current->prev;
    }

    return current;
}

/*
 * 复制一个链表，复制成功则返回链表的副本
 * T = O(N)
 */
list* listDup(list* orig) {
    list* copy;
    listIter* iter;
    listNode* node;

    // 创建新链表
    if ((copy = listCreate()) == NULL)
        return NULL;

    // 设置节点值处理函数
    copy->dup = orig->dup;
    copy->free = orig->free;
    copy->match = orig->match;

    // 迭代整个输入链表
    iter = listGetIterator(orig, AL_START_HEAD);
    while ((node = listNext(iter)) != NULL) {
        void* value;

        // 复制节点值到新节点值
        if (copy->dup) {
            value = copy->dup(node->value);
            if (value == NULL) {
                listRelease(copy);
                listReleaseIterator(iter);
                return NULL;
            }
        } else
            value = node->value;

        // 插入一个包含给定值指针value的新元素作为新链表尾节点
        if (listAddNodeTail(copy, value) == NULL) {
            listRelease(copy);
            listReleaseIterator(iter);
            return NULL;
        }
    }

    // 释放迭代器
    listReleaseIterator(iter);

    return copy;
}

/*
 * 查询链表list中与key匹配的节点
 * 对比操作由链表的match函数执行，如果没有设置match函数，则直接对比值指针来确定是否匹配
 * T: O(N)
 */
listNode* listSearchKey(list* list, void* key) {
    listIter* iter;
    listNode* node;

    // 迭代整个输入链表
    iter = listGetIterator(list, AL_START_HEAD);
    while ((node = listNext(iter)) != NULL) {
        if (list->match) {
            // 通过match函数比较值
            if (list->match(node->value, key)) {
                listReleaseIterator(iter);
                return node;
            }
        } else {
            // 直接比较值
            if (node->value == key) {
                listReleaseIterator(iter);
                return node;
            }
        }
    }

    // 释放迭代器
    listReleaseIterator(iter);

    return NULL;
}

/*
 * 取链表中索引为index的节点，index允许为复数，则从尾节点往前数
 * T: O(N)
 */
listNode* listIndex(list* list, long index) {
    listNode* n;

    if (index < 0) {
        // index为负数，从尾往头找
        index = (-index) - 1;
        n = list->tail;
        while (index-- && n) n = n->prev;
    } else {
        // index为整数，从头往尾找
        n = list->head;
        while (index-- && n) n = n->next;
    }

    return n;
}

/*
 * 将尾节点移动到链表头
 */
void listRotate(list* list) {
    listNode* tail = list->tail;

    if (list->len <= 1) return;

    list->tail = tail->prev;
    list->tail->next = NULL;

    list->head->prev = tail;
    tail->prev = NULL;
    tail->next = list->head;
    list->head = tail;
}