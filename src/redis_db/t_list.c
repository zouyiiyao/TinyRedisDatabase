//
// Created by zouyi on 2021/8/31.
//

#include "redis.h"

/*
 * List API
 */

/*
 * 检查输入值value，如果超出限定长度，则将列表类型对象编码由REDIS_ENCODING_ZIPLIST转化为REDIS_ENCODING_LINKEDLIST
 */
void listTypeTryConversion(robj* subject, robj* value) {

    if (subject->encoding != REDIS_ENCODING_ZIPLIST) return;

    if (sdsEncodesObject(value) && sdslen(value->ptr) > LIST_MAX_ZIPLIST_VALUE)
        listTypeConvert(subject, REDIS_ENCODING_LINKEDLIST);
}

/*
 * 将给定元素添加到列表的表头或表尾
 */
void listTypePush(robj* subject, robj* value, int where) {

    // 检查是否需要进行编码转换以保存value
    listTypeTryConversion(subject, value);

    // 检查列表元素数目是否已经超出限定，如果超出则转换编码为REDIS_ENCODING_LINKEDLIST
    if (subject->encoding == REDIS_ENCODING_ZIPLIST && ziplistLen(subject->ptr) >= LIST_MAX_ZIPLIST_ENTRIES)
        listTypeConvert(subject, REDIS_ENCODING_LINKEDLIST);

    // 如果列表类型对象的底层使用的是压缩列表，则调用压缩列表API: ziplistPush
    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
        int pos = (where == REDIS_HEAD) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
        value = getDecodedObject(value);
        subject->ptr = ziplistPush(subject->ptr, value->ptr, sdslen(value->ptr), pos);
        decrRefCount(value);
    // 如果列表类型对象的底层使用的是双向链表，则调用双向链表API: listAddNodeHead & listAddNodeTail
    } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {
        if (where == REDIS_HEAD) {
            listAddNodeHead(subject->ptr, value);
        } else {
            listAddNodeTail(subject->ptr, value);
        }
        incrRefCount(value);
    } else {
        exit(1);
    }
}

/*
 * 从列表的表头或表尾弹出一个元素
 */
robj* listTypePop(robj* subject, int where) {

    robj* value = NULL;

    // 如果列表类型对象的底层使用的是压缩列表，则调用压缩列表API: ziplistIndex
    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char* p;
        unsigned char* vstr;
        unsigned int vlen;
        long long vlong;

        int pos = (where == REDIS_HEAD) ? 0 : -1;

        p = ziplistIndex(subject->ptr, pos);
        if (ziplistGet(p, &vstr, &vlen, &vlong)) {
            if (vstr) {
                value = createStringObject((char*)vstr, vlen);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
            subject->ptr = ziplistDelete(subject->ptr, &p);
        }
    // 如果列表类型对象的底层使用的是双向链表，则调用双向链表API: listFirst & listLast
    } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {

        list* list = subject->ptr;
        listNode* ln;

        if (where == REDIS_HEAD) {
            ln = listFirst(list);
        } else {
            ln = listLast(list);
        }

        if (ln != NULL) {
            value = listNodeValue(ln);
            incrRefCount(value);
            listDelNode(list, ln);
        }
    } else {
        exit(1);
    }

    return value;
}

/*
 * 返回列表的节点数量
 */
unsigned long listTypeLength(robj* subject) {

    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
        return ziplistLen(subject->ptr);
    } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {
        return listLength((list*)subject->ptr);
    } else {
        exit(1);
    }
}

/*
 * 创建并返回一个列表迭代器，参数index指定开始迭代的索引，参数direction指定迭代的方向
 */
listTypeIterator* listTypeInitIterator(robj* subject, long index, unsigned char direction) {

    listTypeIterator* li = zmalloc(sizeof(listTypeIterator));

    li->subject = subject;

    li->encoding = subject->encoding;

    li->direction = direction;

    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        li->zi = ziplistIndex(subject->ptr, index);
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        li->ln = listIndex(subject->ptr, index);
    } else {
        exit(1);
    }

    return li;
}

/*
 * 释放指定的列表迭代器
 */
void listTypeReleaseIterator(listTypeIterator* li) {
    zfree(li);
}

/*
 * 使用entry记录迭代器当前指向的节点，并将迭代器的指针移动到下一个元素
 */
int listTypeNext(listTypeIterator* li, listTypeEntry* entry) {
    assert(li->subject->encoding == li->encoding);

    entry->li = li;

    // 迭代压缩列表
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        // 记录当前节点
        entry->zi = li->zi;

        if (entry->zi != NULL) {
            // 移动迭代器的指针
            if (li->direction == REDIS_TAIL)
                li->zi = ziplistNext(li->subject->ptr, li->zi);
            else
                li->zi = ziplistPrev(li->subject->ptr, li->zi);
            return 1;
        }
    // 迭代双向链表
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        // 记录当前节点
        entry->ln = li->ln;

        if (entry->ln != NULL) {
            // 移动迭代器的指针
            if (li->direction == REDIS_TAIL)
                li->ln = li->ln->next;
            else
                li->ln = li->ln->prev;
            return 1;
        }
    } else {
        exit(1);
    }

    return 0;
}

/*
 * 返回entry当前记录的节点
 */
robj* listTypeGet(listTypeEntry* entry) {

    listTypeIterator* li = entry->li;

    robj* value = NULL;

    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char* vstr;
        unsigned int vlen;
        long long vlong;
        assert(entry->zi != NULL);
        if (ziplistGet(entry->zi, &vstr, &vlen, &vlong)) {
            if (vstr) {
                value = createStringObject((char*)vstr, vlen);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
        }
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        assert(entry->ln != NULL);
        value = listNodeValue(entry->ln);
        incrRefCount(value);
    } else {
        exit(1);
    }

    return value;
}

/*
 * 将对象value插入到entry当前记录节点的之前或之后
 */
void listTypeInsert(listTypeEntry* entry, robj* value, int where) {

    robj* subject = entry->li->subject;

    if (entry->li->encoding == REDIS_ENCODING_ZIPLIST) {

        value = getDecodedObject(value);

        if (where == REDIS_TAIL) {
            unsigned char* next = ziplistNext(subject->ptr, entry->zi);

            if (next == NULL) {
                subject->ptr = ziplistPush(subject->ptr, value->ptr, sdslen(value->ptr), REDIS_TAIL);
            } else {
                subject->ptr = ziplistInsert(subject->ptr, next, value->ptr, sdslen(value->ptr));
            }
        } else {
            subject->ptr = ziplistInsert(subject->ptr, entry->zi, value->ptr, sdslen(value->ptr));
        }
        decrRefCount(value);
    } else if (entry->li->encoding == REDIS_ENCODING_LINKEDLIST) {
        if (where == REDIS_TAIL) {
            listInsertNode(subject->ptr, entry->ln, value, AL_START_TAIL);
        } else {
            listInsertNode(subject->ptr, entry->ln, value, AL_START_HEAD);
        }

        incrRefCount(value);
    } else {
        exit(1);
    }
}

/*
 * 比较entry记录节点的值和对象o记录的值
 */
int listTypeEqual(listTypeEntry* entry, robj* o) {

    listTypeIterator* li = entry->li;

    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        assert(sdsEncodesObject(o));
        return ziplistCompare(entry->zi, o->ptr, sdslen(o->ptr));
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        return equalStringObjects(o, listNodeValue(entry->ln));
    } else {
        exit(1);
    }
}

/*
 * 删除entry记录的节点
 */
void listTypeDelete(listTypeEntry* entry) {

    listTypeIterator* li = entry->li;

    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char* p = entry->zi;

        li->subject->ptr = ziplistDelete(li->subject->ptr, &p);

        if (li->direction == REDIS_TAIL)
            li->zi = p;
        else
            li->zi = ziplistPrev(li->subject->ptr, p);
    } else if (entry->li->encoding == REDIS_ENCODING_LINKEDLIST) {

        listNode* next;

        if (li->direction == REDIS_TAIL)
            next = entry->ln->next;
        else
            next = entry->ln->prev;

        listDelNode(li->subject->ptr, entry->ln);

        li->ln = next;
    } else {
        exit(1);
    }
}

/*
 * 将列表对象的编码转换为REDIS_ENCODING_LINKEDLIST
 */
void listTypeConvert(robj* subject, int enc) {

    listTypeIterator* li;
    listTypeEntry entry;

    assert(subject->type == REDIS_LIST);

    if (enc == REDIS_ENCODING_LINKEDLIST) {

        list* l = listCreate();
        listSetFreeMethod(l, decrRefCountVoid);

        li = listTypeInitIterator(subject, 0, REDIS_TAIL);
        while (listTypeNext(li, &entry)) listAddNodeTail(l, listTypeGet(&entry));
        listTypeReleaseIterator(li);

        subject->encoding = REDIS_ENCODING_LINKEDLIST;

        zfree(subject->ptr);

        subject->ptr = l;
    } else {
        exit(1);
    }
}
