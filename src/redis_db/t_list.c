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

    if (sdsEncodedObject(value) && sdslen(value->ptr) > LIST_MAX_ZIPLIST_VALUE)
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
        assert(sdsEncodedObject(o));
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

/*
 * 注意: 
 * signalModifiedKey函数与独立功能事务相关，在本代码中删除；
 * notifyKeyspaceEvent函数与独立功能发布/订阅相关，在本代码中删除；
 */

/*
 * List Commands
 */

/*
 * LPUSH，RPUSH命令的底层实现
 *
 * 如果列表对象不存在，则创建一个新的列表对象，插入到数据库的键空间
 */
void pushGenericCommand(redisClient* c, int where) {

    int j;
    // 阻塞相关
    /* int waiting = 0; */
    int pushed = 0;

    robj* lobj = lookupKeyWrite(c->db, c->argv[1]);

    // 阻塞相关
    /* int may_have_waiting_clients = (lobj == NULL); */

    if (lobj && lobj->type != REDIS_LIST) {
        addReply(c, shared.wrongtypeerr);
        return;
    }

    // 阻塞相关
    /* if (may_have_waiting_clients) signalListAsReady(c, c->argv[1]); */

    for (j = 2; j < c->argc; j++) {

        c->argv[j] = tryObjectEncoding(c->argv[j]);

        if (!lobj) {
            lobj = createZiplistObject();
            dbAdd(c->db, c->argv[1], lobj);
        }

        listTypePush(lobj, c->argv[j], where);
        pushed++;
    }

    addReplyLongLong(c, /* waiting + */ (lobj ? listTypeLength(lobj) : 0));

    server.dirty += pushed;
}

/*
 * LPUSH命令
 */
void lpushCommand(redisClient* c) {
    pushGenericCommand(c, REDIS_HEAD);
}

/*
 * RPUSH命令
 */
void rpushCommand(redisClient* c) {
    pushGenericCommand(c, REDIS_TAIL);
}

/*
 * LPUSHX，RPUSHX，LINSERT的底层实现
 *
 * 如果列表对象不存在，则什么也不做
 */
void pushxGenericCommand(redisClient* c, robj* refval, robj* val, int where) {

    robj* subject;
    listTypeIterator* iter;
    listTypeEntry entry;
    int inserted = 0;

    if ((subject = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, subject, REDIS_LIST))
        return;

    // LINSERT
    if (refval != NULL) {
        /* We're not sure if this value can be inserted yet, but we cannot
         * convert the list inside the iterator. We don't want to loop over
         * the list twice (once to see if the value can be inserted and once
         * to do the actual insert), so we assume this value can be inserted
         * and convert the ziplist to a regular list if necessary. */
        listTypeTryConversion(subject, val);

        /* Seek refval from head to tail */
        iter = listTypeInitIterator(subject, 0, REDIS_TAIL);
        while (listTypeNext(iter, &entry)) {
            if (listTypeEqual(&entry, refval)) {
                listTypeInsert(&entry, val, where);
                inserted = 1;
                break;
            }
        }
        listTypeReleaseIterator(iter);

        if (inserted) {
            /* Check if the length exceeds the ziplist length threshold. */
            if (subject->encoding == REDIS_ENCODING_ZIPLIST &&
                ziplistLen(subject->ptr) > server.list_max_ziplist_entries)
                listTypeConvert(subject, REDIS_ENCODING_LINKEDLIST);

            server.dirty++;
        } else {
            /* Notify client of a failed insert */
            addReply(c, shared.cnegone);
            return;
        }

    // LPUSHX or RPUSHX
    } else {

        listTypePush(subject, val, where);

        server.dirty++;
    }

    addReplyLongLong(c, listTypeLength(subject));
}

/*
 * LPUSHX命令
 */
void lpushxCommand(redisClient* c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    pushxGenericCommand(c, NULL, c->argv[2], REDIS_HEAD);
}

/*
 * RPUSHX命令
 */
void rpushxCommand(redisClient* c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    pushxGenericCommand(c, NULL, c->argv[2], REDIS_TAIL);
}

/*
 * LINSERT命令
 */
void linsertCommand(redisClient* c) {

    c->argv[4] = tryObjectEncoding(c->argv[4]);

    if (strcasecmp(c->argv[2]->ptr, "after") == 0) {
        pushxGenericCommand(c, c->argv[3], c->argv[4], REDIS_TAIL);

    } else if (strcasecmp(c->argv[2]->ptr, "before") == 0) {
        pushxGenericCommand(c, c->argv[3], c->argv[4], REDIS_HEAD);

    } else {
        addReply(c, shared.syntaxerr);
    }
}

/*
 * LPOP，RPOP命令的底层实现
 */
void popGenericCommand(redisClient* c, int where) {

    robj* o = lookupKeyWriteOrReply(c, c->argv[1], shared.nullbulk);

    if (o == NULL || checkType(c, o, REDIS_LIST)) return;

    robj* value = listTypePop(o, where);

    if (value == NULL) {
        addReply(c, shared.nullbulk);
    } else {

        addReplyBulk(c, value);
        decrRefCount(value);

        if (listTypeLength(o) == 0) {
            dbDelete(c->db, c->argv[1]);
        }

        server.dirty++;
    }
}

/*
 * LPOP命令
 */
void lpopCommand(redisClient* c) {
    popGenericCommand(c, REDIS_HEAD);
}

/*
 * RPOP命令
 */
void rpopCommand(redisClient* c) {
    popGenericCommand(c, REDIS_TAIL);
}

/*
 * LLEN命令
 */
void llenCommand(redisClient* c) {

    robj* o = lookupKeyReadOrReply(c, c->argv[1], shared.czero);

    if (o == NULL || checkType(c, o, REDIS_LIST)) return;

    addReplyLongLong(c, listTypeLength(o));
}

/*
 * LINDEX命令
 */
void lindexCommand(redisClient* c) {

    robj* o = lookupKeyReadOrReply(c, c->argv[1], shared.nullbulk);

    if (o == NULL || checkType(c, o, REDIS_LIST)) return;
    long index;
    robj* value = NULL;

    if (getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != REDIS_OK)
        return;

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char* p;
        unsigned char* vstr;
        unsigned int vlen;
        long long vlong;

        p = ziplistIndex(o->ptr, index);

        if (ziplistGet(p, &vstr, &vlen, &vlong)) {
            if (vstr) {
                value = createStringObject((char*)vstr, vlen);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
            addReplyBulk(c, value);
            decrRefCount(value);
        } else {
            addReply(c, shared.nullbulk);
        }

    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {

        listNode* ln = listIndex(o->ptr, index);

        if (ln != NULL) {
            value = listNodeValue(ln);
            addReplyBulk(c, value);
        } else {
            addReply(c, shared.nullbulk);
        }
    } else {
        exit(1);
    }
}

/*
 * LREM命令
 */
void lremCommand(redisClient* c) {

    robj* subject;
    robj* obj;

    obj = c->argv[3] = tryObjectEncoding(c->argv[3]);
    long toremove;
    long removed = 0;
    listTypeEntry entry;

    if (getLongFromObjectOrReply(c, c->argv[2], &toremove, NULL) != REDIS_OK)
        return;

    subject = lookupKeyWriteOrReply(c, c->argv[1], shared.czero);
    if (subject == NULL || checkType(c, subject, REDIS_LIST)) return;

    /* Make sure obj is raw when we're dealing with a ziplist */
    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
        obj = getDecodedObject(obj);

    listTypeIterator* li;

    if (toremove < 0) {
        toremove = -toremove;
        li = listTypeInitIterator(subject, -1, REDIS_HEAD);
    } else {
        li = listTypeInitIterator(subject, 0, REDIS_TAIL);
    }

    while (listTypeNext(li, &entry)) {
        if (listTypeEqual(&entry, obj)) {
            listTypeDelete(&entry);
            server.dirty++;
            removed++;
            if (toremove && removed == toremove) break;
        }
    }
    listTypeReleaseIterator(li);

    /* Clean up raw encoded object */
    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
        decrRefCount(obj);

    // 删除空列表对象
    if (listTypeLength(subject) == 0) dbDelete(c->db, c->argv[1]);

    addReplyLongLong(c, removed);
}

/*
 * LTRIM命令
 */
void ltrimCommand(redisClient* c) {

    robj* o;
    long start;
    long end;
    long llen;
    long j;
    long ltrim;
    long rtrim;
    list* list;
    listNode* ln;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK))
        return;

    if ((o = lookupKeyWriteOrReply(c, c->argv[1], shared.ok)) == NULL || checkType(c, o, REDIS_LIST))
        return;

    llen = listTypeLength(o);

    if (start < 0) start = llen + start;
    if (end < 0) end = llen + end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list */
        ltrim = llen;
        rtrim = 0;
    } else {
        if (end >= llen) end = llen - 1;
        ltrim = start;
        rtrim = llen - end - 1;
    }

    // 删除两端元素
    /* Remove list elements to perform the trim */
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {

        o->ptr = ziplistDeleteRange(o->ptr, 0, ltrim);
        o->ptr = ziplistDeleteRange(o->ptr, -rtrim, rtrim);
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        list = o->ptr;
        for (j = 0; j < ltrim; j++) {
            ln = listFirst(list);
            listDelNode(list, ln);
        }

        for (j = 0; j < rtrim; j++) {
            ln = listLast(list);
            listDelNode(list, ln);
        }
    } else {
        exit(1);
    }

    if (listTypeLength(o) == 0) {
        dbDelete(c->db, c->argv[1]);
    }

    server.dirty++;

    addReply(c, shared.ok);
}

/*
 * LSET命令
 */
void lsetCommand(redisClient* c) {

    robj* o = lookupKeyWriteOrReply(c, c->argv[1], shared.nokeyerr);

    if (o == NULL || checkType(c, o, REDIS_LIST)) return;
    long index;

    // 取value
    robj* value = (c->argv[3] = tryObjectEncoding(c->argv[3]));

    // 取index
    if (getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != REDIS_OK)
        return;

    // 查看保存value值对象是否需要转换列表的底层编码
    listTypeTryConversion(o, value);

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {

        unsigned char* p;
        unsigned char* zl = o->ptr;

        p = ziplistIndex(zl, index);
        if (p == NULL) {
            addReply(c, shared.outofrangeerr);
        } else {
            o->ptr = ziplistDelete(o->ptr, &p);
            value = getDecodedObject(value);
            o->ptr = ziplistInsert(o->ptr, p, value->ptr, sdslen(value->ptr));
            decrRefCount(value);

            addReply(c, shared.ok);

            server.dirty++;
        }

    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {

        listNode* ln = listIndex(o->ptr, index);

        if (ln == NULL) {
            addReply(c, shared.outofrangeerr);
        } else {
            decrRefCount((robj*)listNodeValue(ln));
            listNodeValue(ln) = value;
            incrRefCount(value);

            addReply(c, shared.ok);

            server.dirty++;
        }
    } else {
        exit(1);
    }
}
