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
 * ...
 */
robj* lookupKey(redisDb* db, robj* key) {

    dictEntry* de = dictFind(db->dict, key->ptr);

    if (de) {

        robj* val = dictGetVal(de);

        /* Update the access time for the ageing algorithm.
         * Don't do it if we have a saving child, as this will trigger
         * a copy on write madness. */
        if (server.rdb_child_pid == -1 && server.aof_child_pid == -1)
            val->lru = getLRUClock();

        return val;
    } else {
        return NULL;
    }
}

/*
 * ...
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
 * ...
 */
robj* lookupKeyWrite(redisDb* db, robj* key) {

    expireIfNeeded(db, key);

    return lookupKey(db, key);
}

/*
 * ...
 */
robj* lookupKeyReadOrReply(redisClient* c, robj* key, robj* reply) {

    robj* o = lookupKeyRead(c->db, key);

    if (!o) addReply(c, reply);

    return o;
}

/*
 * ...
 */
robj* lookupKeyWriteOrReply(redisClient* c, robj* key, robj* reply) {

    robj* o = lookupKeyWrite(c->db, key);

    if (!o) addReply(c, reply);

    return o;
}

/*
 * ...
 */
void dbAdd(redisDb* db, robj* key, robj* val) {

    sds copy = sdsdup(key->ptr);

    // key is sds
    int retval = dictAdd(db->dict, copy, val);

    assert(retval == REDIS_OK);

    /* if (server.cluster_enabled) slotToKeyAdd(key); */
}

/*
 * ...
 */
void dbOverwrite(redisDb* db, robj* key, robj* val) {

    dictEntry* de = dictFind(db->dict, key->ptr);

    assert(de != NULL);

    dictReplace(db->dict, key->ptr, val);
}

/*
 * ...
 */
void setKey(redisDb* db, robj* key, robj* val) {

    if (lookupKeyWrite(db, key) == NULL) {
        dbAdd(db, key, val);
    } else {
        dbOverwrite(db, key, val);
    }

    incrRefCount(val);

    removeExpire(db, key);

    /* signalModifiedKey(db, key); */
}

/*
 * ...
 */
int dbExists(redisDb* db, robj* key) {
    return dictFind(db->dict, key->ptr) != NULL;
}

/*
 * ...
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
 * ...
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
 * ...
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
 * ...
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
 * ...
 */
int selectDb(redisClient* c, int id) {

    if (id < 0 || id >= server.dbnum)
        return REDIS_ERR;

    c->db = &server.db[id];

    return REDIS_OK;
}

/* Type agnostic commands operating on the key space */

/*
 * ...
 */
//void flushdbCommand(redisClient* c) {
//
//    server.dirty += dictSize(c->db->dict);
//
//    /* signalFlushedDb(c->db->id); */
//
//    dictEmpty(c->db->dict, NULL);
//    dictEmpty(c->db->expires, NULL);
//
//    /* if (server.cluster_enabled) slotToKeyFlush(); */
//
//    addReply(c, shared.ok);
//}

/*
 * ...
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
 * ...
 */
void existsCommand(redisClient* c) {

    expireIfNeeded(c->db, c->argv[1]);

    if (dbExists(c->db, c->argv[1])) {
        addReply(c, shared.cone);
    } else {
        addReply(c, shared.czero);
    }
}

/*
 * ...
 */
void selectCommand(redisClient* c) {

    long id;

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
 * ...
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
 * ...
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

/* The SCAN command completely relies on scanGenericCommand. */
void scanCommand(redisClient* c) {
    unsigned long cursor;
    if (parseScanCursorOrReply(c, c->argv[1], &cursor) == REDIS_ERR) return;
    scanGenericCommand(c, NULL, cursor);
}

void dbsizeCommand(redisClient* c) {
    addReplyLongLong(c, dictSize(c->db->dict));
}

void lastsaveCommand(redisClient* c) {
    addReplyLongLong(c, server.lastsave);
}

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
 * ...
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
 * ...
 */
void renameGenericCommand(redisClient* c, int nx) {
    robj* o;
    long long expire;

    /* To use the same key as src and dst is probably an error */
    if (sdscmp(c->argv[1]->ptr, c->argv[2]->ptr) == 0) {
        addReply(c, shared.sameobjecterr);
        return;
    }

    if ((o = lookupKeyWriteOrReply(c, c->argv[1], shared.nokeyerr)) == NULL)
        return;

    // ...
    incrRefCount(o);

    expire = getExpire(c->db, c->argv[1]);

    // ...
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
 * ...
 */
void renameCommand(redisClient* c) {
    renameGenericCommand(c, 0);
}

/*
 * ...
 */
void renamenxCommand(redisClient* c) {
    renameGenericCommand(c, 1);
}

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
 * ...
 */
int removeExpire(redisDb* db, robj* key) {

    assert(dictFind(db->dict, key->ptr) != NULL);

    return dictDelete(db->expires, key->ptr) == DICT_OK;
}

/*
 * ...
 */
void setExpire(redisDb* db, robj* key, long long when) {

    dictEntry* kde;
    dictEntry* de;

    /* Reuse the sds from the main dict in the expire dict */
    kde = dictFind(db->dict, key->ptr);

    assert(kde != NULL);

    de = dictReplaceRaw(db->expires, dictGetKey(kde));

    dictSetSignedIntegerVal(de, when);
}

/*
 * ...
 */
long long getExpire(redisDb* db, robj* key) {

    dictEntry* de;

    if (dictSize(db->expires) == 0 || (de = dictFind(db->expires, key->ptr)) == NULL) return -1;

    assert(dictFind(db->dict, key->ptr) != NULL);

    return dictGetSignedIntegerVal(de);
}

/*
 * ...
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
 * ...
 */
int expireIfNeeded(redisDb* db, robj* key) {

    mstime_t when = getExpire(db, key);
    mstime_t now;

    if (when < 0) return 0;

    if (server.loading) return 0;

    now = /* server.lua_caller ? server.lua_time_start : */ mstime();

    /* if (server.masterhost != NULL) return now > when; */

    if (now <= when) return 0;

    server.stat_expiredkeys++;

    propagateExpire(db, key);

    /* notifyKeyspaceEvent(REDIS_NOTIFY_EXPIRED, "expired", key, db->id); */

    return dbDelete(db, key);
}

/*
 * Expires Commands
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

void expireCommand(redisClient* c) {
    expireGenericCommand(c, mstime(), UNIT_SECONDS);
}

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
 * ...
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

void ttlCommand(redisClient* c) {
    ttlGenericCommand(c, 0);
}

void pttlCommand(redisClient* c) {
    ttlGenericCommand(c, 1);
}

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