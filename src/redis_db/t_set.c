//
// Created by zouyi on 2021/9/5.
//

#include "redis.h"

/*
 * Set API
 */

/*
 * 以能够保存对象value的值的编码方式创建一个集合
 * 如果对象value的值能表示为long long，则使用REDIS_ENCODING_INTSET编码；
 * 否则使用REDIS_ENCODING_HT编码；
 */
robj* setTypeCreate(robj* value) {

    if (isObjectRepresentableAsLongLong(value, NULL) == REDIS_OK)
        return createIntsetObject();

    return createSetObject();
}

/*
 * 向指定集合中添加一个元素
 */
int setTypeAdd(robj* subject, robj* value) {
    long long llval;

    // 如果集合类型对象底层编码为REDIS_ENCODING_HT，则调用字典API: dictAdd
    if (subject->encoding == REDIS_ENCODING_HT) {
        if (dictAdd(subject->ptr, value, NULL) == DICT_OK) {
            incrRefCount(value);
            return 1;
        }
    // 如果集合类型对象底层编码为REDIS_ENCODING_INTSET，且value的值能表示成long long(intset能保存)，
    // 则调用整数集合API: intsetAdd，如果添加后元素数目超过SET_MAX_INTSET_ENTRIES，则转为REDIS_ENCODING_HT编码；
    // 若value的值不能表示成long long，先转为REDIS_ENCODING_HT编码，再调用字典API: dictAdd；
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {

        if (isObjectRepresentableAsLongLong(value, &llval) == REDIS_OK) {
            uint8_t success = 0;
            subject->ptr = intsetAdd(subject->ptr, llval, &success);
            if (success) {
                if (intsetLen(subject->ptr) > SET_MAX_INTSET_ENTRIES)
                    setTypeConvert(subject, REDIS_ENCODING_HT);
                return 1;
            }
        } else {
            setTypeConvert(subject, REDIS_ENCODING_HT);

            assert(dictAdd(subject->ptr, value, NULL) == DICT_OK);
            incrRefCount(value);
            return 1;
        }
    } else {
        exit(1);
    }

    return 0;
}

/*
 * 在指定集合中删除元素value
 */
int setTypeRemove(robj* setobj, robj* value) {
    long long llval;

    if (setobj->encoding == REDIS_ENCODING_HT) {
        if (dictDelete(setobj->ptr, value) == DICT_OK) {
            if (htNeedsResize(setobj->ptr)) dictResize(setobj->ptr);
            return 1;
        }
    } else if (setobj->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value, &llval) == REDIS_OK) {
            int success;
            setobj->ptr = intsetRemove(setobj->ptr, llval, &success);
            if (success) return 1;
        }
    } else {
        exit(1);
    }

    return 0;
}

/*
 * 检查元素value是否在集合中
 */
int setTypeIsMember(robj* subject, robj* value) {
    long long llval;

    if (subject->encoding == REDIS_ENCODING_HT) {
        return dictFind((dict*)subject->ptr, value) != NULL;
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value, &llval) == REDIS_OK) {
            return intsetFind((intset*)subject->ptr, llval);
        }
    } else {
        exit(1);
    }

    return 0;
}

/*
 * 初始化一个集合类型对象迭代器
 */
setTypeIterator* setTypeInitIterator(robj* subject) {

    setTypeIterator* si = zmalloc(sizeof(setTypeIterator));

    si->subject = subject;

    si->encoding = subject->encoding;

    if (si->encoding == REDIS_ENCODING_HT) {
        si->di = dictGetIterator(subject->ptr);
    } else if (si->encoding == REDIS_ENCODING_INTSET) {
        si->ii = 0;
    } else {
        exit(1);
    }

    return si;
}

/*
 * 释放一个集合类型对象迭代器
 */
void setTypeReleaseIterator(setTypeIterator* si) {

    if (si->encoding == REDIS_ENCODING_HT)
        dictReleaseIterator(si->di);

    zfree(si);
}

/*
 * 取迭代器当前指向的对象，返回集合类型对象的编码方式
 * 这个函数不增加引用计数，所以是copy-on-write友好的
 */
int setTypeNext(setTypeIterator* si, robj** objele, int64_t* llele) {

    if (si->encoding == REDIS_ENCODING_HT) {

        dictEntry* de = dictNext(si->di);

        if (de == NULL) return -1;

        *objele = dictGetKey(de);
    } else if (si->encoding == REDIS_ENCODING_INTSET) {
        if (!intsetGet(si->subject->ptr, si->ii++, llele))
            return -1;
    }

    return si->encoding;
}

/*
 * 取迭代器当前指向的对象，会返回一个新的或者增加引用计数的对象(非copy-on-write友好)
 * 调用者在使用完对象之后，应该对对象调用decrRefCount
 */
robj* setTypeNextObject(setTypeIterator* si) {
    int64_t intele;
    robj* objele;
    int encoding;

    encoding = setTypeNext(si, &objele, &intele);

    switch (encoding) {
        case -1:
            return NULL;
        case REDIS_ENCODING_INTSET:
            return createStringObjectFromLongLong(intele);
        case REDIS_ENCODING_HT:
            incrRefCount(objele);
            return objele;
        default:
            exit(1);
    }
}

/*
 * 从集合类型对象中随机取出一个元素
 */
int setTypeRandomElement(robj* setobj, robj** objele, int64_t* llele) {

    if (setobj->encoding == REDIS_ENCODING_HT) {
        dictEntry* de = dictGetRandomKey(setobj->ptr);
        *objele = dictGetKey(de);
    } else if (setobj->encoding == REDIS_ENCODING_INTSET) {
        *llele = intsetRandom(setobj->ptr);
    } else {
        exit(1);
    }

    return setobj->encoding;
}

/*
 * 返回集合类型对象的元素个数
 */
unsigned long setTypeSize(robj* subject) {

    if (subject->encoding == REDIS_ENCODING_HT) {
        return dictSize((dict*)subject->ptr);
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        return intsetLen((intset*)subject->ptr);
    } else {
        exit(1);
    }
}

/*
 * 将集合类型对象底层编码从REDIS_ENCODING_INTSET转REDIS_ENCODING_HT
 */
void setTypeConvert(robj* setobj, int enc) {

    setTypeIterator* si;

    assert(setobj->type == REDIS_SET && setobj->encoding == REDIS_ENCODING_INTSET);

    if (enc == REDIS_ENCODING_HT) {
        int64_t intele;
        dict* d = dictCreate(&setDictType, NULL);
        robj* element;

        dictExpand(d, intsetLen(setobj->ptr));

        si = setTypeInitIterator(setobj);
        while (setTypeNext(si, NULL, &intele) != -1) {
            element = createStringObjectFromLongLong(intele);
            assert(dictAdd(d, element, NULL) == DICT_OK);
        }
        setTypeReleaseIterator(si);

        setobj->encoding = REDIS_ENCODING_HT;
        zfree(setobj->ptr);
        setobj->ptr = d;
    } else {
        exit(1);
    }
}

/*
 * Set Commands
 */

/*
 * sadd
 */
void saddCommand(redisClient* c) {
    robj* set;
    int j;
    int added = 0;

    set = lookupKeyWrite(c->db, c->argv[1]);

    if (set == NULL) {
        set = setTypeCreate(c->argv[2]);
        dbAdd(c->db, c->argv[1], set);
    } else {
        if (set->type != REDIS_SET) {
            addReply(c, shared.wrongtypeerr);
            return;
        }
    }

    for (j = 2; j < c->argc; j++) {
        c->argv[j] = tryObjectEncoding(c->argv[j]);
        if (setTypeAdd(set, c->argv[j])) added++;
    }

    /*
     * if (added) {
     *     signalModifiedKey(c->db, c->argv[1]);
     *     notifyKeyspaceEvent(REDIS_NOTIFY_SET, "sadd", c->argv[1], c->db->id);
     * }
     */

    server.dirty += added;

    addReplyLongLong(c, added);
}

/*
 * srem
 */
void sremCommand(redisClient* c) {
    robj* set;
    int j;
    int deleted = 0;
    /* int keyremoved = 0; */

    if ((set = lookupKeyWriteOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, set, REDIS_SET))
        return;

    for (j = 2; j < c->argc; j++) {

        if (setTypeRemove(set, c->argv[j])) {
            deleted++;
            if (setTypeSize(set) == 0) {
                dbDelete(c->db, c->argv[1]);
                keyremoved = 1;
                break;
            }
        }
    }

    if (deleted) {
        /* signalModifiedKey(c->db, c->argv[1]); */
        /* notifyKeyspaceEvent(REDIS_NOTIFY_SET, "srem", c->argv[1], c->db->id); */

        /*
         * if (keyremoved)
         *     notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
         */

        server.dirty += deleted;
    }

    addReplyLongLong(c, deleted);
}

/*
 * scard
 */
void scardCommand(redisClient* c) {
    robj* o;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, o, REDIS_SET))
        return;

    addReplyLongLong(c, setTypeSize(o));
}

/*
 * sismember
 */
void sismemberCommand(redisClient* c) {
    robj* set;

    if ((set = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, set, REDIS_SET))
        return;

    c->argv[2] = tryObjectEncoding(c->argv[2]);

    if (setTypeIsMember(set, c->argv[2]))
        addReply(c, shared.cone);
    else
        addReply(c, shared.czero);
}

/*
 * 计算集合s1的基数减去集合s2的基数
 */
int qsortCompareSetsByCardinality(const void* s1, const void* s2) {
    return setTypeSize(*(robj**)s1) - setTypeSize(*(robj**)s2);
}

/*
 * 计算集合s2的基数减去集合s1的基数
 */
int qsortCompareSetByRevCardinality(const void* s1, const void* s2) {
    robj* o1 = *(robj**)s1;
    robj* o2 = *(robj**)s2;
    return (o2 ? setTypeSize(o2) : 0) - (o1 ? setTypeSize(o1) : 0);
}

/*
 * sinter，sinterstore底层实现
 */
void sinterGenericCommand(redisClient* c, robj** setkeys, unsigned long setnum, robj* dstkey) {

    robj** sets = zmalloc(sizeof(robj*) * setnum);

    setTypeIterator* si;
    robj* eleobj;
    robj* dstset = NULL;
    int64_t intobj;
    void* replylen = NULL;
    unsigned long j;
    unsigned long cardinality = 0;
    int encoding;

    for (j = 0; j < setnum; j++) {

        robj* setobj = dstkey ? lookupKeyWrite(c->db, setkeys[j]) : lookupKeyRead(c->db, setkeys[j]);

        if (!setobj) {
            zfree(sets);
            if (dstkey) {
                if (dbDelete(c->db, dstkey)) {
                    /* signalModifiedKey(c->db, dstkey); */
                    server.dirty++;
                }
                addReply(c, shared.czero);
            } else {
                addReply(c, shared.emptymultibulk);
            }
            return;
        }

        if (checkType(c, setobj, REDIS_SET)) {
            zfree(sets);
            return;
        }

        sets[j] = setobj;
    }

    /* Sort sets from the smallest to largest, this will improve our
     * algorithm's performance */
    qsort(sets, setnum, sizeof(robj*), qsortCompareSetsByCardinality);

    /* The first thing we should output is the total number of elements...
     * since this is a multi-bulk write, but at this stage we don't know
     * the intersection set size, so we use a trick, append an empty object
     * to the output list and save the pointer to later modify it with the
     * right length */
    if (!dstkey) {
        replylen = addDeferredMultiBulkLength(c);
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with an empty set inside */
        dstset = createIntsetObject();
    }

    /* Iterate all the elements of the first (smallest) set, and test
     * the element against all the other sets, if at least one set does
     * not include the element it is discarded */
    si = setTypeInitIterator(sets[0]);
    while ((encoding = setTypeNext(si, &eleobj, &intobj)) != -1) {

        for (j = 1; j < setnum; j++) {

            if (sets[j] == sets[0]) continue;

            if (encoding == REDIS_ENCODING_INTSET) {
                if (sets[j]->encoding == REDIS_ENCODING_INTSET && !intsetFind((intset*)sets[j]->ptr, intobj)) {
                    break;
                /* in order to compare an integer with an object we
                 * have to use the generic function, creating an object
                 * for this */
                } else if (sets[j]->encoding == REDIS_ENCODING_HT) {
                    eleobj = createStringObjectFromLongLong(intobj);
                    if (!setTypeIsMember(sets[j], eleobj)) {
                        decrRefCount(eleobj);
                        break;
                    }
                    decrRefCount(eleobj);
                }

            } else if (encoding == REDIS_ENCODING_HT) {
                /* Optimization... if the source object is integer
                 * encoded AND the target set is an intset, we can get
                 * a much faster path. */
                if (eleobj->encoding == REDIS_ENCODING_INT &&
                    sets[j]->encoding == REDIS_ENCODING_INTSET &&
                    !intsetFind((intset*)sets[j]->ptr, (long)eleobj->ptr)) {
                    break;
                /* else... object to object check is easy as we use the
                 * type agnostic API here. */
                } else if (!setTypeIsMember(sets[j], eleobj)) {
                    break;
                }
            }
        }

        /* Only take action when all sets contain the member */
        if (j == setnum) {

            // SINTER
            if (!dstkey) {
                if (encoding == REDIS_ENCODING_HT)
                    addReplyBulk(c, eleobj);
                else
                    addReplyBulkLongLong(c, intobj);
                cardinality++;

            // SINTERSTORE
            } else {
                if (encoding == REDIS_ENCODING_INTSET) {
                    eleobj = createStringObjectFromLongLong(intobj);
                    setTypeAdd(dstset, eleobj);
                    decrRefCount(eleobj);
                } else {
                    setTypeAdd(dstset, eleobj);
                }
            }
        }
    }
    setTypeReleaseIterator(si);

    // SINTERSTORE
    if (dstkey) {
        /* Store the resulting set into the target, if the intersection
         * is not an empty set. */
        // 若dstkey已经存在，则从键空间中将其删除
        int deleted = dbDelete(c->db, dstkey);

        if (setTypeSize(dstset) > 0) {
            dbAdd(c->db, dstkey, dstset);
            addReplyLongLong(c, setTypeSize(dstset));
            /* notifyKeyspaceEvent(REDIS_NOTIFY_SET, "sinterstore", dstkey, c->db->id); */
        } else {
            decrRefCount(dstset);
            addReply(c, shared.czero);
            /*
             * if (deleted)
             *     notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "del", dstkey, c->db->id);
             */
        }

        /* signalModifiedKey(c->db, dstkey); */

        server.dirty++;

    // SINTER
    } else {
        setDeferredMultiBulkLength(c, replylen, cardinality);
    }

    zfree(sets);
}

/*
 * sinter
 */
void sinterCommand(redisClient* c) {
    sinterGenericCommand(c, c->argv + 1, c->argc - 1, NULL);
}

/*
 * 集合操作的类型
 */
#define REDIS_OP_UNION 0
#define REDIS_OP_DIFF 1
#define REDIS_OP_INTER 2

/*
 * sdiff，sdiffstore，sunion，sunionstore底层实现
 */
void sunionDiffGenericCommand(redisClient* c, robj** setkeys, int setnum, robj* dstkey, int op) {

    robj** sets = zmalloc(sizeof(robj*) * setnum);

    setTypeIterator* si;
    robj* ele;
    robj* dstset = NULL;
    int j;
    int cardinality = 0;
    int diff_algo = 1;

    for (j = 0; j < setnum; j++) {
        robj* setobj = dstkey ? lookupKeyWrite(c->db, setkeys[j]) : lookupKeyRead(c->db, setkeys[j]);

        if (!setobj) {
            sets[j] = NULL;
            continue;
        }

        if (checkType(c, setobj, REDIS_SET)) {
            zfree(sets);
            return;
        }

        sets[j] = setobj;
    }

    // 估计计算差集的时间复杂度，选择合适的算法
    /* Select what DIFF algorithm to use.
     *
     * Algorithm 1 is O(N*M) where N is the size of the element first set
     * and M the total number of sets.
     *
     * Algorithm 2 is O(N) where N is the total number of elements in all
     * the sets.
     *
     * We compute what is the best bet with the current input here.
     *
     */
    if (op == REDIS_OP_DIFF && sets[0]) {
        long long algo_one_work = 0;
        long long algo_two_work = 0;

        for (j = 0; j < setnum; j++) {
            if (sets[j] == NULL) continue;

            algo_one_work += setTypeSize(sets[0]);
            algo_two_work += setTypeSize(sets[j]);
        }

        /* Algorithm 1 has better constant times and performs less operations
         * if there are elements in common. Give it some advantage. */
        algo_one_work /= 2;
        diff_algo = (algo_one_work <= algo_two_work) ? 1 : 2;

        if (diff_algo == 1 && setnum > 1) {
            /* With algorithm 1 it is better to order the sets to subtract
             * by decreasing size, so that we are more likely to find
             * duplicated elements ASAP. */
            qsort(sets + 1, setnum - 1, sizeof(robj*), qsortCompareSetByRevCardinality);
        }
    }

    /* We need a temp set object to store our union. If the dstkey
     * is not NULL (that is, we are inside an SUNIONSTORE operation) then
     * this set object will be the resulting object to set into the target key
     */
    dstset = createIntsetObject();

    if (op == REDIS_OP_UNION) {
        /* Union is trivial, just add every element of every set to the
         * temporary set. */
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue;

            si = setTypeInitIterator(sets[j]);
            while ((ele = setTypeNextObject(si)) != NULL) {
                if (setTypeAdd(dstset, ele)) cardinality++;
                decrRefCount(ele);
            }
            setTypeReleaseIterator(si);
        }
    // 计算差集，使用算法1，迭代源集合，对每个元素，依次判断是否出现在剩下的集合中，如果都没有出现，加入到结果集合中
    } else if (op == REDIS_OP_DIFF && sets[0] && diff_algo == 1) {
        /* DIFF Algorithm 1:
         *
         * We perform the diff by iterating all the elements of the first set,
         * and only adding it to the target set if the element does not exist
         * into all the other sets.
         *
         * This way we perform at max N*M operations, where N is the size of
         * the first set, and M the number of sets.
         */
        si = setTypeInitIterator(sets[0]);
        while ((ele = setTypeNextObject(si)) != NULL) {

            for (j = 1; j < setnum; j++) {
                if (!sets[j]) continue;
                if (sets[j] == sets[0]) break;
                if (setTypeIsMember(sets[j], ele)) break;
            }

            if (j == setnum) {
                /* There is no other set with this element. Add it. */
                setTypeAdd(dstset, ele);
                cardinality++;
            }

            decrRefCount(ele);
        }
        setTypeReleaseIterator(si);

    // 计算差集，使用算法2，先将源集合元素全部加入到结果集合中，再逐个遍历剩下的集合，将出现的元素删除
    } else if (op == REDIS_OP_DIFF && sets[0] && diff_algo == 2) {
        /* DIFF Algorithm 2:
         *
         * Add all the elements of the first set to the auxiliary set.
         * Then remove all the elements of all the next sets from it.
         *
         * This is O(N) where N is the sum of all the elements in every set.
         */
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue;

            si = setTypeInitIterator(sets[j]);
            while ((ele = setTypeNextObject(si)) != NULL) {

                if (j == 0) {
                    if (setTypeAdd(dstset, ele)) cardinality++;
                } else {
                    if (setTypeRemove(dstset, ele)) cardinality--;
                }
                decrRefCount(ele);
            }
            setTypeReleaseIterator(si);

            /* Exit if result set is empty as any additional removal
             * of elements will have no effect. */
            if (cardinality == 0) break;
        }
    }

    // SDIFF or SUNION
    /* Output the content of the resulting set, if not in STORE mode */
    if (!dstkey) {
        addReplyMultiBulkLen(c, cardinality);

        si = setTypeInitIterator(dstset);
        while ((ele = setTypeNextObject(si)) != NULL) {
            addReplyBulk(c, ele);
            decrRefCount(ele);
        }
        setTypeReleaseIterator(si);

        decrRefCount(dstset);

    // SDIFFSTORE or SUNIONSTORE
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with the result set inside */
        int deleted = dbDelete(c->db, dstkey);

        if (setTypeSize(dstset) > 0) {
            dbAdd(c->db, dstkey, dstset);
            addReplyLongLong(c, setTypeSize(dstset));

            /* notifyKeyspaceEvent(REDIS_NOTIFY_SET, op == REDIS_OP_UNION ? "sunionstore" : "sdiffstore", dstkey, c->db->id); */

        // 结果集为空
        } else {
            decrRefCount(dstset);
            addReply(c, shared.czero);

            /*
             * if (deleted)
             *     notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "del", dstkey, c->db->id);
             */
        }

        /* signalModifiedKey(c->db, dstkey); */

        server.dirty++;
    }

    zfree(sets);
}

/*
 * sunion
 */
void sunionCommand(redisClient* c) {
    sunionDiffGenericCommand(c, c->argv + 1, c->argc - 1, NULL, REDIS_OP_UNION);
}

/*
 * sdiff
 */
void sdiffCommand(redisClient* c) {
    sunionDiffGenericCommand(c, c->argv + 1, c->argc - 1, NULL, REDIS_OP_DIFF);
}


/*
 * srandmember带count参数时的底层实现
 */
/* How many times bigger should be the set compared to the requested size
 * for us to don't use the "remove elements" strategy? Read later in the
 * implementation for more info.
 */
#define SRANDMEMBER_SUB_STRATEGY_MUL 3

void srandmemberWithCountCommand(redisClient* c) {
    long l;
    unsigned long count;
    unsigned long size;
    int uniq = 1;
    robj* set;
    robj* ele;
    int64_t llele;
    int encoding;
    dict* d;

    if (getLongFromObjectOrReply(c, c->argv[2], &l, NULL) != REDIS_OK) return;
    if (l >= 0) {
        count = (unsigned)l;
    } else {
        /* A negative count means: return the same elements multiple times
         * (i.e. don't remove the extracted element after every extraction). */
        count = -l;
        uniq = 0;
    }

    if ((set = lookupKeyReadOrReply(c, c->argv[1], shared.emptymultibulk)) == NULL || checkType(c, set, REDIS_SET))
        return;
    size = setTypeSize(set);

    if (count == 0) {
        addReply(c, shared.emptymultibulk);
        return;
    }

    // count是负数，表示返回元素允许重复，则直接随机取count次，不需要去重
    /* CASE 1: The count was negative, so the extraction method is just:
     * "return N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures.
     */
    if (!uniq) {
        addReplyMultiBulkLen(c, count);

        while (count--) {
            encoding = setTypeRandomElement(set, &ele, &llele);
            if (encoding == REDIS_ENCODING_INTSET) {
                addReplyBulkLongLong(c, llele);
            } else {
                addReplyBulk(c, ele);
            }
        }

        return;
    }

    // count是正数，返回元素不允许重复，而且所需元素大于整个集合大小，返回整个集合
    /* CASE 2:
     * The number of requested elements is greater than the number of
     * elements inside the set: simply return the whole set.
     */
    if (count >= size) {
        sunionDiffGenericCommand(c, c->argv + 1, 1, NULL, REDIS_OP_UNION);
        return;
    }

    /* For CASE 3 and CASE 4 we need an auxiliary dictionary. */
    d = dictCreate(&setDictType, NULL);

    // 所需元素与整个集合大小接近，先把集合加入到一个辅助字典中，再从中删除元素，直到字典元素个数等于count
    /* CASE 3:
     *
     * The number of elements inside the set is not greater than
     * SRANDMEMBER_SUB_STRATEGY_MUL times the number of requested elements.
     *
     * In this case we create a set from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requsted elements is just
     * a bit less than the number of elements in the set, the natural approach
     * used into CASE 3 is highly inefficient.
     */
    if (count * SRANDMEMBER_SUB_STRATEGY_MUL > size) {

        setTypeIterator* si;

        /* Add all the elements into the temporary dictionary. */
        si = setTypeInitIterator(set);
        while ((encoding = setTypeNext(si, &ele, &llele)) != -1) {
            int retval = DICT_ERR;

            if (encoding == REDIS_ENCODING_INTSET) {
                retval = dictAdd(d, createStringObjectFromLongLong(llele), NULL);
            } else {
                retval = dictAdd(d, dupStringObject(ele), NULL);
            }
            assert(retval == DICT_OK);
        }
        setTypeReleaseIterator(si);
        assert(dictSize(d) == size);

        /* Remove random elements to reach the right count. */
        while (size > count) {
            dictEntry* de;

            de = dictGetRandomKey(d);
            dictDelete(d, dictGetKey(de));

            size--;
        }

    // 所需元素个数远小于集合大小，则随机取元素，加入到辅助字典，直到辅助字典大小等于count
    /* CASE 4: We have a big set compared to the requested number of elements.
     *
     * In this case we can simply get random elements from the set and add
     * to the temporary set, trying to eventually get enough unique elements
     * to reach the specified count.
     */
    } else {
        unsigned long added = 0;

        while (added < count) {

            encoding = setTypeRandomElement(set, &ele, &llele);

            if (encoding == REDIS_ENCODING_INTSET) {
                ele = createStringObjectFromLongLong(llele);
            } else {
                ele = dupStringObject(ele);
            }

            /* Try to add the object to the dictionary. If it already exists
             * free it, otherwise increment the number of objects we have
             * in the result dictionary. */
            if (dictAdd(d, ele, NULL) == DICT_OK)
                added++;
            else
                decrRefCount(ele);
        }
    }

    /* CASE 3 & 4: send the result to the user. */
    dictIterator* di;
    dictEntry* de;

    addReplyMultiBulkLen(c, count);
    di = dictGetIterator(d);
    while ((de = dictNext(di)) != NULL)
        addReplyBulk(c, dictGetKey(de));
    dictReleaseIterator(di);
    dictRelease(d);
}

/*
 * srandmember
 */
void srandmemberCommand(redisClient* c) {
    robj* set;
    robj* ele;
    int64_t llele;
    int encoding;

    if (c->argc == 3) {
        srandmemberWithCountCommand(c);
        return;
    } else if (c->argc > 3) {
        addReply(c, shared.syntaxerr);
        return;
    }

    if ((set = lookupKeyReadOrReply(c, c->argv[1], shared.nullbulk)) == NULL || checkType(c, set, REDIS_SET))
        return;

    encoding = setTypeRandomElement(set, &ele, &llele);
    if (encoding == REDIS_ENCODING_INTSET) {
        addReplyBulkLongLong(c, llele);
    } else {
        addReplyBulk(c, ele);
    }
}

/*
 * spop
 */
void spopCommand(redisClient* c) {
    robj* set;
    robj* ele;
    robj* aux;
    int64_t llele;
    int encoding;

    if ((set = lookupKeyWriteOrReply(c, c->argv[1], shared.nullbulk)) == NULL || checkType(c, set, REDIS_SET))
        return;

    encoding = setTypeRandomElement(set, &ele, &llele);

    if (encoding == REDIS_ENCODING_INTSET) {
        ele = createStringObjectFromLongLong(llele);
        set->ptr = intsetRemove(set->ptr, llele, NULL);
    } else {
        incrRefCount(ele);
        setTypeRemove(set, ele);
    }

    /* notifyKeyspaceEvent(REDIS_NOTIFY_SET, "spop", c->argv[1], c->db->id); */

    /* Replicate/AOF this command as an SREM operation */
    aux = createStringObject("SREM", 4);
    rewriteClientCommandVector(c, 3, aux, c->argv[1], ele);
    /*
     * 这里3.0版本源代码是有问题的，不应该在这里释放ele
     *
     * decrRefCount(ele);
     */
    decrRefCount(aux);

    addReplyBulk(c, ele);
    /* 4.0稳定版已修复，先发送Reply，再释放ele */
    decrRefCount(ele);

    if (setTypeSize(set) == 0) {
        dbDelete(c->db, c->argv[1]);
        /* notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "del", c->argv[1], c->db->id); */
    }

    /* signalModifiedKey(c->db, c->argv[1]); */

    server.dirty++;
}
