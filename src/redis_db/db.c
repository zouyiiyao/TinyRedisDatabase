//
// Created by zouyi on 2021/9/15.
//

#include <signal.h>
#include <ctype.h>

#include "redis.h"

/*
 * C-level DB API
 */

/*
 * 从数据库db中取出键key的值对象
 */
robj* lookupKey(redisDb* db, robj* key) {

    // 查找键空间
    dictEntry* de = dictFind(db->dict, key->ptr);

    // 键存在，返回val
    if (de) {

        robj* val = dictGetVal(de);

        // 每次读取键，都更新它的LRU(最近一次使用)时间
        /* Update the access time for the ageing algorithm.
         * Don't do it if we have a saving child, as this will trigger
         * a copy on write madness. */
        if (server.rdb_child_pid == -1 && server.aof_child_pid == -1)
            val->lru = getLRUClock();

        return val;
    // 键不存在，返回NULL
    } else {
        return NULL;
    }
}

/*
 * 为读取操作取出键key的值对象，会检查键是否过期，如果过期将其删除(惰性删除)，
 * 会更新服务器的键空间命中/未命中信息
 */
robj* lookupKeyRead(redisDb* db, robj* key) {

    robj* val;

    expireIfNeeded(db, key);

    val = lookupKey(db, key);

    if (val == NULL)
        server.stat_keyspace_misses++;
    else
        server.stat_keyspace_hits++;

    return val;
}

/*
 * 为写入操作取出键key的值对象，会检查键是否过期，如果过期将其删除(惰性删除)
 */
robj* lookupKeyWrite(redisDb* db, robj* key) {

    expireIfNeeded(db, key);

    return lookupKey(db, key);
}

/*
 * 调用lookupKeyRead，如果键不存在，向客户端发送reply
 */
robj* lookupKeyReadOrReply(redisClient* c, robj* key, robj* reply) {

    robj* o = lookupKeyRead(c->db, key);

    if (!o) addReply(c, reply);

    return o;
}

/*
 * 调用lookupKeyWrite，如果键不存在，向客户端发送reply
 */
robj* lookupKeyWriteOrReply(redisClient* c, robj* key, robj* reply) {

    robj* o = lookupKeyWrite(c->db, key);

    if (!o) addReply(c, reply);

    return o;
}

/*
 * 为数据库db的键空间添加一个键值对，
 * 如果键存在，程序终止
 */
void dbAdd(redisDb* db, robj* key, robj* val) {

    sds copy = sdsdup(key->ptr);

    // 键空间的键是一个sds，不是一个redis对象
    int retval = dictAdd(db->dict, copy, val);

    assert(retval == REDIS_OK);

    /* if (server.cluster_enabled) slotToKeyAdd(key); */
}

/*
 * 为数据库db中已经存在的键关联一个新值，
 * 如果键不存在，程序终止
 */
void dbOverwrite(redisDb* db, robj* key, robj* val) {

    dictEntry* de = dictFind(db->dict, key->ptr);

    assert(de != NULL);

    dictReplace(db->dict, key->ptr, val);
}

/*
 * 高层次的set函数，调用dbAdd或dbOverwrite完成实际的工作
 */
void setKey(redisDb* db, robj* key, robj* val) {

    // 如果键不存在，调用dbAdd
    if (lookupKeyWrite(db, key) == NULL) {
        dbAdd(db, key, val);
    // 如果键已经存在，调用dbOverwrite
    } else {
        dbOverwrite(db, key, val);
    }

    // 增加值对象的引用计数
    incrRefCount(val);

    // 移除键key的过期时间，变为持久键
    removeExpire(db, key);

    /* signalModifiedKey(db, key); */
}

/*
 * 检查键key是否存在于数据库中
 */
int dbExists(redisDb* db, robj* key) {
    return dictFind(db->dict, key->ptr) != NULL;
}

/*
 * 从数据库中随机返回一个键，并为其创建一个新的redis对象，
 * 返回前会对键进行检查，如果已经过期，则删除它，重新随机
 */
robj* dbRandomKey(redisDb* db) {

    dictEntry* de;

    while (1) {
        sds key;
        robj* keyobj;

        de = dictGetRandomKey(db->dict);

        if (de == NULL) return NULL;

        key = dictGetKey(de);
        keyobj = createStringObject(key, sdslen(key));

        if (dictFind(db->expires, key)) {
            if (expireIfNeeded(db, keyobj)) {
                /* search for another key. This expired. */
                decrRefCount(keyobj);
                continue;
            }
        }

        return keyobj;
    }
}

/*
 * 从数据库中删除给定键key，并删除其过期时间，
 * 在键空间和过期字典中进行删除
 */
int dbDelete(redisDb* db, robj* key) {

    if (dictSize(db->expires) > 0) dictDelete(db->expires, key->ptr);

    if (dictDelete(db->dict, key->ptr) == DICT_OK) {
        /* if (server.cluster_enabled) slotToKeyDel(key); */
        return 1;
    } else {
        return 0;
    }
}

/*
 * 保证对象返回时是准备好可以改变的
 * 如果对象o是共享的(o->refcount != 1)或不是REDIS_ENCODING_RAW编码的，根据原对象的内容创建一个新的非共享对象；
 * 否则，返回原对象o；
 */
robj* dbUnshareStringValue(redisDb* db, robj* key, robj* o) {

    assert(o->type == REDIS_STRING);

    if (o->refcount != 1 || o->encoding != REDIS_ENCODING_RAW) {
        robj* decoded = getDecodedObject(o);
        o = createRawStringObject(decoded->ptr, sdslen(decoded->ptr));
        decrRefCount(decoded);
        dbOverWrite(db, key, o);
    }
    return o;
}

/*
 * 清空服务器的所有数据库
 */
long long emptyDb(void(callback)(void*)) {

    int j;
    long long removed = 0;

    for (j = 0; j < server.dbnum; j++) {

        removed += dictSize(server.db[j].dict);

        dictEmpty(server.db[j].dict, callback);

        dictEmpty(server.db[j].expires, callback);
    }

    /* if (server.cluster_enabled) slotToKeyFlush(); */

    return removed;
}

/*
 * 将客户端的目标数据库切换为id指定的数据库
 */
int selectDb(redisClient* c, int id) {

    if (id < 0 || id >= server.dbnum)
        return REDIS_ERR;

    c->db = &server.db[id];

    return REDIS_OK;
}

/* Type agnostic commands operating on the key space */

/*
 * del
 * 
 * 删除键，删除之前先检查键是否过期，调用expireIfNeeded；
 * 如果未过期，调用dbDelete；
 */
void delCommand(redisClient* c) {

    int deleted = 0;
    int j;

    for (j = 1; j < c->argc; j++) {

        expireIfNeeded(c->db, c->argv[j]);

        if (dbDelete(c->db, c->argv[j])) {

            /* signalModifiedKey(c->db, c->argv[j]); */

            /* notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "del", c->argv[j], c->db->id); */

            server.dirty++;

            deleted++;
        }
    }

    addReplyLongLong(c, deleted);
}

/*
 * exists
 *
 * 回复键是否存在
 */
void existsCommand(redisClient* c) {

    // 先检查键是否过期，如果过期将其删除
    expireIfNeeded(c->db, c->argv[1]);

    if (dbExists(c->db, c->argv[1])) {
        addReply(c, shared.cone);
    } else {
        addReply(c, shared.czero);
    }
}

/*
 * select
 *
 * 切换客户端的目标数据库，调用selectDb完成实际的工作
 */
void selectCommand(redisClient* c) {

    long id;

    // 检查指定的目标数据库id是否合法
    if (getLongFromObjectOrReply(c, c->argv[1], &id, "invalid DB index") != REDIS_OK)
        return;

    /*
     * if (server.cluster_enabled && id != 0) {
     *     addReplyError(c, "SELECT is not allowed in cluster mode");
     *     return;
     * }
     */

    if (selectDb(c, id) == REDIS_ERR) {
        addReplyError(c, "invalid DB index");
    } else {
        addReply(c, shared.ok);
    }
}

/*
 * randomkey
 *
 * 从客户端的目标数据库中随机返回一个键，调用dbRandomKey完成实际的工作
 */
void randomkeyCommand(redisClient* c) {

    robj* key;

    if ((key = dbRandomKey(c->db)) == NULL) {
        addReply(c, shared.nullbulk);
        return;
    }

    addReplyBulk(c, key);
    decrRefCount(key);
}

/*
 * keys
 *
 * 遍历整个数据库，返回名字与模式匹配的键，
 * 遍历过程中会检查键是否过期，如果过期将其删除，不会返回
 */
void keysCommand(redisClient* c) {

    dictIterator* di;
    dictEntry* de;

    sds pattern = c->argv[1]->ptr;

    int plen = sdslen(pattern);
    int allkeys;
    unsigned long numkeys = 0;
    void* replylen = addDeferredMultiBulkLength(c);

    // 遍历整个数据库，返回名字与模式匹配的键
    // 安全迭代器使用场景，遍历过程中需要对过期的键进行删除
    di = dictGetSafeIterator(c->db->dict);
    allkeys = (pattern[0] == '*' && pattern[1] == '\0');
    while ((de = dictNext(di)) != NULL) {
        sds key = dictGetKey(de);
        robj* keyobj;

        if (allkeys || stringmatchlen(pattern, plen, key, sdslen(key), 0)) {

            keyobj = createStringObject(key, sdslen(key));

            // 删除过期键
            if (expireIfNeeded(c->db, keyobj) == 0) {
                addReplyBulk(c, keyobj);
                numkeys++;
            }

            decrRefCount(keyobj);
        }
    }
    dictReleaseIterator(di);

    setDeferredMultiBulkLength(c, replylen, numkeys);
}

/*
 * dictScan的回调函数，在迭代字典元素时对每个元素调用，
 * 收集元素，将键和值添加到指定的列表尾
 */
/* This callback is used by scanGenericCommand in order to collect elements
 * returned by the dictionary iterator into a list. */
void scanCallback(void* privdata, const dictEntry* de) {

    void** pd = (void**)privdata;
    list* keys = pd[0];
    robj* o = pd[1];
    robj* key;
    robj* val = NULL;

    if (o == NULL) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey, sdslen(sdskey));
    } else if (o->type == REDIS_SET) {
        key = dictGetKey(de);
        incrRefCount(key);
    } else if (o->type == REDIS_HASH) {
        key = dictGetKey(de);
        incrRefCount(key);
        val = dictGetVal(de);
        incrRefCount(val);
    } else if (o->type == REDIS_ZSET) {
        key = dictGetKey(de);
        incrRefCount(key);
        val = createStringObjectFromLongDouble(*(double*)dictGetVal(de));
    } else {
        exit(1);
    }

    listAddNodeTail(keys, key);
    if (val) listAddNodeTail(keys, val);
}

/*
 * 从对象o中解析出SCAN游标，将其存在*cursor中
 */
/* Try to parse a SCAN cursor stored at object 'o':
 * if the cursor is valid, store it as unsigned integer into *cursor and
 * returns REDIS_OK. Otherwise return REDIS_ERR and send an error to the
 * client. */
int parseScanCursorOrReply(redisClient* c, robj* o, unsigned long* cursor) {

    char* eptr;

    /* Use strtoul() because we need an *unsigned* long, so
     * getLongLongFromObject() does not cover the whole cursor space. */
    errno = 0;
    *cursor = strtoul(o->ptr, &eptr, 10);
    if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' || errno == ERANGE) {
        addReplyError(c, "invalid cursor");
        return REDIS_ERR;
    }
    return REDIS_OK;
}

/*
 * SCAN，HSCAN，SSCAN命令的底层实现
 */
/*
 * This command implements SCAN, HSCAN and SSCAN commands.
 */
void scanGenericCommand(redisClient* c, robj* o, unsigned long cursor) {

    int rv;
    int i, j;
    char buf[REDIS_LONGSTR_SIZE];
    list* keys = listCreate();
    listNode* node;
    listNode* nextnode;
    long count = 10;
    sds pat;
    int patlen;
    int use_pattern = 0;
    dict* ht;

    /* Object must be NULL (to iterate keys names), or the type of the object
     * must be Set, Sorted Set, or Hash. */
    assert(o == NULL || o->type == REDIS_SET || o->type == REDIS_HASH || o->type == REDIS_ZSET);

    /* Set i to the first option argument. The previous one is the cursor. */
    i = (o == NULL) ? 2 : 3;    /* Skip the key argument if needed. */

    /* Step1: Parse options. */
    while (i < c->argc) {

        j = c->argc - i;

        // COUNT <number>
        if (!strcasecmp(c->argv[i]->ptr, "count") && j >= 2) {
            if (getLongFromObjectOrReply(c, c->argv[i + 1], &count, NULL) != REDIS_OK) {
                goto cleanup;
            }

            if (count < 1) {
                addReply(c, shared.syntaxerr);
                goto cleanup;
            }

            i += 2;

        // MATCH <pattern>
        } else if (!strcasecmp(c->argv[i]->ptr, "match") && j >= 2) {

            pat = c->argv[i + 1]->ptr;
            patlen = sdslen(pat);

            /* The pattern always matches if it is exactly "*", so it is
             * equivalent to disabling it. */
            use_pattern = !(pat[0] == '*' && patlen == 1);

            i += 2;

        // error
        } else {
            addReply(c, shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Step2: Iterate the collection. */

    /* Handle the case of a hash table. */
    ht = NULL;
    if (o == NULL) {
        ht = c->db->dict;
    } else if (o->type == REDIS_SET && o->encoding == REDIS_ENCODING_HT) {
        ht = o->ptr;
    } else if (o->type == REDIS_HASH && o->encoding == REDIS_ENCODING_HT) {
        ht = o->ptr;
        count *= 2;    /* We return key / value for this type. */
    } else if (o->type == REDIS_ZSET && o->encoding == REDIS_ENCODING_SKIPLIST) {
        zset* zs = o->ptr;
        ht = zs->dict;
        count *= 2;    /* We return key / value for this type. */
    }

    if (ht) {
        void* privdata[2];

        privdata[0] = keys;
        privdata[1] = o;
        do {
            cursor = dictScan(ht, cursor, scanCallback, privdata);
        } while (cursor && listLength(keys) < count);
    } else if (o->type == REDIS_SET) {
        int pos = 0;
        int64_t ll;

        while (intsetGet(o->ptr, pos++, &ll))
            listAddNodeTail(keys, createStringObjectFromLongLong(ll));
        cursor = 0;
    } else if (o->type == REDIS_HASH || o->type == REDIS_ZSET) {
        unsigned char* p = ziplistIndex(o->ptr, 0);
        unsigned char* vstr;
        unsigned int vlen;
        long long vll;

        while (p) {
            ziplistGet(p, &vstr, &vlen, &vll);
            listAddNodeTail(keys, (vstr != NULL) ? createStringObject((char*)vstr, vlen) : createStringObjectFromLongLong(vll));
            p = ziplistNext(o->ptr, p);
        }
        cursor = 0;
    } else {
        exit(1);
    }

    /* Step3: Filter elements. */
    node = listFirst(keys);
    while (node) {

        robj* kobj = listNodeValue(node);
        nextnode = listNextNode(node);
        int filter = 0;

        /* Filter element if it does not match the pattern. */
        if (!filter && use_pattern) {
            if (sdsEncodedObject(kobj)) {
                if (!stringmatchlen(pat, patlen, kobj->ptr, sdslen(kobj->ptr), 0))
                    filter = 1;
            } else {
                char buf[REDIS_LONGSTR_SIZE];
                int len;

                assert(kobj->encoding == REDIS_ENCODING_INT);
                len = ll2string(buf, sizeof(buf), (long)kobj->ptr);
                if (!stringmatchlen(pat, patlen, buf, len, 0)) filter = 1;
            }
        }

        /* Filter element if it is an expired key. */
        if (!filter && o == NULL && expireIfNeeded(c->db, kobj)) filter = 1;

        /* Remove the element and its associated value if needed. */
        if (filter) {
            decrRefCount(kobj);
            listDelNode(keys, node);
        }

        /* If this is a hash or a sorted set, we have a flat list of
         * key-value elements, so if this element was filtered, remove the
         * value, or skip it if it was not filtered: we only match keys. */
        if (o && (o->type == REDIS_ZSET || o->type == REDIS_HASH)) {
            node = nextnode;
            nextnode = listNextNode(node);
            if (filter) {
                kobj = listNodeValue(node);
                decrRefCount(kobj);
                listDelNode(keys, node);
            }
        }
        node = nextnode;
    }

    /* Step 4: Reply to the client. */
    addReplyMultiBulkLen(c, 2);
    rv = snprintf(buf, sizeof(buf), "%lu", cursor);
    assert(rv < sizeof(buf));
    addReplyBulkCBuffer(c, buf, rv);

    addReplyMultiBulkLen(c, listLength(keys));
    while ((node = listFirst(keys)) != NULL) {
        robj* kobj = listNodeValue(node);
        addReplyBulk(c, kobj);
        decrRefCount(kobj);
        listDelNode(keys, node);
    }

cleanup:
    listSetFreeMethod(keys, decrRefCountVoid);
    listRelease(keys);
}

/*
 * scan
 *
 * 用于增量迭代当前数据库中的数据库键，需要带cursor调用，
 * 由于每次只返回少量元素，所以不会像keys或smembers命令一样阻塞服务器(可能数秒)
 * 
 * 注意: 可能会返回重复元素，这个问题由应用程序解决
 */
/* The SCAN command completely relies on scanGenericCommand. */
void scanCommand(redisClient* c) {
    unsigned long cursor;
    if (parseScanCursorOrReply(c, c->argv[1], &cursor) == REDIS_ERR) return;
    scanGenericCommand(c, NULL, cursor);
}

/*
 * dbsize
 *
 * 返回当前数据库中key的数量
 */
void dbsizeCommand(redisClient* c) {
    addReplyLongLong(c, dictSize(c->db->dict));
}

/*
 * lastsave
 *
 * 以UNIX时间戳格式返回最近一次Redis成功将数据保存到磁盘上的时间
 */
void lastsaveCommand(redisClient* c) {
    addReplyLongLong(c, server.lastsave);
}

/*
 * type
 *
 * 以字符串的形式返回存储在key中的值的类型
 */
void typeCommand(redisClient* c) {
    robj* o;
    char* type;

    o = lookupKeyRead(c->db, c->argv[1]);

    if (o == NULL) {
        type = "none";
    } else {
        switch (o->type) {
            case REDIS_STRING:
                type = "string";
                break;
            case REDIS_LIST:
                type = "list";
                break;
            case REDIS_SET:
                type = "set";
                break;
            case REDIS_ZSET:
                type = "zset";
                break;
            case REDIS_HASH:
                type = "hash";
                break;
            default:
                type = "unknown";
                break;
        }
    }

    addReplyStatus(c, type);
}

/*
 * shutdown
 *
 * 如果配置了持久化策略，那么这个命令能够保证在关闭redis服务进程的时候数据不会丢失
 */
void shutdownCommand(redisClient* c) {
    int flags = 0;

    if (c->argc > 2) {
        addReply(c, shared.syntaxerr);
        return;
    } else if (c->argc == 2) {

        if (!strcasecmp(c->argv[1]->ptr, "nosave")) {
            flags |= REDIS_SHUTDOWN_NOSAVE;
        } else if (!strcasecmp(c->argv[1]->ptr, "save")) {
            flags |= REDIS_SHUTDOWN_SAVE;
        } else {
            addReply(c, shared.syntaxerr);
            return;
        }
    }

    /* When SHUTDOWN is called while the server is loading a dataset in
     * memory we need to make sure no attempt is performed to save
     * the dataset on shutdown (otherwise it could overwrite the current DB
     * with half-read data).
     *
     * Also when in Sentinel mode clear the SAVE flag and force NOSAVE. */
    if (server.loading || server.sentinel_mode)
        flags = (flags & ~REDIS_SHUTDOWN_SAVE) | REDIS_SHUTDOWN_NOSAVE;

    if (prepareForShutdown(flags) == REDIS_OK) exit(0);

    addReplyError(c, "Errors trying to SHUTDOWN. Check logs.");
}

/*
 * rename，renamenx命令的底层实现
 */
void renameGenericCommand(redisClient* c, int nx) {
    robj* o;
    long long expire;

    /* To use the same key as src and dst is probably an error */
    if (sdscmp(c->argv[1]->ptr, c->argv[2]->ptr) == 0) {
        addReply(c, shared.sameobjecterr);
        return;
    }

    // 检查来源键是否存在
    if ((o = lookupKeyWriteOrReply(c, c->argv[1], shared.nokeyerr)) == NULL)
        return;

    incrRefCount(o);

    expire = getExpire(c->db, c->argv[1]);

    // 检查目标键是否存在
    if (lookupKeyWrite(c->db, c->argv[2]) != NULL) {

        // RENAMENX
        if (nx) {
            decrRefCount(o);
            addReply(c, shared.czero);
            return;
        }

        // RENAME
        /* Overwrite: delete the old key before creating the new one
         * with the same name. */
        dbDelete(c->db, c->argv[2]);
    }

    dbAdd(c->db, c->argv[2], o);

    if (expire != -1) setExpire(c->db, c->argv[2], expire);

    dbDelete(c->db, c->argv[1]);

    /* signalModifiedKey(c->db, c->argv[1]); */
    /* signalModifiedKey(c->db, c->argv[2]); */

    /* notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "rename_from", c->argv[1], c->db->id); */
    /* notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "rename_to", c->argv[2], c->db->id); */

    server.dirty++;

    addReply(c, nx ? shared.cone : shared.ok);
}

/*
 * rename
 *
 * 修改key的名字为newkey，如果newkey已经存在，则会执行删除操作(隐式)
 */
void renameCommand(redisClient* c) {
    renameGenericCommand(c, 0);
}

/*
 * renamenx
 *
 * 修改key的名字为newkey，仅在newkey不存在时进行
 */
void renamenxCommand(redisClient* c) {
    renameGenericCommand(c, 1);
}

/*
 * move
 *
 * 将当前数据库的key移动到指定的数据库当中
 */
void moveCommand(redisClient* c) {
    robj* o;
    redisDb* src;
    redisDb* dst;
    int srcid;

    /*
     * if (server.cluster_enabled) {
     *     addReplyError(c, "MOVE is not allowed int cluster mode");
     *     return;
     * }
     */

    src = c->db;

    srcid = c->db->id;

    if (selectDb(c, atoi(c->argv[2]->ptr)) == REDIS_ERR) {
        addReply(c, shared.outofrangeerr);
        return;
    }

    dst = c->db;

    selectDb(c, srcid);

    if (src == dst) {
        addReply(c, shared.sameobjecterr);
        return;
    }

    o = lookupKeyWrite(c->db, c->argv[1]);
    if (!o) {
        addReply(c, shared.czero);
        return;
    }

    if (lookupKeyWrite(dst, c->argv[1]) != NULL) {
        addReply(c, shared.czero);
        return;
    }

    dbAdd(dst, c->argv[1], o);
    incrRefCount(o);

    dbDelete(src, c->argv[1]);

    server.dirty++;

    addReply(c, shared.cone);
}

/*
 * Expires API
 */

/*
 * 移除键key的过期时间，键必须在键空间存在，
 * 如果键key设置了过期时间，则从过期字典中将键key删除
 */
int removeExpire(redisDb* db, robj* key) {

    assert(dictFind(db->dict, key->ptr) != NULL);

    return dictDelete(db->expires, key->ptr) == DICT_OK;
}

/*
 * 将键key的过期时间设置为when，键必须在键空间存在
 */
void setExpire(redisDb* db, robj* key, long long when) {

    dictEntry* kde;
    dictEntry* de;

    // 过期字典复用键空间字典的键(sds)
    /* Reuse the sds from the main dict in the expire dict */
    kde = dictFind(db->dict, key->ptr);

    assert(kde != NULL);

    de = dictReplaceRaw(db->expires, dictGetKey(kde));

    // 设置键的过期时间
    dictSetSignedIntegerVal(de, when);
}

/*
 * 返回给定key的过期时间，键必须在键空间存在
 */
long long getExpire(redisDb* db, robj* key) {

    dictEntry* de;

    if (dictSize(db->expires) == 0 || (de = dictFind(db->expires, key->ptr)) == NULL) return -1;

    assert(dictFind(db->dict, key->ptr) != NULL);

    // 取键的过期时间
    return dictGetSignedIntegerVal(de);
}

/*
 * 当一个键过期时，向AOF文件传播一个显式的del命令
 */
void propagateExpire(redisDb* db, robj* key) {
    robj* argv[2];

    argv[0] = shared.del;
    argv[1] = key;
    incrRefCount(argv[0]);
    incrRefCount(argv[1]);

    // propagate to AOF
    if (server.aof_state != REDIS_AOF_OFF)
        feedAppendOnlyFile(server.delCommand, db->id, argv, 2);

    /* replicationFeedSlaves(server.slaves, db->id, argv, 2); */

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

/*
 * 检查键key是否已经过期，如果过期，将它从数据库中移除
 */
int expireIfNeeded(redisDb* db, robj* key) {

    mstime_t when = getExpire(db, key);
    mstime_t now;

    // 没有过期时间
    if (when < 0) return 0;

    // 服务器正在载入，不做任何过期检查
    if (server.loading) return 0;

    now = /* server.lua_caller ? server.lua_time_start : */ mstime();

    /* if (server.masterhost != NULL) return now > when; */

    // 有过期时间，但未到期
    if (now <= when) return 0;

    // 需要删除，更新统计信息
    server.stat_expiredkeys++;

    // 把删除信息传播到AOF
    propagateExpire(db, key);

    /* notifyKeyspaceEvent(REDIS_NOTIFY_EXPIRED, "expired", key, db->id); */

    return dbDelete(db, key);
}

/*
 * Expires Commands
 */

/*
 * EXPIRE，PEXPIRE，EXPIREAT，PEXPIREAT命令的底层实现
 */
/* This is the generic command implementation for EXPIRE, PEXPIRE, EXPIREAT
 * and PEXPIREAT. Because the commad second argument may be relative or absolute
 * the "basetime" argument is used to signal what the base time is (either 0
 * for *AT variants of the command, or the current time for relative expires).
 */
void expireGenericCommand(redisClient* c, long long basetime, int unit) {

    robj* key = c->argv[1];
    robj* param = c->argv[2];
    long long when;

    if (getLongLongFromObjectOrReply(c, param, &when, NULL) != REDIS_OK)
        return;

    if (unit == UNIT_SECONDS) when *= 1000;
    when += basetime;

    if (lookupKeyRead(c->db, key) == NULL) {
        addReply(c, shared.czero);
        return;
    }

    if (when <= mstime() && !server.loading /* && !server.masterhost */) {
        robj* aux;

        assert(dbDelete(c->db, key));
        server.dirty++;

        /* Replicate/AOF this as an explicit DEL. */
        aux = createStringObject("DEL", 3);

        rewriteClientCommandVector(c, 2, aux, key);
        decrRefCount(aux);

        /* signalModifiedKey(c->db, key); */
        /* notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "del", key, c->db->id); */

        addReply(c, shared.cone);

        return;
    } else {

        setExpire(c->db, key, when);

        addReply(c, shared.cone);

        /* signalModifiedKey(c->db, key); */
        /* notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "expire", key, c->db->id); */

        server.dirty++;

        return;
    }
}

/*
 * expire
 *
 * 设置过期时间，参数为时间间隔
 */
void expireCommand(redisClient* c) {
    expireGenericCommand(c, mstime(), UNIT_SECONDS);
}

/*
 * expireat
 *
 * 设置过期时间，参数为时间戳
 */
void expireatCommand(redisClient* c) {
    expireGenericCommand(c, 0, UNIT_SECONDS);
}

void pexpireCommand(redisClient* c) {
    expireGenericCommand(c, mstime(), UNIT_MILLISECONDS);
}

void pexpireatCommand(redisClient* c) {
    expireGenericCommand(c, 0, UNIT_MILLISECONDS);
}

/*
 * TTL，PTTL命令的底层实现
 */
void ttlGenericCommand(redisClient* c, int output_ms) {

    long long expire;
    long long ttl = -1;

    /* If the key does not exist at all, return -2 */
    if (lookupKeyRead(c->db, c->argv[1]) == NULL) {
        addReplyLongLong(c, -2);
        return;
    }

    /* The key exists. Return -1 if it has no expire, or the actual TTL value otherwise. */
    expire = getExpire(c->db, c->argv[1]);

    if (expire != -1) {
        ttl = expire - mstime();
        if (ttl < 0) ttl = 0;
    }

    if (ttl == -1) {
        addReplyLongLong(c, -1);
    } else {
        addReplyLongLong(c, output_ms ? ttl : ((ttl + 500) / 1000));
    }
}

/*
 * ttl
 *
 * 返回键的剩余生存时间(s)
 */
void ttlCommand(redisClient* c) {
    ttlGenericCommand(c, 0);
}

/*
 * pttl
 *
 * 返回键的剩余生存时间(ms)
 */
void pttlCommand(redisClient* c) {
    ttlGenericCommand(c, 1);
}

/*
 * persist
 *
 * 删除给定键的过期时间
 */
void persistCommand(redisClient* c) {

    dictEntry* de;

    de = dictFind(c->db->dict, c->argv[1]->ptr);

    if (de == NULL) {
        addReply(c, shared.czero);
    } else {

        if (removeExpire(c->db, c->argv[1])) {
            addReply(c, shared.cone);
            server.dirty++;
        } else {
            addReply(c, shared.czero);
        }
    }
}

/*
 * API to get key arguments from commands
 */

/* The base case is to use the keys position as given in the command table
 * (firstkey, lastkey, step). */
int* getKeysUsingCommandTable(struct redisCommand* cmd, robj** argv, int argc, int* numkeys) {

    int j;
    int i = 0;
    int last;
    int* keys;
    REDIS_NOTUSED(argv);

    if (cmd->firstkey == 0) {
        *numkeys = 0;
        return NULL;
    }
    last = cmd->lastkey;
    if (last < 0) last = argc + last;
    keys = zmalloc(sizeof(int) * ((last - cmd->firstkey) + 1));
    for (j = cmd->firstkey; j <= last; j += cmd->keystep) {
        assert(j < argc);
        keys[i++] = j;
    }
    *numkeys = i;
    return keys;
}

/* Return all the arguments that are keys in the command passed via argc / argv.
 *
 * The command returns the positions of all the key arguments inside the array,
 * so the actual return value is an heap allocated array of integers. The
 * length of the array is returned by reference into *numkeys.
 *
 * 'cmd' must be point to the corresponding entry into the redisCommand
 * table, according to the command name in argv[0].
 *
 * This function uses the command table if a command-specific helper function
 * is not required, otherwise it calls the command-specific function. */
int* getKeysFromCommand(struct redisCommand* cmd, robj** argv, int argc, int* numkeys) {
    if (cmd->getkeys_proc) {
        return cmd->getkeys_proc(cmd, argv, argc, numkeys);
    } else {
        return getKeysUsingCommandTable(cmd, argv, argc, numkeys);
    }
}

/* Free the result of getKeysFromCommand. */
void getKeysFreeResult(int* result) {
    zfree(result);
}
