//
// Created by zouyi on 2021/9/15.
//

#include <math.h>
#include "redis.h"

/*
 * 注意: 
 * signalModifiedKey函数与独立功能事务相关，在本代码中删除；
 * notifyKeyspaceEvent函数与独立功能发布/订阅相关，在本代码中删除；
 */

/*
 * String Commands
 */

/*
 * 检查给定字符串长度len是否超过限制值512MB
 */
static int checkStringLength(redisClient* c, long long size) {

    if (size > 512 * 1024 * 1024) {
        addReplyError(c, "string exceeds maximum allowed size (512MB)");
        return REDIS_ERR;
    }

    return REDIS_OK;
}

/*
 * SET，SETEX，PSETEX，SETNX命令的底层实现
 */

/* The setGenericCommand() function implements the SET operation with different
 * options and variants. This function is called in order to implement the
 * following commands: SET, SETEX, PSETEX, SETNX.
 */

#define REDIS_SET_NO_FLAGS 0
#define REDIS_SET_NX (1<<0)    /* Set if key not exists. */
#define REDIS_SET_XX (1<<1)    /* Set if key exists. */

void setGenericCommand(redisClient* c, int flags, robj* key, robj* val, robj* expire, int unit, robj* ok_reply, robj* abort_reply) {

    long long milliseconds = 0;    /* initialized to avoid any harmness warning */

    if (expire) {

        if (getLongLongFromObjectOrReply(c, expire, &milliseconds, NULL) != REDIS_OK)
            return;

        if (milliseconds <= 0) {
            addReplyError(c, "invalid expire time in SETEX");
            return;
        }

        if (unit == UNIT_SECONDS) milliseconds *= 1000;
    }

    if ((flags & REDIS_SET_NX && lookupKeyWrite(c->db, key) != NULL) ||
        (flags & REDIS_SET_XX && lookupKeyWrite(c->db, key) == NULL)) {

        addReply(c, abort_reply ? abort_reply : shared.nullbulk);
        return;
    }

    setKey(c->db, key, val);

    server.dirty++;

    if (expire) setExpire(c->db, key, mstime() + milliseconds);

    addReply(c, ok_reply ? ok_reply : shared.ok);
}
/*
 * SET命令
 *
 * 为给定键设置字符串值，可指定过期时间
 */
/* SET key value [NX] [XX] [EX <seconds>] [PX <milliseconds>] */
void setCommand(redisClient* c) {

    int j;
    robj* expire = NULL;
    int unit = UNIT_SECONDS;
    int flags = REDIS_SET_NO_FLAGS;

    for (j = 3; j < c->argc; j++) {

        char* a = c->argv[j]->ptr;
        robj* next = (j == c->argc - 1) ? NULL : c->argv[j + 1];

        if ((a[0] == 'n' || a[0] == 'N') && (a[1] == 'x' || a[1] == 'X') && a[2] == '\0') {
            flags |= REDIS_SET_NX;
        } else if ((a[0] == 'x' || a[0] == 'X') && (a[1] == 'x' || a[1] == 'X') && a[2] == '\0') {
            flags |= REDIS_SET_XX;
        } else if ((a[0] == 'e' || a[0] == 'E') && (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' && next) {
            unit = UNIT_SECONDS;
            expire = next;
            j++;
        } else if ((a[0] == 'p' || a[0] == 'P') && (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' && next) {
            unit = UNIT_MILLISECONDS;
            expire = next;
            j++;
        } else {
            addReply(c, shared.syntaxerr);
            return;
        }
    }

    c->argv[2] = tryObjectEncoding(c->argv[2]);

    setGenericCommand(c, flags, c->argv[1], c->argv[2], expire, unit, NULL, NULL);
}

/*
 * SETNX命令
 */
void setnxCommand(redisClient* c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c, REDIS_SET_NX, c->argv[1], c->argv[2], NULL, UNIT_SECONDS, shared.cone, shared.czero);
}

/*
 * SETEX命令
 */
void setexCommand(redisClient* c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c, REDIS_SET_NO_FLAGS, c->argv[1], c->argv[3], c->argv[2], UNIT_SECONDS, NULL, NULL);
}

/*
 * PSETEX命令
 */
void psetexCommand(redisClient* c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c, REDIS_SET_NO_FLAGS, c->argv[1], c->argv[3], c->argv[2], UNIT_MILLISECONDS, NULL, NULL);
}

/*
 * GET命令的底层实现
 */
int getGenericCommand(redisClient* c) {

    robj* o;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.nullbulk)) == NULL)
        return REDIS_OK;

    if (o->type != REDIS_STRING) {
        addReply(c, shared.wrongtypeerr);
        return REDIS_ERR;
    } else {
        addReplyBulk(c, o);
        return REDIS_OK;
    }
}

/*
 * GET命令
 *
 * 获取指定键的字符串类型值
 */
void getCommand(redisClient* c) {
    getGenericCommand(c);
}

/*
 * INCR，DECR，INCRBY，DECRBY命令的底层实现
 */
void incrDecrCommand(redisClient* c, long long incr) {

    long long value;
    long long oldvalue;
    robj* o;
    robj* new;

    o = lookupKeyWrite(c->db, c->argv[1]);

    if (o != NULL && checkType(c, o, REDIS_STRING)) return;

    if (getLongLongFromObjectOrReply(c, o, &value, NULL) != REDIS_OK) return;

    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN - oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX - oldvalue))) {
        addReplyError(c, "increment or decrement would overflow");
        return;
    }

    value += incr;
    new = createStringObjectFromLongLong(value);
    if (o)
        dbOverWrite(c->db, c->argv[1], new);
    else
        dbAdd(c->db, c->argv[1], new);

    server.dirty++;

    addReply(c, shared.colon);
    addReply(c, new);
    addReply(c, shared.crlf);
}

/*
 * INCR命令
 */
void incrCommand(redisClient* c) {
    incrDecrCommand(c, 1);
}

/*
 * DECR命令
 */
void decrCommand(redisClient* c) {
    incrDecrCommand(c, -1);
}

/*
 * INCRBY命令
 */
void incrbyCommand(redisClient* c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != REDIS_OK) return;
    incrDecrCommand(c, incr);
}

/*
 * DECRBY命令
 */
void decrbyCommand(redisClient* c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != REDIS_OK) return;
    incrDecrCommand(c, -incr);
}

/*
 * INCRBYFLOAT命令
 */
void incrbyfloatCommand(redisClient* c) {
    long double incr;
    long double value;
    robj* o;
    robj* new;
    robj* aux;

    o = lookupKeyWrite(c->db, c->argv[1]);

    if (o != NULL && checkType(c, o, REDIS_STRING)) return;

    if (getLongDoubleFromObjectOrReply(c, o, &value, NULL) != REDIS_OK ||
        getLongDoubleFromObjectOrReply(c, c->argv[2], &incr, NULL) != REDIS_OK)
        return;

    value += incr;
    if (isnan(value) || isinf(value)) {
        addReplyError(c, "increment would product NaN or Infinity");
        return;
    }

    new = createStringObjectFromLongDouble(value);
    if (o)
        dbOverWrite(c->db, c->argv[1], new);
    else
        dbAdd(c->db, c->argv[1], new);


    server.dirty++;

    addReplyBulk(c, new);

    // 将INCRBYFLOAT重写为等价的SET命令
    /* Always replicate INCRBYFLOAT as a SET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
    aux = createStringObject("SET", 3);
    rewriteClientCommandArgument(c, 0, aux);
    decrRefCount(aux);
    rewriteClientCommandArgument(c, 2, new);
}

/*
 * APPEND命令
 *
 * 空间预分配策略
 */
void appendCommand(redisClient* c) {

    size_t totlen;
    robj* o;
    robj* append;

    o = lookupKeyWrite(c->db, c->argv[1]);

    if (o == NULL) {

        c->argv[2] = tryObjectEncoding(c->argv[2]);
        dbAdd(c->db, c->argv[1], c->argv[2]);
        incrRefCount(c->argv[2]);
        totlen = stringObjectLen(c->argv[2]);
    } else {

        if (checkType(c, o, REDIS_STRING))
            return;

        append = c->argv[2];
        totlen = stringObjectLen(o) + sdslen(append->ptr);
        if (checkStringLength(c, totlen) != REDIS_OK)
            return;

        o = dbUnshareStringValue(c->db, c->argv[1], o);
        o->ptr = sdscatlen(o->ptr, append->ptr, sdslen(append->ptr));
        totlen = sdslen(o->ptr);
    }

    server.dirty++;

    addReplyLongLong(c, totlen);
}
