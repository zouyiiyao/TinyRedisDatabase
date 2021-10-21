//
// Created by zouyi on 2021/8/31.
//

#include <time.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <locale.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include "redis.h"
#include "bio.h"

/* -----------------------------------------------------------------------------
 * 全局变量定义
 * -------------------------------------------------------------------------- */

/*
 * 全局的共享对象
 */
/* Our shared "common" objects */
struct sharedObjectsStruct shared;

/*
 * 全局的常量
 */
/* Global vars that are actually used as constants. The following double
 * values are used for double on-disk serialization, and are initialized
 * at runtime to avoid strange compiler optimizations. */
double R_Zero, R_PosInf, R_NegInf, R_Nan;

/*
 * 全局的redis服务器对象
 */
/* Global vars */
struct redisServer server;

/*
 * 全局的命令表
 */
/* Our command table.
 *
 * 命令表
 *
 * Every entry is composed of the following fields:
 *
 * 表中的每个项都由以下域组成：
 *
 * name: a string representing the command name.
 *       命令的名字
 *
 * function: pointer to the C function implementing the command.
 *           一个指向命令的实现函数的指针
 *
 * arity: number of arguments, it is possible to use -N to say >= N
 *        参数的数量。可以用 -N 表示 >= N
 *
 * sflags: command flags as string. See below for a table of flags.
 *         字符串形式的 FLAG ，用来计算以下的真实 FLAG
 *
 * flags: flags as bitmask. Computed by Redis using the 'sflags' field.
 *        位掩码形式的 FLAG ，根据 sflags 的字符串计算得出
 *
 * get_keys_proc: an optional function to get key arguments from a command.
 *                This is only used when the following three fields are not
 *                enough to specify what arguments are keys.
 *                一个可选的函数，用于从命令中取出 key 参数，仅在以下三个参数都不足以表示 key 参数时使用
 *
 * first_key_index: first argument that is a key
 *                  第一个 key 参数的位置
 *
 * last_key_index: last argument that is a key
 *                 最后一个 key 参数的位置
 *
 * key_step: step to get all the keys from first to last argument. For instance
 *           in MSET the step is two since arguments are key,val,key,val,...
 *           从 first 参数和 last 参数之间，所有 key 的步数（step）
 *           比如说， MSET 命令的格式为 MSET key value [key value ...]
 *           它的 step 就为 2
 *
 * microseconds: microseconds of total execution time for this command.
 *               执行这个命令耗费的总微秒数
 *
 * calls: total number of calls of this command.
 *        命令被执行的总次数
 *
 * The flags, microseconds and calls fields are computed by Redis and should
 * always be set to zero.
 *
 * microseconds 和 call 由 Redis 计算，总是初始化为 0 。
 *
 * Command flags are expressed using strings where every character represents
 * a flag. Later the populateCommandTable() function will take care of
 * populating the real 'flags' field using this characters.
 *
 * 命令的 FLAG 首先由 SFLAG 域设置，之后 populateCommandTable() 函数从 sflags 属性中计算出真正的 FLAG 到 flags 属性中。
 *
 * This is the meaning of the flags:
 *
 * 以下是各个 FLAG 的意义：
 *
 * w: write command (may modify the key space).
 *    写入命令，可能会修改 key space
 *
 * r: read command  (will never modify the key space).
 *    读命令，不修改 key space
 * m: may increase memory usage once called. Don't allow if out of memory.
 *    可能会占用大量内存的命令，调用时对内存占用进行检查
 *
 * a: admin command, like SAVE or SHUTDOWN.
 *    管理用途的命令，比如 SAVE 和 SHUTDOWN
 *
 * p: Pub/Sub related command.
 *    发布/订阅相关的命令
 *
 * f: force replication of this command, regardless of server.dirty.
 *    无视 server.dirty ，强制复制这个命令。
 *
 * s: command not allowed in scripts.
 *    不允许在脚本中使用的命令
 *
 * R: random command. Command is not deterministic, that is, the same command
 *    with the same arguments, with the same key space, may have different
 *    results. For instance SPOP and RANDOMKEY are two random commands.
 *    随机命令。
 *    命令是非确定性的：对于同样的命令，同样的参数，同样的键，结果可能不同。
 *    比如 SPOP 和 RANDOMKEY 就是这样的例子。
 *
 * S: Sort command output array if called from script, so that the output
 *    is deterministic.
 *    如果命令在 Lua 脚本中执行，那么对输出进行排序，从而得出确定性的输出。
 *
 * l: Allow command while loading the database.
 *    允许在载入数据库时使用的命令。
 *
 * t: Allow command while a slave has stale data but is not allowed to
 *    server this data. Normally no command is accepted in this condition
 *    but just a few.
 *    允许在附属节点带有过期数据时执行的命令。
 *    这类命令很少有，只有几个。
 *
 * M: Do not automatically propagate the command on MONITOR.
 *    不要在 MONITOR 模式下自动广播的命令。
 *
 * k: Perform an implicit ASKING for this command, so the command will be
 *    accepted in cluster mode if the slot is marked as 'importing'.
 *    为这个命令执行一个显式的 ASKING ，
 *    使得在集群模式下，一个被标示为 importing 的槽可以接收这命令。
 */
struct redisCommand redisCommandTable[] = {
    /* Db commands */
    {"del",delCommand,-2,"w",0,NULL,1,-1,1,0,0},
    {"exists",existsCommand,2,"r",0,NULL,1,1,1,0,0},
    {"select",selectCommand,2,"rl",0,NULL,0,0,0,0,0},
    {"randomkey",randomkeyCommand,1,"rR",0,NULL,0,0,0,0,0},
    {"keys",keysCommand,2,"rS",0,NULL,0,0,0,0,0},
    {"scan",scanCommand,-2,"rR",0,NULL,0,0,0,0,0},
    {"dbsize",dbsizeCommand,1,"r",0,NULL,0,0,0,0,0},
    {"lastsave",lastsaveCommand,1,"rR",0,NULL,0,0,0,0,0},
    {"type",typeCommand,2,"r",0,NULL,1,1,1,0,0},
    {"shutdown",shutdownCommand,-1,"arlt",0,NULL,0,0,0,0,0},
    {"move",moveCommand,3,"w",0,NULL,1,1,1,0,0},
    {"rename",renameCommand,3,"w",0,NULL,1,2,1,0,0},
    {"renamenx",renamenxCommand,3,"w",0,NULL,1,2,1,0,0},

    /* String commands */
    {"set", setCommand, -3, "wm", 0, NULL, 1, 1, 1, 0, 0},
    {"setnx",setnxCommand,3,"wm",0,NULL,1,1,1,0,0},
    {"setex",setexCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"psetex",psetexCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"get", getCommand, 2, "r", 0, NULL, 1, 1, 1, 0, 0},
    {"append",appendCommand,3,"wm",0,NULL,1,1,1,0,0},
    {"incr",incrCommand,2,"wm",0,NULL,1,1,1,0,0},
    {"decr",decrCommand,2,"wm",0,NULL,1,1,1,0,0},
    {"incrby",incrbyCommand,3,"wm",0,NULL,1,1,1,0,0},
    {"decrby",decrbyCommand,3,"wm",0,NULL,1,1,1,0,0},
    {"incrbyfloat",incrbyfloatCommand,3,"wm",0,NULL,1,1,1,0,0},

    /* List commands */
    {"rpush",rpushCommand,-3,"wm",0,NULL,1,1,1,0,0},
    {"lpush",lpushCommand,-3,"wm",0,NULL,1,1,1,0,0},
    {"rpushx",rpushxCommand,3,"wm",0,NULL,1,1,1,0,0},
    {"lpushx",lpushxCommand,3,"wm",0,NULL,1,1,1,0,0},
    {"linsert",linsertCommand,5,"wm",0,NULL,1,1,1,0,0},
    {"rpop",rpopCommand,2,"w",0,NULL,1,1,1,0,0},
    {"lpop",lpopCommand,2,"w",0,NULL,1,1,1,0,0},
    {"llen",llenCommand,2,"r",0,NULL,1,1,1,0,0},
    {"lindex",lindexCommand,3,"r",0,NULL,1,1,1,0,0},
    {"lrem",lremCommand,4,"w",0,NULL,1,1,1,0,0},
    {"ltrim",ltrimCommand,4,"w",0,NULL,1,1,1,0,0},
    {"lset",lsetCommand,4,"wm",0,NULL,1,1,1,0,0},

    /* Hash commands */
    {"hset",hsetCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"hsetnx",hsetnxCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"hget",hgetCommand,3,"r",0,NULL,1,1,1,0,0},
    {"hexists",hexistsCommand,3,"r",0,NULL,1,1,1,0,0},
    {"hdel",hdelCommand,-3,"w",0,NULL,1,1,1,0,0},
    {"hlen",hlenCommand,2,"r",0,NULL,1,1,1,0,0},
    {"hgetall",hgetallCommand,2,"r",0,NULL,1,1,1,0,0},

    /* Set commands */
    {"sadd",saddCommand,-3,"wm",0,NULL,1,1,1,0,0},
    {"srem",sremCommand,-3,"w",0,NULL,1,1,1,0,0},
    {"scard",scardCommand,2,"r",0,NULL,1,1,1,0,0},
    {"sismember",sismemberCommand,3,"r",0,NULL,1,1,1,0,0},
    {"sinter",sinterCommand,-2,"rS",0,NULL,1,-1,1,0,0},
    {"sunion",sunionCommand,-2,"rS",0,NULL,1,-1,1,0,0},
    {"sdiff",sdiffCommand,-2,"rS",0,NULL,1,-1,1,0,0},
    {"srandmember",srandmemberCommand,-2,"rR",0,NULL,1,1,1,0,0},
    {"spop",spopCommand,2,"wRs",0,NULL,1,1,1,0,0},

    /* Sorted set commands */
    {"zadd",zaddCommand,-4,"wm",0,NULL,1,1,1,0,0},
    {"zcard",zcardCommand,2,"r",0,NULL,1,1,1,0,0},
    {"zcount",zcountCommand,4,"r",0,NULL,1,1,1,0,0},
    {"zrange",zrangeCommand,-4,"r",0,NULL,1,1,1,0,0},
    {"zrevrange",zrevrangeCommand,-4,"r",0,NULL,1,1,1,0,0},
    {"zrank",zrankCommand,3,"r",0,NULL,1,1,1,0,0},
    {"zrevrank",zrevrankCommand,3,"r",0,NULL,1,1,1,0,0},
    {"zrem",zremCommand,-3,"w",0,NULL,1,1,1,0,0},
    {"zscore",zscoreCommand,3,"r",0,NULL,1,1,1,0,0}
};

/* -----------------------------------------------------------------------------
 * 获取时钟API
 * -------------------------------------------------------------------------- */

/*
 * 返回微秒级别的UNIX时间戳
 */
long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec) * 1000000;
    ust += tv.tv_usec;
    return ust;
}

/*
 * 返回毫秒级别的时间戳
 */
long long mstime(void) {
    return ustime() / 1000;
}

/*
 * 获取秒级时钟
 */
unsigned int getLRUClock(void) {
    return (mstime() / REDIS_LRU_CLOCK_RESOLUTION) & REDIS_LRU_CLOCK_MAX;
}

/*
 * 更新缓存的时钟，避免频繁的time(NULL)调用
 */
/* We take a cached value of the unix time in the global state because with
 * virtual memory and aging there is to store the current time in objects at
 * every object access, and accuracy is not needed. To access a global var is
 * a lot faster than calling time(NULL) */
void updateCachedTime(void) {
    server.unixtime = time(NULL);
    server.mstime = mstime();
}

/* -----------------------------------------------------------------------------
 * redis使用字典数据结构时定义的特有函数
 * -------------------------------------------------------------------------- */

/*
 * 键为sds时使用的哈希函数
 */
unsigned int dictSdsHash(const void* key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

/*
 * 键为sds时使用的哈希函数，大小写不敏感
 */
unsigned int dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char*)key, sdslen((char*)key));
}

/*
 * 比较两个sds
 */
int dictSdsKeyCompare(void* privdata, const void* key1, const void* key2) {
    int l1, l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

/* A case insensitive version used for the command lookup table and other
 * places where case insensitive non binary-safe comparison is needed. */
int dictSdsKeyCaseCompare(void *privdata, const void *key1, const void *key2)
{
    DICT_NOTUSED(privdata);

    return strcasecmp(key1, key2) == 0;
}

/*
 * 键为sds时使用的销毁函数
 */
void dictSdsDestructor(void* privdata, void* val) {
    DICT_NOTUSED(privdata);

    sdsfree(val);
}

/*
 * redis对象作为字典的键/值时使用的销毁函数
 */
void dictRedisObjectDestructor(void* privdata, void* val) {
    DICT_NOTUSED(privdata);

    if (val == NULL) return;
    decrRefCount(val);
}

/*
 * redis对象(只允许字符串类型对象)作为字典的键时使用的比较函数
 */
int dictEncObjKeyCompare(void* privdata, const void* key1, const void* key2) {
    robj* o1 = (robj*)key1;
    robj* o2 = (robj*)key2;
    int cmp;

    if (o1->encoding == REDIS_ENCODING_INT && o2->encoding == REDIS_ENCODING_INT)
        return o1->ptr == o2->ptr;

    o1 = getDecodedObject(o1);
    o2 = getDecodedObject(o2);
    cmp = dictSdsKeyCompare(privdata, o1->ptr, o2->ptr);
    decrRefCount(o1);
    decrRefCount(o2);
    return cmp;
}

/*
 * redis对象(只允许字符串类型对象)作为字典的键时使用的哈希函数
 */
unsigned int dictEncObjHash(const void* key) {
    robj* o = (robj*)key;

    if (sdsEncodedObject(o)) {
        return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
    } else {
        if (o->encoding == REDIS_ENCODING_INT) {
            char buf[32];
            int len;

            len = ll2string(buf, 32, (long)o->ptr);
            return dictGenHashFunction((unsigned char*)buf, len);
        // BUG: 此处源码中为无用分支，redis6.0稳定版还没有修复
        } else {
            exit(1);
        }
    }
}

/*
 * 字典用作集合类型对象的底层实现时，使用的特有函数
 */
dictType setDictType = {
    dictEncObjHash,
    NULL,
    NULL,
    dictEncObjKeyCompare,
    dictRedisObjectDestructor,
    NULL
};

/*
 * 字典用作有序集合类型对象的底层实现时，使用的特有函数
 */
dictType zsetDictType = {
    dictEncObjHash,
    NULL,
    NULL,
    dictEncObjKeyCompare,
    dictRedisObjectDestructor,
    NULL
};

/*
 * 字典用作哈希类型对象的底层实现时，使用的特有函数
 */
dictType hashDictType = {
    dictEncObjHash,
    NULL,
    NULL,
    dictEncObjKeyCompare,
    dictRedisObjectDestructor,
    dictRedisObjectDestructor
};
/*
 * 字典用作键空间的底层实现时，使用的特有函数
 */
/* Db->dict, keys are sds strings, vals are Redis objects. */
dictType dbDictType = {
    dictSdsHash,
    NULL,
    NULL,
    dictSdsKeyCompare,
    dictSdsDestructor,
    dictRedisObjectDestructor
};

/*
 * 字典用作过期字典的底层实现时，使用的特有函数
 */
/* Db->expires */
dictType keyptrDictType = {
    dictSdsHash,
    NULL,
    NULL,
    dictSdsKeyCompare,
    NULL,
    NULL
};

/*
 * 字典用作命令表的底层实现时，使用的特有函数
 */
/* Command table. sds string -> command struct pointer. */
dictType commandTableDictType = {
        dictSdsCaseHash,
        NULL,
        NULL,
        dictSdsKeyCaseCompare,
        dictSdsDestructor,
        NULL
};

/*
 * 判断字典是否需要缩小
 */
int htNeedsResize(dict* dict) {
    long long size, used;

    size = dictSlots(dict);
    used = dictSize(dict);
    return (size && used && size > DICT_HT_INITIAL_SIZE && (used * 100 / size < REDIS_HT_MINFILL));
}

/*
 * 尝试缩小数据库的键空间字典和过期字典的大小来节省内存
 *
 * serverCron -> databasesCron -> tryResizeHashTables
 */
/* If the percentage of used slots in the HT reaches REDIS_HT_MINFILL
 * we resize the hash table to save memory */
void tryResizeHashTables(int dbid) {
    // 缩小键空间字典
    if (htNeedsResize(server.db[dbid].dict))
        dictResize(server.db[dbid].dict);
    // 缩小过期字典
    if (htNeedsResize(server.db[dbid].expires))
        dictResize(server.db[dbid].expires);
}

/*
 * 主动对数据库进行渐进式rehash
 * 
 * serverCron -> databasesCron -> incrementallyRehash
 */
/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use 1 millisecond
 * of CPU time at every call of this function to perform some rehahsing.
 *
 * 虽然服务器在对数据库执行读取/写入命令时会对数据库进行渐进式 rehash ，
 * 但如果服务器长期没有执行命令的话，数据库字典的 rehash 就可能一直没办法完成，
 * 为了防止出现这种情况，我们需要对数据库执行主动 rehash 。
 *
 * The function returns 1 if some rehashing was performed, otherwise 0
 * is returned.
 *
 * 函数在执行了主动 rehash 时返回 1 ，否则返回 0 。
 */
int incrementallyRehash(int dbid) {

    /* Keys dictionary */
    if (dictIsRehashing(server.db[dbid].dict)) {
        dictRehashMilliseconds(server.db[dbid].dict, 1);
        return 1;    /* already used our millisecond for this loop... */
    }

    /* Expires */
    if (dictIsRehashing(server.db[dbid].expires)) {
        dictRehashMilliseconds(server.db[dbid].expires, 1);
        return 1;    /* already used our millisecond for this loop... */
    }

    return 0;
}

/*
 * 如果有子进程，禁止字典resize
 *
 * serverCron -> updateDictResizePolicy
 */
/* This function is called once a background process of some kind terminates,
 * as we want to avoid resizing the hash tables when there is a child in order
 * to play well with copy-on-write (otherwise when a resize happens lots of
 * memory pages are copied). The goal of this function is to update the ability
 * for dict.c to resize the hash tables accordingly to the fact we have o not
 * running childs. */
void updateDictResizePolicy(void) {
    if (server.rdb_child_pid == -1 && server.aof_child_pid == -1)
        dictEnableResize();
    else
        dictDisableResize();
}

/* -----------------------------------------------------------------------------
 * redis命令表API
 * -------------------------------------------------------------------------- */

/*
 * 根据redis.c文件顶部的命令列表，创建命令表
 */
/* Populates the Redis Command Table starting from the hard coded list
 * we have on top of redis.c file.
 */
void populateCommandTable(void) {
    int j;

    // 命令的数量
    int numcommands = sizeof(redisCommandTable)/sizeof(struct redisCommand);

    for (j = 0; j < numcommands; j++) {

        // 指定命令
        struct redisCommand *c = redisCommandTable+j;

        // 取出字符串 FLAG
        char *f = c->sflags;

        int retval1, retval2;

        // 根据字符串 FLAG 生成实际 FLAG
        while(*f != '\0') {
            switch(*f) {
                case 'w':
                    c->flags |= REDIS_CMD_WRITE; break;
                case 'r':
                    c->flags |= REDIS_CMD_READONLY; break;
                case 'm':
                    c->flags |= REDIS_CMD_DENYOOM; break;
                case 'a':
                    c->flags |= REDIS_CMD_ADMIN; break;
                case 'p':
                    c->flags |= REDIS_CMD_PUBSUB; break;
                case 's':
                    c->flags |= REDIS_CMD_NOSCRIPT; break;
                case 'R':
                    c->flags |= REDIS_CMD_RANDOM; break;
                case 'S':
                    c->flags |= REDIS_CMD_SORT_FOR_SCRIPT; break;
                case 'l':
                    c->flags |= REDIS_CMD_LOADING; break;
                case 't':
                    c->flags |= REDIS_CMD_STALE; break;
                case 'M':
                    c->flags |= REDIS_CMD_SKIP_MONITOR; break;
                case 'k':
                    c->flags |= REDIS_CMD_ASKING; break;
                default:
                    exit(1);
            }
            f++;
        }

        // 将命令关联到命令表
        retval1 = dictAdd(server.commands, sdsnew(c->name), c);

        // 将命令也关联到原始命令表，原始命令表不会受 redis.conf 中命令改名的影响
        /* Populate an additional dictionary that will be unaffected
         * by rename-command statements in redis.conf.
         */
        retval2 = dictAdd(server.orig_commands, sdsnew(c->name), c);

        assert(retval1 == DICT_OK && retval2 == DICT_OK);
    }
}

/*
 * 根据给定命令名字（SDS），查找命令
 */
struct redisCommand *lookupCommand(sds name) {
    return dictFetchValue(server.commands, name);
}

/*
 * 根据给定命令名字（C 字符串），查找命令
 */
struct redisCommand *lookupCommandByCString(char *s) {
    struct redisCommand *cmd;
    sds name = sdsnew(s);

    cmd = dictFetchValue(server.commands, name);
    sdsfree(name);
    return cmd;
}

/* Lookup the command in the current table, if not found also check in
 * the original table containing the original command names unaffected by
 * redis.conf rename-command statement.
 *
 * 从当前命令表 server.commands 中查找给定名字，
 * 如果没找到的话，就尝试从 server.orig_commands 中查找未被改名的原始名字
 * 原始表中的命令名不受 redis.conf 中命令改名的影响
 *
 * This is used by functions rewriting the argument vector such as
 * rewriteClientCommandVector() in order to set client->cmd pointer
 * correctly even if the command was renamed.
 *
 * 这个函数可以在命令被更名之后，仍然在重写命令时得出正确的名字。
 */
struct redisCommand *lookupCommandOrOriginal(sds name) {

    // 查找当前表
    struct redisCommand *cmd = dictFetchValue(server.commands, name);

    // 如果有需要的话，查找原始表
    if (!cmd) cmd = dictFetchValue(server.orig_commands,name);

    return cmd;
}

/* -----------------------------------------------------------------------------
 * activeExpireCycle执行主动删除，有多种执行模式:
 * 
 * 1. 在serverCron中被调用，ACTIVE_EXPIRE_CYCLE_SLOW模式
 * 2. 在beforeSleep中被调用，ACTIVE_EXPIRE_CYCLE_FAST模式
 * -------------------------------------------------------------------------- */

/* Helper function for the activeExpireCycle() function.
 * This function will try to expire the key that is stored in the hash table
 * entry 'de' of the 'expires' hash table of a Redis database.
 *
 * activeExpireCycle() 函数使用的检查键是否过期的辅助函数。
 *
 * If the key is found to be expired, it is removed from the database and
 * 1 is returned. Otherwise no operation is performed and 0 is returned.
 *
 * 如果 de 中的键已经过期，那么移除它，并返回 1 ，否则不做动作，并返回 0 。
 *
 * When a key is expired, server.stat_expiredkeys is incremented.
 *
 * The parameter 'now' is the current time in milliseconds as is passed
 * to the function to avoid too many gettimeofday() syscalls.
 *
 * 参数 now 是毫秒格式的当前时间
 */
int activeExpireCycleTryExpire(redisDb *db, dictEntry *de, long long now) {

    // 获取键的过期时间
    long long t = dictGetSignedIntegerVal(de);

    // 键已过期
    if (now > t) {
        sds key = dictGetKey(de);
        robj *keyobj = createStringObject(key,sdslen(key));

        // 传播过期命令
        propagateExpire(db,keyobj);

        // 从数据库中删除该键
        dbDelete(db,keyobj);

        // TODO: 发布/订阅相关 
        // 发送事件
        /* notifyKeyspaceEvent(REDIS_NOTIFY_EXPIRED, "expired",keyobj,db->id); */

        decrRefCount(keyobj);

        // 更新计数器
        server.stat_expiredkeys++;

        return 1;
    } else {

        // 键未过期
        return 0;
    }
}

/* Try to expire a few timed out keys. The algorithm used is adaptive and
 * will use few CPU cycles if there are few expiring keys, otherwise
 * it will get more aggressive to avoid that too much memory is used by
 * keys that can be removed from the keyspace.
 *
 * 函数尝试删除数据库中已经过期的键。
 *
 * 当带有过期时间的键比较少时，函数运行得比较保守；
 * 如果带有过期时间的键比较多，那么函数会以更积极的方式来删除过期键，从而可能地释放被过期键占用的内存。
 *
 * No more than REDIS_DBCRON_DBS_PER_CALL databases are tested at every
 * iteration.
 *
 * 每次循环中被测试的数据库数目不会超过 REDIS_DBCRON_DBS_PER_CALL 。
 *
 * This kind of call is used when Redis detects that timelimit_exit is
 * true, so there is more work to do, and we do it more incrementally from
 * the beforeSleep() function of the event loop.
 *
 * 如果 timelimit_exit 为真，那么说明还有更多删除工作要做，
 * 那么在 beforeSleep() 函数调用时，程序会再次执行这个函数。
 *
 * Expire cycle type:
 *
 * 过期循环的类型：
 *
 * If type is ACTIVE_EXPIRE_CYCLE_FAST the function will try to run a
 * "fast" expire cycle that takes no longer than EXPIRE_FAST_CYCLE_DURATION
 * microseconds, and is not repeated again before the same amount of time.
 *
 * 如果循环的类型为ACTIVE_EXPIRE_CYCLE_FAST，那么函数会以"快速过期"模式执行，
 * 执行的时间不会长过 EXPIRE_FAST_CYCLE_DURATION 毫秒，并且在 EXPIRE_FAST_CYCLE_DURATION 毫秒之内不会再重新执行。
 *
 * If type is ACTIVE_EXPIRE_CYCLE_SLOW, that normal expire cycle is
 * executed, where the time limit is a percentage of the REDIS_HZ period
 * as specified by the REDIS_EXPIRELOOKUPS_TIME_PERC define.
 *
 * 如果循环的类型为ACTIVE_EXPIRE_CYCLE_SLOW，那么函数会以"正常过期"模式执行，
 * 函数的执行时限为 REDIS_HS 常量的一个百分比，这个百分比由 REDIS_EXPIRELOOKUPS_TIME_PERC 定义。
 */

void activeExpireCycle(int type) {

    /* This function has some global state in order to continue the work
     * incrementally across calls. */
    // 静态变量，用来累积函数连续执行时的数据
    // 上次处理到的数据库id
    static unsigned int current_db = 0; /* Last DB tested. */
    // 上次处理到达了时间上限导致返回
    static int timelimit_exit = 0;      /* Time limit hit in previous call? */
    // 上次快速模式开始时间
    static long long last_fast_cycle = 0; /* When last fast cycle ran. */

    unsigned int j, iteration = 0;
    // 默认每次处理的数据库数量，REDIS_DBCRON_DBS_PER_CALL = 16
    unsigned int dbs_per_call = REDIS_DBCRON_DBS_PER_CALL;
    // 函数开始的时间
    long long start = ustime(), timelimit;

    // 快速模式
    if (type == ACTIVE_EXPIRE_CYCLE_FAST) {
        /* Don't start a fast cycle if the previous cycle did not exited
         * for time limt. Also don't repeat a fast cycle for the same period
         * as the fast cycle total duration itself. */
        // 如果上次函数没有触发timelimit_exit，说明要删除的键比较少，那么不执行处理
        if (!timelimit_exit) return;
        // 如果距离上次执行未够一定时间，那么不执行处理，ACTIVE_EXPIRE_CYCLE_FAST_DURATION = 1000us
        if (start < last_fast_cycle + ACTIVE_EXPIRE_CYCLE_FAST_DURATION*2) return;
        // 运行到这里，说明执行快速处理，记录当前时间
        last_fast_cycle = start;
    }

    /* We usually should test REDIS_DBCRON_DBS_PER_CALL per iteration, with
     * two exceptions:
     *
     * 一般情况下，函数只处理 REDIS_DBCRON_DBS_PER_CALL 个数据库，
     * 除非：
     *
     * 1) Don't test more DBs than we have.
     *    当前数据库的数量小于 REDIS_DBCRON_DBS_PER_CALL
     * 2) If last time we hit the time limit, we want to scan all DBs
     * in this iteration, as there is work to do in some DB and we don't want
     * expired keys to use memory for too much time.
     *     如果上次处理遇到了时间上限，那么这次需要对所有数据库进行扫描，
     *     这可以避免过多的过期键占用空间
     */
    if (dbs_per_call > server.dbnum || timelimit_exit)
        dbs_per_call = server.dbnum;

    /* We can use at max ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC percentage of CPU time
     * per iteration. Since this function gets called with a frequency of
     * server.hz times per second, the following is the max amount of
     * microseconds we can spend in this function. */
    // 函数处理的微秒时间上限
    // ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC 默认为 25 ，也即是 25 % 的 CPU 时间
    timelimit = 1000000*ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC/server.hz/100;
    timelimit_exit = 0;
    if (timelimit <= 0) timelimit = 1;

    // 如果是运行在快速模式之下
    // 那么最多只能运行 FAST_DURATION 微秒
    // 默认值为 1000 （微秒）
    if (type == ACTIVE_EXPIRE_CYCLE_FAST)
        timelimit = ACTIVE_EXPIRE_CYCLE_FAST_DURATION; /* in microseconds. */

    // 最多处理dbs_per_call个数据库
    for (j = 0; j < dbs_per_call; j++) {
        int expired;
        // 指向要处理的数据库
        redisDb *db = server.db+(current_db % server.dbnum);

        /* Increment the DB now so we are sure if we run out of time
         * in the current DB we'll restart from the next. This allows to
         * distribute the time evenly across DBs. */
        // 为 DB 计数器加一，如果进入 do 循环之后因为超时而跳出
        // 那么下次会直接从下个 DB 开始处理
        current_db++;

        /* Continue to expire if at the end of the cycle more than 25%
         * of the keys were expired. */
        do {
            unsigned long num, slots;
            long long now, ttl_sum;
            int ttl_samples;

            /* If there is nothing to expire try next DB ASAP. */
            // 获取数据库中带过期时间的键的数量
            // 如果该数量为 0 ，直接跳过这个数据库
            if ((num = dictSize(db->expires)) == 0) {
                db->avg_ttl = 0;
                break;
            }
            // 获取过期字典的桶数量
            slots = dictSlots(db->expires);
            // 当前时间
            now = mstime();

            /* When there are less than 1% filled slots getting random
             * keys is expensive, so stop here waiting for better times...
             * The dictionary will be resized asap. */
            // 这个数据库的过期字典负载因子低于 1% ，扫描起来太费力了（大部分都会 MISS）
            // 跳过，等待字典收缩程序运行
            if (num && slots > DICT_HT_INITIAL_SIZE &&
                (num*100/slots < 1)) break;

            /* The main collection cycle. Sample random keys among keys
             * with an expire set, checking for expired ones.
             *
             * 样本计数器
             */
            // 已处理过期键计数器
            expired = 0;
            // 键的总 TTL 计数器
            ttl_sum = 0;
            // 总共处理的键计数器
            ttl_samples = 0;

            // 内循环每次最多只能检查 LOOKUPS_PER_LOOP 个键，ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP = 20
            if (num > ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP)
                num = ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP;

            // 开始遍历数据库
            while (num--) {
                dictEntry *de;
                long long ttl;

                // 从 expires 中随机取出一个带过期时间的键
                if ((de = dictGetRandomKey(db->expires)) == NULL) break;
                // 计算 TTL
                ttl = dictGetSignedIntegerVal(de)-now;
                // 如果键已经过期，那么删除它，并将 expired 计数器增一
                if (activeExpireCycleTryExpire(db,de,now)) expired++;
                if (ttl < 0) ttl = 0;
                // 累积键的 TTL
                ttl_sum += ttl;
                // 累积处理键的个数
                ttl_samples++;
            }

            /* Update the average TTL stats for this database. */
            // 为这个数据库更新平均 TTL 统计数据
            if (ttl_samples) {
                // 计算当前平均值
                long long avg_ttl = ttl_sum/ttl_samples;

                // 如果这是第一次设置数据库平均 TTL ，那么进行初始化
                if (db->avg_ttl == 0) db->avg_ttl = avg_ttl;
                // 取数据库的上次平均 TTL 和今次平均 TTL 的平均值
                /* Smooth the value averaging with the previous one. */
                db->avg_ttl = (db->avg_ttl+avg_ttl)/2;
            }

            /* We can't block forever here even if there are many keys to
             * expire. So after a given amount of milliseconds return to the
             * caller waiting for the other active expire cycle. */
            // 我们不能用太长时间处理过期键，
            // 所以这个函数执行一定时间之后就要返回

            // 更新遍历次数
            iteration++;

            // 每遍历 16 次执行一次
            if ((iteration & 0xf) == 0 && /* check once every 16 iterations. */
                (ustime()-start) > timelimit)
            {
                // 如果遍历次数正好是 16 的倍数
                // 并且遍历的时间超过了 timelimit
                // 那么断开 timelimit_exit
                timelimit_exit = 1;
            }

            // 已经超时了，返回
            if (timelimit_exit) return;

            /* We don't repeat the cycle if there are less than 25% of keys
             * found expired in the current DB. */
            // 如果已删除的过期键占当前总数据库带过期时间的键数量的 25 %(随机抽取ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP作为样本检查)
            // 那么不再遍历
        } while (expired > ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP/4);
    }
}

/* -----------------------------------------------------------------------------
 * redis时间事件，服务器定时任务(serverCron)相关
 * -------------------------------------------------------------------------- */

// 将服务器的命令执行次数记录到抽样数组中
/* Add a sample to the operations per second array of samples. */
void trackOperationsPerSecond(void) {

    // 计算两次抽样之间的时间长度，毫秒格式
    long long t = mstime() - server.ops_sec_last_sample_time;

    // 计算两次抽样之间，执行了多少个命令
    long long ops = server.stat_numcommands - server.ops_sec_last_sample_ops;

    long long ops_sec;

    // 计算距离上一次抽样之后，每秒执行命令的数量
    ops_sec = t > 0 ? (ops*1000/t) : 0;

    // 将计算出的执行命令数量保存到抽样数组
    server.ops_sec_samples[server.ops_sec_idx] = ops_sec;
    // 更新抽样数组的索引
    server.ops_sec_idx = (server.ops_sec_idx+1) % REDIS_OPS_SEC_SAMPLES;
    // 更新最后一次抽样的时间
    server.ops_sec_last_sample_time = mstime();
    // 更新最后一次抽样时的执行命令数量
    server.ops_sec_last_sample_ops = server.stat_numcommands;
}

// 根据所有取样信息，计算服务器最近一秒执行命令数的平均值
/* Return the mean of all the samples. */
long long getOperationsPerSecond(void) {
    int j;
    long long sum = 0;

    // 计算所有取样值的总和
    for (j = 0; j < REDIS_OPS_SEC_SAMPLES; j++)
        sum += server.ops_sec_samples[j];

    // 计算取样的平均值
    return sum / REDIS_OPS_SEC_SAMPLES;
}

// 检查客户端是否已经超时，如果超时就关闭客户端，并返回 1 ；否则返回 0 。
/* Check for timeouts. Returns non-zero if the client was terminated */
int clientsCronHandleTimeout(redisClient *c) {

    // 获取当前时间
    time_t now = server.unixtime;

    // 服务器设置了 maxidletime 时间
    if (server.maxidletime &&
        // TODO: 复制相关 
        // 不检查作为从服务器的客户端
        /* !(c->flags & REDIS_SLAVE) && */    /* no timeout for slaves */
        // 不检查作为主服务器的客户端
        /* !(c->flags & REDIS_MASTER) && */   /* no timeout for masters */
        // TODO: 阻塞相关
        // 不检查被阻塞的客户端
        /* !(c->flags & REDIS_BLOCKED) && */  /* no timeout for BLPOP */
        // TODO: 发布/订阅相关
        // 不检查订阅了频道的客户端
        /* dictSize(c->pubsub_channels) == 0 && */ /* no timeout for pubsub */
        // 不检查订阅了模式的客户端
        /* listLength(c->pubsub_patterns) == 0 && */
        // 客户端最后一次与服务器通讯的时间已经超过了 maxidletime 时间
        (now - c->lastinteraction > server.maxidletime))
    {
        printf("Closing idle client\n");
        // 关闭超时客户端
        freeClient(c);
        return 1;
    // TODO: 阻塞相关
    } /* else if (c->flags & REDIS_BLOCKED) { */

        /* Blocked OPS timeout is handled with milliseconds resolution.
         * However note that the actual resolution is limited by
         * server.hz. */
        // 获取最新的系统时间
        /* mstime_t now_ms = mstime(); */

        // 检查被 BLPOP 等命令阻塞的客户端的阻塞时间是否已经到达
        // 如果是的话，取消客户端的阻塞
        /* if (c->bpop.timeout != 0 && c->bpop.timeout < now_ms) { */
        /*     // 向客户端返回空回复 */
        /*     replyToBlockedClientTimedOut(c); */
        /*     // 取消客户端的阻塞状态 */
        /*     unblockClient(c); */
        /* } */
    /* } */

    // 客户端没有被关闭
    return 0;
}

/* The client query buffer is an sds.c string that can end with a lot of
 * free space not used, this function reclaims space if needed.
 *
 * 根据情况，缩小查询缓冲区的大小。
 *
 * The function always returns 0 as it never terminates the client.
 *
 * 函数总是返回 0 ，因为它不会中止客户端。
 */
int clientsCronResizeQueryBuffer(redisClient *c) {
    size_t querybuf_size = sdsAllocSize(c->querybuf);
    time_t idletime = server.unixtime - c->lastinteraction;

    /* There are two conditions to resize the query buffer:
     *
     * 符合以下两个条件的话，执行大小调整：
     *
     * 1) Query buffer is > BIG_ARG and too big for latest peak.
     *    查询缓冲区的大小大于 BIG_ARG 以及 querybuf_peak
     *
     * 2) Client is inactive and the buffer is bigger than 1k.
     *    客户端不活跃，并且缓冲区大于 1k 。
     */
    if (((querybuf_size > REDIS_MBULK_BIG_ARG) &&
         (querybuf_size/(c->querybuf_peak+1)) > 2) ||
        (querybuf_size > 1024 && idletime > 2))
    {
        /* Only resize the query buffer if it is actually wasting space. */
        if (sdsavail(c->querybuf) > 1024) {
            c->querybuf = sdsRemoveFreeSpace(c->querybuf);
        }
    }

    /* Reset the peak again to capture the peak memory usage in the next
     * cycle. */
    // 重置峰值
    c->querybuf_peak = 0;

    return 0;
}

/*
 * 客户端相关定时任务
 *
 * 1. 检查客户端超时情况
 * 2. 检查客户端查询缓冲区是否需要缩小
 */
void clientsCron(void) {
    /* Make sure to process at least 1/(server.hz*10) of clients per call.
     *
     * 这个函数每次执行都会处理至少1/(server.hz*10)比例的客户端。
     *
     * Since this function is called server.hz times per second we are sure that
     * in the worst case we process all the clients in 10 seconds.
     *
     * 因为这个函数每秒钟会调用 server.hz 次，
     * 所以在最坏情况下，服务器需要使用 10 秒钟来遍历所有客户端。
     *
     * In normal conditions (a reasonable number of clients) we process
     * all the clients in a shorter time.
     *
     * 在一般情况下，遍历所有客户端所需的时间会比实际中短很多。
     */

    // 客户端数量
    int numclients = listLength(server.clients);

    // 要处理的客户端数量
    int iterations = numclients/(server.hz*10);

    // 至少要处理 50 个客户端
    if (iterations < 50)
        iterations = (numclients < 50) ? numclients : 50;

    while(listLength(server.clients) && iterations--) {
        redisClient *c;
        listNode *head;

        /* Rotate the list, take the current head, process.
         * This way if the client must be removed from the list it's the
         * first element and we don't incur into O(N) computation. */
        // 翻转列表，然后取出表头元素，这样一来上一个被处理的客户端会被放到表头
        // 另外，如果程序要删除当前客户端，那么只要删除表头元素就可以了
        listRotate(server.clients);
        head = listFirst(server.clients);
        c = listNodeValue(head);
        // 检查客户端，并在客户端超时时关闭它
        /* The following functions do different service checks on the client.
         * The protocol is that they return non-zero if the client was
         * terminated. */
        if (clientsCronHandleTimeout(c)) continue;
        // 根据情况，缩小客户端查询缓冲区的大小
        if (clientsCronResizeQueryBuffer(c)) continue;
    }
}

/*
 * 数据库相关定时任务
 *
 * 1. 主动删除过期键，调整大小
 * 2. 执行主动的渐进式 rehash
 */
/* This function handles 'background' operations we are required to do
 * incrementally in Redis databases, such as active key expiring, resizing,
 * rehashing. */
void databasesCron(void) {

    // 函数先从数据库中删除过期键，然后再对数据库的大小进行修改

    // TODO: 复制相关
    // 如果服务器不是从服务器，那么执行主动过期键清除
    /* Expire keys by random sampling. Not required for slaves
     * as master will synthesize DELs for us. */
    if (server.active_expire_enabled /* && server.masterhost == NULL */)
        // 清除模式为ACTIVE_EXPIRE_CYCLE_SLOW，这个模式会尽量多清除过期键
        activeExpireCycle(ACTIVE_EXPIRE_CYCLE_SLOW);

    // 在没有 BGSAVE 或者 BGREWRITEAOF 执行时，对哈希表进行 rehash
    /* Perform hash tables rehashing if needed, but only if there are no
     * other processes saving the DB on disk. Otherwise rehashing is bad
     * as will cause a lot of copy-on-write of memory pages. */
    if (server.rdb_child_pid == -1 && server.aof_child_pid == -1) {
        /* We use global counters so if we stop the computation at a given
         * DB we'll be able to start from the successive in the next
         * cron loop iteration. */
        static unsigned int resize_db = 0;
        static unsigned int rehash_db = 0;
        unsigned int dbs_per_call = REDIS_DBCRON_DBS_PER_CALL;
        unsigned int j;

        // 设定要测试的数据库数量
        /* Don't test more DBs than we have. */
        if (dbs_per_call > server.dbnum) dbs_per_call = server.dbnum;

        // 调整字典的大小
        /* Resize */
        for (j = 0; j < dbs_per_call; j++) {
            tryResizeHashTables(resize_db % server.dbnum);
            resize_db++;
        }

        // 对字典进行渐进式 rehash
        /* Rehash */
        if (server.activerehashing) {
            for (j = 0; j < dbs_per_call; j++) {
                int work_done = incrementallyRehash(rehash_db % server.dbnum);
                rehash_db++;
                if (work_done) {
                    /* If the function did some work, stop here, we'll do
                     * more at the next cron loop. */
                    break;
                }
            }
        }
    }
}

/*
 * redis时间事件serverCron
 */
/* This is our timer interrupt, called server.hz times per second.
 *
 * 这是 Redis 的时间中断器，每秒调用 server.hz 次。
 *
 * Here is where we do a number of things that need to be done asynchronously.
 * For instance:
 *
 * 以下是需要异步执行的操作：
 *
 * - Active expired keys collection (it is also performed in a lazy way on
 *   lookup).
 *   主动清除过期键。
 *
 * - Software watchdog.
 *   更新软件 watchdog 的信息。
 *
 * - Update some statistic.
 *   更新统计信息。
 *
 * - Incremental rehashing of the DBs hash tables.
 *   对数据库进行渐增式 Rehash
 *
 * - Triggering BGSAVE / AOF rewrite, and handling of terminated children.
 *   触发 BGSAVE 或者 AOF 重写，并处理之后由 BGSAVE 和 AOF 重写引发的子进程停止。
 *
 * - Clients timeout of different kinds.
 *   处理客户端超时。
 *
 * - Replication reconnection.
 *   复制重连
 *
 * - Many more...
 *   等等。。。
 *
 * Everything directly called here will be called server.hz times per second,
 * so in order to throttle execution of things we want to do less frequently
 * a macro is used: run_with_period(milliseconds) { .... }
 *
 * 因为 serverCron 函数中的所有代码都会每秒调用 server.hz 次，
 * 为了对部分代码的调用次数进行限制，
 * 使用了一个宏 run_with_period(milliseconds) { ... } ，
 * 这个宏可以将被包含代码的执行次数降低为每 milliseconds 执行一次。
 */

int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {

    int j;
    REDIS_NOTUSED(eventLoop);
    REDIS_NOTUSED(id);
    REDIS_NOTUSED(clientData);

    // TODO: 看门狗功能相关
    /* Software watchdog: deliver the SIGALRM that will reach the signal
     * handler if we don't return here fast enough. */
    /* if (server.watchdog_period) watchdogScheduleSignal(server.watchdog_period); */

    // 更新缓存的时钟
    /* Update the time cache. */
    updateCachedTime();

    // 记录服务器执行命令的次数
    run_with_period(100) trackOperationsPerSecond();

    /* We have just REDIS_LRU_BITS bits per object for LRU information.
     * So we use an (eventually wrapping) LRU clock.
     *
     * Note that even if the counter wraps it's not a big problem,
     * everything will still work but some object will appear younger
     * to Redis. However for this to happen a given object should never be
     * touched for all the time needed to the counter to wrap, which is
     * not likely.
     *
     * 即使服务器的时间最终比 1.5 年长也无所谓，
     * 对象系统仍会正常运作，不过一些对象可能会比服务器本身的时钟更年轻。
     * 不过这要这个对象在 1.5 年内都没有被访问过，才会出现这种现象。
     *
     * Note that you can change the resolution altering the
     * REDIS_LRU_CLOCK_RESOLUTION define.
     *
     * LRU 时间的精度可以通过修改 REDIS_LRU_CLOCK_RESOLUTION 常量来改变。
     */
    server.lruclock = getLRUClock();

    // 记录服务器的内存峰值
    /* Record the max memory used since the server was started. */
    if (zmalloc_used_memory() > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used_memory();

    // 记录服务器的驻留集大小
    /* Sample the RSS here since this is a relatively slow call. */
    server.resident_set_size = zmalloc_get_rss();

    // 服务器进程收到 SIGTERM 信号，关闭服务器
    /* We received a SIGTERM, shutting down here in a safe way, as it is
     * not ok doing so inside the signal handler. */
    if (server.shutdown_asap) {

        // 尝试关闭服务器
        if (prepareForShutdown(0) == REDIS_OK) exit(0);

        // 如果关闭失败，那么打印 LOG ，并移除关闭标识
        printf("SIGTERM received but errors trying to shut down the server, check the logs for more information\n");
        server.shutdown_asap = 0;
    }

    // 打印数据库的键值对统计信息
    /* Show some info about non-empty databases */
    run_with_period(5000) {
        for (j = 0; j < server.dbnum; j++) {
            long long size, used, vkeys;

            // 字典总大小
            size = dictSlots(server.db[j].dict);
            // 已用键值对的数量
            used = dictSize(server.db[j].dict);
            // 带有过期时间的键值对数量
            vkeys = dictSize(server.db[j].expires);

            // 用 LOG 打印数量
            if (used || vkeys) {
                printf("DB %d: %lld keys (%lld volatile) in %lld slots HT.\n",j,used,vkeys,size);
                /* dictPrintStats(server.dict); */
            }
        }
    }

    // TODO: 哨兵相关
    // 如果服务器没有运行在 SENTINEL 模式下，那么打印客户端的连接信息
    /* Show information about connected clients */
    // if (!server.sentinel_mode) {
    //     run_with_period(5000) {
    //         redisLog(REDIS_VERBOSE,
    //                  "%lu clients connected (%lu slaves), %zu bytes in use",
    //                  listLength(server.clients)-listLength(server.slaves),
    //                  listLength(server.slaves),
    //                  zmalloc_used_memory());
    //     }
    // }

    // 检查客户端，关闭超时客户端，并释放客户端多余的缓冲区
    /* We need to do a few operations on clients asynchronously. */
    clientsCron();

    // 对数据库执行定时操作
    /* Handle background operations on Redis databases. */
    databasesCron();

    // AOF持久化，后台开启一个子进程完成AOF文件的重写工作
    // 如果 BGSAVE 和 BGREWRITEAOF 都没有在执行，并且有一个 BGREWRITEAOF 在等待，那么执行 BGREWRITEAOF
    /* Start a scheduled AOF rewrite if this was requested by the user while
     * a BGSAVE was in progress. */
    if (server.rdb_child_pid == -1 && server.aof_child_pid == -1 &&
        server.aof_rewrite_scheduled)
    {
        rewriteAppendOnlyFileBackground();
    }

    // AOF持久化 & RDB持久化
    // 检查 BGSAVE 或者 BGREWRITEAOF 是否已经执行完毕
    /* Check if a background saving or AOF rewrite in progress terminated. */
    if (server.rdb_child_pid != -1 || server.aof_child_pid != -1) {
        int statloc;
        pid_t pid;

        // 接收子进程发来的信号，非阻塞
        if ((pid = wait3(&statloc,WNOHANG,NULL)) != 0) {
            int exitcode = WEXITSTATUS(statloc);
            int bysignal = 0;

            if (WIFSIGNALED(statloc)) bysignal = WTERMSIG(statloc);

            // RDB持久化
            // BGSAVE 执行完毕
            if (pid == server.rdb_child_pid) {
                /* backgroundSaveDoneHandler(exitcode,bysignal); */

            // BGREWRITEAOF 执行完毕
            } else if (pid == server.aof_child_pid) {
                backgroundRewriteDoneHandler(exitcode,bysignal);

            } else {
                printf(
                         "Warning, detected child with unmatched pid: %ld\n",
                         (long)pid);
            }
            updateDictResizePolicy();
        }

    // 既然没有 BGSAVE 或者 BGREWRITEAOF 在执行，那么检查是否需要执行它们
    } else {

        /* If there is not a background saving/rewrite in progress check if
         * we have to save/rewrite now */

        // RDB持久化
        // 遍历所有保存条件，看是否需要执行 BGSAVE 命令
        for (j = 0; j < server.saveparamslen; j++) {
            struct saveparam *sp = server.saveparams+j;

            // 检查是否有某个保存条件已经满足了
            /* Save if we reached the given amount of changes,
             * the given amount of seconds, and if the latest bgsave was
             * successful or if, in case of an error, at least
             * REDIS_BGSAVE_RETRY_DELAY seconds already elapsed. */
            if (server.dirty >= sp->changes &&
                server.unixtime-server.lastsave > sp->seconds &&
                (server.unixtime-server.lastbgsave_try >
                 REDIS_BGSAVE_RETRY_DELAY ||
                 server.lastbgsave_status == REDIS_OK))
            {
                printf("%d changes in %d seconds. Saving...\n",
                         sp->changes, (int)sp->seconds);
                // 执行 BGSAVE
                /* rdbSaveBackground(server.rdb_filename); */
                break;
            }
        }

        // AOF持久化
        // 判断是否触发BGREWRITEAOF
        /* Trigger an AOF rewrite if needed */
        if (server.rdb_child_pid == -1 &&
            server.aof_child_pid == -1 &&
            server.aof_rewrite_perc &&
            // AOF 文件的当前大小大于执行 BGREWRITEAOF 所需的最小大小
            server.aof_current_size > server.aof_rewrite_min_size)
        {
            // 上一次完成 AOF 写入之后，AOF 文件的大小
            long long base = server.aof_rewrite_base_size ?
                             server.aof_rewrite_base_size : 1;

            // AOF 文件当前的体积相对于 base 的体积的百分比
            long long growth = (server.aof_current_size*100/base) - 100;

            // 如果增长体积的百分比超过了 growth ，那么执行 BGREWRITEAOF
            if (growth >= server.aof_rewrite_perc) {
                printf("Starting automatic rewriting of AOF on %lld%% growth\n",growth);
                // 执行 BGREWRITEAOF
                rewriteAppendOnlyFileBackground();
            }
        }
    }

    // AOF持久化，根据策略决定将aof缓冲区写入和同步的行为
    // 根据 AOF 政策，考虑是否需要将 AOF 缓冲区中的内容写入到 AOF 文件中
    /* AOF postponed flush: Try at every cron cycle if the slow fsync
     * completed. */
    if (server.aof_flush_postponed_start) flushAppendOnlyFile(0);

    // AOF持久化，根据策略决定将aof缓冲区写入和同步的行为
    /* AOF write errors: in this case we have a buffer to flush as well and
     * clear the AOF error in case of success to make the DB writable again,
     * however to try every second is enough in case of 'hz' is set to
     * an higher frequency. */
    run_with_period(1000) {
        if (server.aof_last_write_status == REDIS_ERR)
            flushAppendOnlyFile(0);
    }

    // 关闭那些需要异步关闭的客户端
    /* Close clients that need to be closed asynchronous */
    freeClientsInAsyncFreeQueue();

    // TODO: 客户端暂停相关
    /* Clear the paused clients flag if needed. */
    /* clientsArePaused(); */ /* Don't check return value, just use the side effect. */

    // TODO: 复制相关
    /* Replication cron function -- used to reconnect to master and
     * to detect transfer failures. */
    // 复制函数
    // 重连接主服务器、向主服务器发送 ACK 、判断数据发送失败情况、断开本服务器超时的从服务器，等等
    /* run_with_period(1000) replicationCron(); */

    // TODO: 集群相关
    /* Run the Redis Cluster cron. */
    // 如果服务器运行在集群模式下，那么执行集群操作
    /* run_with_period(100) { */
    /*     if (server.cluster_enabled) clusterCron(); */
    /* } */

    // TODO: 哨兵相关
    /* Run the Sentinel timer if we are in sentinel mode. */
    // 如果服务器运行在 sentinel 模式下，那么执行 SENTINEL 的主函数
    /* run_with_period(100) { */
    /*     if (server.sentinel_mode) sentinelTimer(); */
    /* } */

    // TODO: 集群相关
    /* Cleanup expired MIGRATE cached sockets. */
    /* run_with_period(1000) { */
    /*     migrateCloseTimedoutSockets(); */
    /* } */

    // 增加 loop 计数器
    server.cronloops++;

    // 1000/server.hz后再次到达，单位ms
    return 1000/server.hz;
}

/* -----------------------------------------------------------------------------
 * 内存超出限制时淘汰API
 * -------------------------------------------------------------------------- */

/* freeMemoryIfNeeded() gets called when 'maxmemory' is set on the config
 * file to limit the max memory used by the server, before processing a
 * command.
 *
 * 此函数在 maxmemory 选项被打开，并且内存超出限制时调用。
 *
 * The goal of the function is to free enough memory to keep Redis under the
 * configured memory limit.
 *
 * 此函数的目的是释放 Redis 的占用内存至 maxmemory 选项设置的最大值之下。
 *
 * The function starts calculating how many bytes should be freed to keep
 * Redis under the limit, and enters a loop selecting the best keys to
 * evict accordingly to the configured policy.
 *
 * 函数先计算出需要释放多少字节才能低于 maxmemory 选项设置的最大值，
 * 然后根据指定的淘汰算法，选出最适合被淘汰的键进行释放。
 *
 * If all the bytes needed to return back under the limit were freed the
 * function returns REDIS_OK, otherwise REDIS_ERR is returned, and the caller
 * should block the execution of commands that will result in more memory
 * used by the server.
 *
 * 如果成功释放了所需数量的内存，那么函数返回 REDIS_OK ，否则函数将返回 REDIS_ERR ，
 * 并阻止执行新的命令。
 *
 * ------------------------------------------------------------------------
 *
 * LRU approximation algorithm
 *
 * Redis uses an approximation of the LRU algorithm that runs in constant
 * memory. Every time there is a key to expire, we sample N keys (with
 * N very small, usually in around 5) to populate a pool of best keys to
 * evict of M keys (the pool size is defined by REDIS_EVICTION_POOL_SIZE).
 *
 * The N keys sampled are added in the pool of good keys to expire (the one
 * with an old access time) if they are better than one of the current keys
 * in the pool.
 *
 * After the pool is populated, the best key we have in the pool is expired.
 * However note that we don't remove keys from the pool when they are deleted
 * so the pool may contain keys that no longer exist.
 *
 * When we try to evict a key, and all the entries in the pool don't exist
 * we populate it again. This time we'll be sure that the pool has at least
 * one key that can be evicted, if there is at least one key that can be
 * evicted in the whole database. */

/*
 * 创建一个驱逐池(一段连续的内存)
 */
/* Create a new eviction pool. */
struct evictionPoolEntry *evictionPoolAlloc(void) {
    struct evictionPoolEntry *ep;
    int j;

    // 驱逐池默认大小: REDIS_EVICTION_POOL_SIZE = 16
    ep = zmalloc(sizeof(*ep)*REDIS_EVICTION_POOL_SIZE);
    for (j = 0; j < REDIS_EVICTION_POOL_SIZE; j++) {
        ep[j].idle = 0;
        ep[j].key = NULL;
    }
    return ep;
}

/*
 * 就地插入键(按照空转时间升序)，提供给freeMemoryIfNeeded调用的辅助函数
 */
/* This is an helper function for freeMemoryIfNeeded(), it is used in order
 * to populate the evictionPool with a few entries every time we want to
 * expire a key. Keys with idle time smaller than one of the current
 * keys are added. Keys are always added if there are free entries.
 *
 * We insert keys on place in ascending order, so keys with the smaller
 * idle time are on the left, and keys with the higher idle time on the
 * right. */

#define EVICTION_SAMPLES_ARRAY_SIZE 16
void evictionPoolPopulate(dict *sampledict, dict *keydict, struct evictionPoolEntry *pool) {
    int j, k, count;
    dictEntry *_samples[EVICTION_SAMPLES_ARRAY_SIZE];
    dictEntry **samples;

    /* Try to use a static buffer: this function is a big hit...
     * Note: it was actually measured that this helps. */
    if (server.maxmemory_samples <= EVICTION_SAMPLES_ARRAY_SIZE) {
        samples = _samples;
    } else {
        samples = zmalloc(sizeof(samples[0])*server.maxmemory_samples);
    }

    /*
     * 从字典sampledict中随机取server.maxmemory_samples个键值对存在samples中
     */ 
    
#if 1 /* Use bulk get by default. */
    count = dictGetRandomKeys(sampledict,samples,server.maxmemory_samples);
#else
    count = server.maxmemory_samples;
    for (j = 0; j < count; j++) samples[j] = dictGetRandomKey(sampledict);
#endif

    for (j = 0; j < count; j++) {
        unsigned long long idle;
        sds key;
        robj *o;
        dictEntry *de;

        de = samples[j];
        key = dictGetKey(de);
        /* If the dictionary we are sampling from is not the main
         * dictionary (but the expires one) we need to lookup the key
         * again in the key dictionary to obtain the value object. */
        if (sampledict != keydict) de = dictFind(keydict, key);
        o = dictGetVal(de);
        // 取出第j个对象的空转时间
        idle = estimateObjectIdleTime(o);

        // 将第j个对象插入到驱逐池中，插入排序
        /* Insert the element inside the pool.
         * First, find the first empty bucket or the first populated
         * bucket that has an idle time smaller than our idle time. */
        k = 0;
        while (k < REDIS_EVICTION_POOL_SIZE &&
               pool[k].key &&
               pool[k].idle < idle) k++;

        // 第j个对象的空转时间比当前驱逐池第一个元素还小，而且驱逐池满，则不需要插入
        if (k == 0 && pool[REDIS_EVICTION_POOL_SIZE-1].key != NULL) {
            /* Can't insert if the element is < the worst element we have
             * and there are no empty buckets. */
            continue;
        // 第j个对象插入到驱逐池第一个空位置
        } else if (k < REDIS_EVICTION_POOL_SIZE && pool[k].key == NULL) {
            /* Inserting into empty position. No setup needed before insert. */
        } else {
            // 在中间插入，且驱逐池未满，则不需要释放
            /* Inserting in the middle. Now k points to the first element
             * greater than the element to insert.  */
            if (pool[REDIS_EVICTION_POOL_SIZE-1].key == NULL) {
                /* Free space on the right? Insert at k shifting
                 * all the elements from k to end to the right. */
                memmove(pool+k+1,pool+k,
                        sizeof(pool[0])*(REDIS_EVICTION_POOL_SIZE-k-1));
            // 在中间插入，且驱逐池已满，则释放第一个元素(空转时间最短的)
            } else {
                /* No free space on right? Insert at k-1 */
                k--;
                /* Shift all elements on the left of k (included) to the
                 * left, so we discard the element with smaller idle time. */
                sdsfree(pool[0].key);
                memmove(pool,pool+1,sizeof(pool[0])*k);
            }
        }
        pool[k].key = sdsdup(key);
        pool[k].idle = idle;
    }
    if (samples != _samples) zfree(samples);
}

/*
 * 如果使用内存超出设置，则根据策略释放内存
 */
int freeMemoryIfNeeded(void) {
    size_t mem_used, mem_tofree, mem_freed;
    // TODO: 复制相关
    // int slaves = listLength(server.slaves);

    /* Remove the size of slaves output buffers and AOF buffer from the
     * count of used memory. */
    // 计算出 Redis 目前占用的内存总数，但有两个方面的内存不会计算在内：
    // 1）从服务器的输出缓冲区的内存
    // 2）AOF缓冲区的内存
    mem_used = zmalloc_used_memory();

    // TODO: 复制相关
    // if (slaves) {
    //     listIter li;
    //     listNode *ln;
    // 
    //     listRewind(server.slaves,&li);
    //     while((ln = listNext(&li))) {
    //         redisClient *slave = listNodeValue(ln);
    //         unsigned long obuf_bytes = getClientOutputBufferMemoryUsage(slave);
    //         if (obuf_bytes > mem_used)
    //             mem_used = 0;
    //         else
    //             mem_used -= obuf_bytes;
    //     }
    // }

    if (server.aof_state != REDIS_AOF_OFF) {
        mem_used -= sdslen(server.aof_buf);
        mem_used -= aofRewriteBufferSize();
    }

    // 如果目前使用的内存大小比设置的 maxmemory 要小，那么无须执行进一步操作
    /* Check if we are over the memory limit. */
    if (mem_used <= server.maxmemory) return REDIS_OK;

    // 如果占用内存比 maxmemory 要大，但是 maxmemory 策略为不淘汰，那么直接返回
    if (server.maxmemory_policy == REDIS_MAXMEMORY_NO_EVICTION)
        return REDIS_ERR; /* We need to free memory, but policy forbids. */

    // 计算需要释放多少字节的内存
    /* Compute how much memory we need to free. */
    mem_tofree = mem_used - server.maxmemory;

    // 初始化已释放内存的字节数为 0
    mem_freed = 0;

    // 根据 maxmemory 策略，释放内存并记录被释放内存的字节数
    while (mem_freed < mem_tofree) {
        int j, k, keys_freed = 0;

        // 遍历所有数据库
        for (j = 0; j < server.dbnum; j++) {
            long bestval = 0; /* just to prevent warning */
            sds bestkey = NULL;
            dictEntry *de;
            redisDb *db = server.db+j;
            dict *dict;

            // 如果策略是 allkeys-lru 或者 allkeys-random，那么淘汰的目标为所有数据库键(从数据库键空间采样)
            if (server.maxmemory_policy == REDIS_MAXMEMORY_ALLKEYS_LRU ||
                server.maxmemory_policy == REDIS_MAXMEMORY_ALLKEYS_RANDOM)
            {
                dict = server.db[j].dict;
            // 如果策略是 volatile-lru 、 volatile-random 或者 volatile-ttl，那么淘汰的目标为带过期时间的数据库键(从过期字典中采样)
            } else {
                dict = server.db[j].expires;
            }

            // 跳过空字典
            if (dictSize(dict) == 0) continue;

            // 如果使用的是随机策略，那么从目标字典中随机选出键
            /* volatile-random and allkeys-random policy */
            if (server.maxmemory_policy == REDIS_MAXMEMORY_ALLKEYS_RANDOM ||
                server.maxmemory_policy == REDIS_MAXMEMORY_VOLATILE_RANDOM)
            {
                de = dictGetRandomKey(dict);
                bestkey = dictGetKey(de);
            }
 
            // 如果使用的是 LRU 策略，那么从采样出来的键值对中选出 IDLE 时间最长的那个键
            /* volatile-lru and allkeys-lru policy */
            else if (server.maxmemory_policy == REDIS_MAXMEMORY_ALLKEYS_LRU ||
                     server.maxmemory_policy == REDIS_MAXMEMORY_VOLATILE_LRU)
            {
                struct evictionPoolEntry *pool = db->eviction_pool;

                while(bestkey == NULL) {
                    evictionPoolPopulate(dict, db->dict, db->eviction_pool);
                    /* Go backward from best to worst element to evict. */
                    for (k = REDIS_EVICTION_POOL_SIZE-1; k >= 0; k--) {
                        if (pool[k].key == NULL) continue;
                        de = dictFind(dict,pool[k].key);

                        /* Remove the entry from the pool. */
                        sdsfree(pool[k].key);
                        /* Shift all elements on its right to left. */
                        memmove(pool+k,pool+k+1,
                                sizeof(pool[0])*(REDIS_EVICTION_POOL_SIZE-k-1));
                        /* Clear the element on the right which is empty
                         * since we shifted one position to the left.  */
                        pool[REDIS_EVICTION_POOL_SIZE-1].key = NULL;
                        pool[REDIS_EVICTION_POOL_SIZE-1].idle = 0;

                        /* If the key exists, is our pick. Otherwise it is
                         * a ghost and we need to try the next element. */
                        if (de) {
                            bestkey = dictGetKey(de);
                            break;
                        } else {
                            /* Ghost... */
                            continue;
                        }
                    }
                }
            }

            // 策略为 volatile-ttl ，从采样出来的键值对中选出过期时间距离当前时间最接近的键
            /* volatile-ttl */
            else if (server.maxmemory_policy == REDIS_MAXMEMORY_VOLATILE_TTL) {
                for (k = 0; k < server.maxmemory_samples; k++) {
                    sds thiskey;
                    long thisval;

                    de = dictGetRandomKey(dict);
                    thiskey = dictGetKey(de);
                    thisval = (long) dictGetVal(de);

                    /* Expire sooner (minor expire unix timestamp) is better
                     * candidate for deletion */
                    if (bestkey == NULL || thisval < bestval) {
                        bestkey = thiskey;
                        bestval = thisval;
                    }
                }
            }

            // 删除被选中的键
            /* Finally remove the selected key. */
            if (bestkey) {
                long long delta;

                robj *keyobj = createStringObject(bestkey,sdslen(bestkey));

                // 把删除信息传播到AOF
                propagateExpire(db,keyobj);

                // 计算删除键所释放的内存数量
                /* We compute the amount of memory freed by dbDelete() alone.
                 * It is possible that actually the memory needed to propagate
                 * the DEL in AOF and replication link is greater than the one
                 * we are freeing removing the key, but we can't account for
                 * that otherwise we would never exit the loop.
                 *
                 * AOF and Output buffer memory will be freed eventually so
                 * we only care about memory used by the key space. */
                delta = (long long) zmalloc_used_memory();
                dbDelete(db,keyobj);
                delta -= (long long) zmalloc_used_memory();
                mem_freed += delta;

                // 对淘汰键的计数器增一
                server.stat_evictedkeys++;

                // TODO: 发布/订阅相关
                // 发送事件
                // notifyKeyspaceEvent(REDIS_NOTIFY_EVICTED, "evicted", keyobj, db->id);

                decrRefCount(keyobj);
                keys_freed++;

                // TODO: 复制相关
                /* When the memory to free starts to be big enough, we may
                 * start spending so much time here that is impossible to
                 * deliver data to the slaves fast enough, so we force the
                 * transmission here inside the loop. */
                // if (slaves) flushSlavesOutputBuffers();
            }
        }

        if (!keys_freed) return REDIS_ERR; /* nothing to free... */
    }

    return REDIS_OK;
}

/* -----------------------------------------------------------------------------
 * 执行redis命令相关API
 * -------------------------------------------------------------------------- */

/*
 * Redis OP Array API
 */
void redisOpArrayInit(redisOpArray *oa) {
    oa->ops = NULL;
    oa->numops = 0;
}

/*
 * Redis OP Array API
 */
int redisOpArrayAppend(redisOpArray *oa, struct redisCommand *cmd, int dbid,
                       robj **argv, int argc, int target)
{
    redisOp *op;

    oa->ops = zrealloc(oa->ops,sizeof(redisOp)*(oa->numops+1));
    op = oa->ops+oa->numops;
    op->cmd = cmd;
    op->dbid = dbid;
    op->argv = argv;
    op->argc = argc;
    op->target = target;
    oa->numops++;
    return oa->numops;
}

/*
 * Redis OP Array API
 */
void redisOpArrayFree(redisOpArray *oa) {
    while(oa->numops) {
        int j;
        redisOp *op;

        oa->numops--;
        op = oa->ops+oa->numops;
        for (j = 0; j < op->argc; j++)
            decrRefCount(op->argv[j]);
        zfree(op->argv);
    }
    zfree(oa->ops);
}

/*
 * 命令传播
 */
void propagate(struct redisCommand *cmd, int dbid, robj **argv, int argc, int flags) {
    // 传播到AOF缓存
    if (server.aof_state != REDIS_AOF_OFF && flags & REDIS_PROPAGATE_AOF)
        feedAppendOnlyFile(cmd,dbid,argv,argc);

    // TODO: 复制相关
    // 传播到slave
    // if (flags & REDIS_PROPAGATE_REPL)
    //     replicationFeedSlaves(server.slaves,dbid,argv,argc);
}

/*
 * 调用实际的redis命令函数
 */
/* Call() is the core of Redis execution of a command */
void call(redisClient *c, int flags) {
    // start 记录命令开始执行的时间
    long long dirty, start, duration;
    // 记录命令开始执行前的 FLAG
    int client_old_flags = c->flags;

    // TODO: 监视器相关
    /* Sent the command to clients in MONITOR mode, only if the commands are
     * not generated from reading an AOF. */
    // if (listLength(server.monitors) &&
    //     !server.loading &&
    //    !(c->cmd->flags & REDIS_CMD_SKIP_MONITOR))
    // {
    //     replicationFeedMonitors(c,server.monitors,c->db->id,c->argv,c->argc);
    // }

    /* Call the command. */
    c->flags &= ~(REDIS_FORCE_AOF|REDIS_FORCE_REPL);
    redisOpArrayInit(&server.also_propagate);
    // 保留旧 dirty 计数器值
    dirty = server.dirty;
    // 计算命令开始执行的时间
    start = ustime();
    // 执行实现函数
    c->cmd->proc(c);
    // 计算命令执行耗费的时间
    duration = ustime()-start;
    // 计算命令执行之后的 dirty 值
    dirty = server.dirty-dirty;

    // TODO: LUA相关
    // 不将从 Lua 中发出的命令放入 SLOWLOG ，也不进行统计
    /* When EVAL is called loading the AOF we don't want commands called
     * from Lua to go into the slowlog or to populate statistics. */
    // if (server.loading && c->flags & REDIS_LUA_CLIENT)
    //     flags &= ~(REDIS_CALL_SLOWLOG | REDIS_CALL_STATS);

    // TODO: LUA相关
    // 如果调用者是 Lua ，那么根据命令 FLAG 和客户端 FLAG，打开传播(propagate)标志
    /* If the caller is Lua, we want to force the EVAL caller to propagate
     * the script if the command flag or client flag are forcing the
     * propagation. */
    // if (c->flags & REDIS_LUA_CLIENT && server.lua_caller) {
    //     if (c->flags & REDIS_FORCE_REPL)
    //         server.lua_caller->flags |= REDIS_FORCE_REPL;
    //     if (c->flags & REDIS_FORCE_AOF)
    //         server.lua_caller->flags |= REDIS_FORCE_AOF;
    // }

    // TODO: 慢日志相关
    // 如果有需要，将命令放到 SLOWLOG 里面
    /* Log the command into the Slow log if needed, and populate the
     * per-command statistics that we show in INFO commandstats. */
    // if (flags & REDIS_CALL_SLOWLOG && c->cmd->proc != execCommand)
    //     slowlogPushEntryIfNeeded(c->argv,c->argc,duration);

    // 更新命令的统计信息
    if (flags & REDIS_CALL_STATS) {
        c->cmd->microseconds += duration;
        c->cmd->calls++;
    }

    // AOF持久化
    // 将命令传播到 AOF 和 slave 节点
    /* Propagate the command into the AOF and replication link */
    if (flags & REDIS_CALL_PROPAGATE) {
        int flags = REDIS_PROPAGATE_NONE;

        // 强制 REPL 传播
        if (c->flags & REDIS_FORCE_REPL) flags |= REDIS_PROPAGATE_REPL;

        // 强制 AOF 传播
        if (c->flags & REDIS_FORCE_AOF) flags |= REDIS_PROPAGATE_AOF;

        // 如果数据库有被修改，那么启用 REPL 和 AOF 传播
        if (dirty)
            flags |= (REDIS_PROPAGATE_REPL | REDIS_PROPAGATE_AOF);

        // 如果数据库有修改或强制传播，则进行命令传播
        if (flags != REDIS_PROPAGATE_NONE)
            propagate(c->cmd,c->db->id,c->argv,c->argc,flags);
    }

    // 将客户端的 FLAG 恢复到命令执行之前，因为 call 可能会递归执行
    /* Restore the old FORCE_AOF/REPL flags, since call can be executed
     * recursively. */
    c->flags &= ~(REDIS_FORCE_AOF|REDIS_FORCE_REPL);
    c->flags |= client_old_flags & (REDIS_FORCE_AOF|REDIS_FORCE_REPL);

    // 传播额外的命令
    /* Handle the alsoPropagate() API to handle commands that want to propagate
     * multiple separated commands. */
    if (server.also_propagate.numops) {
        int j;
        redisOp *rop;

        for (j = 0; j < server.also_propagate.numops; j++) {
            rop = &server.also_propagate.ops[j];
            propagate(rop->cmd, rop->dbid, rop->argv, rop->argc, rop->target);
        }
        redisOpArrayFree(&server.also_propagate);
    }

    // 更新统计信息，已经处理命令的数量
    server.stat_numcommands++;
}

/*
 * 执行redis命令的函数
 */
/* If this function gets called we already read a whole
 * command, arguments are in the client argv/argc fields.
 * processCommand() execute the command or prepare the
 * server for a bulk read from the client.
 *
 * 这个函数执行时，我们已经读入了一个完整的命令到客户端，这个函数负责执行这个命令，
 * 或者服务器准备从客户端中进行一次读取。
 *
 * If 1 is returned the client is still alive and valid and
 * other operations can be performed by the caller. Otherwise
 * if 0 is returned the client was destroyed (i.e. after QUIT).
 *
 * 如果这个函数返回 1 ，那么表示客户端在执行命令之后仍然存在，调用者可以继续执行其他操作。
 * 否则，如果这个函数返回 0 ，那么表示客户端已经被销毁。
 */
int processCommand(redisClient *c) {

    // QUIT命令被单独处理
    /* The QUIT command is handled separately. Normal command procs will
     * go through checking for replication and QUIT will cause trouble
     * when FORCE_REPLICATION is enabled and would be implemented in
     * a regular command proc. */
    if (!strcasecmp(c->argv[0]->ptr, "quit")) {
        addReply(c, shared.ok);
        // 将客户端标志REDIS_CLOSE_AFTER_REPLY置位
        c->flags |= REDIS_CLOSE_AFTER_REPLY;
        return REDIS_ERR;
    }

    // 查找命令，并进行命令合法性检查，以及命令参数个数检查
    /* Now lookup the command and check ASAP about trivial error conditions
     * such as wrong arity, bad command name and so forth. */
    c->cmd = c->lastcmd = lookupCommand(c->argv[0]->ptr);
    // 未查找到命令
    if (!c->cmd) {
        // TODO: 事务相关
        // flagTransaction(c);
        addReplyErrorFormat(c, "unknown command '%s'", (char*)c->argv[0]->ptr);
        return REDIS_OK;
    // 命令参数个数错误
    } else if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) || (c->argc < -c->cmd->arity)) {
        // TODO: 事务相关
        // flagTransaction(c);
        addReplyErrorFormat(c,"wrong number of arguments for '%s' command", c->cmd->name);
        return REDIS_OK;
    }

    // TODO: 认证相关
    // 检查认证信息
    /* Check if the user is authenticated */
    // if (server.requirepass && !c->authenticated && c->cmd->proc != authCommand)
    // {
    //     flagTransaction(c);
    //     addReply(c,shared.noautherr);
    //     return REDIS_OK;
    // }


    // TODO: 集群相关
    /* If cluster is enabled perform the cluster redirection here.
     *
     * 如果开启了集群模式，那么在这里进行转向操作。
     *
     * However we don't perform the redirection if:
     *
     * 不过，如果有以下情况出现，那么节点不进行转向：
     *
     * 1) The sender of this command is our master.
     *    命令的发送者是本节点的主节点
     *
     * 2) The command has no key arguments.
     *    命令没有 key 参数
     */
    // if (server.cluster_enabled &&
    //     !(c->flags & REDIS_MASTER) &&
    //     !(c->cmd->getkeys_proc == NULL && c->cmd->firstkey == 0))
    // {
    //     int hashslot;
    // 
    //     // 集群已下线
    //     if (server.cluster->state != REDIS_CLUSTER_OK) {
    //         flagTransaction(c);
    //         addReplySds(c,sdsnew("-CLUSTERDOWN The cluster is down. Use CLUSTER INFO for more information\r\n"));
    //         return REDIS_OK;
    // 
    //     // 集群运作正常
    //     } else {
    //         int error_code;
    //         clusterNode *n = getNodeByQuery(c,c->cmd,c->argv,c->argc,&hashslot,&error_code);
    //         // 不能执行多键处理命令
    //         if (n == NULL) {
    //             flagTransaction(c);
    //             if (error_code == REDIS_CLUSTER_REDIR_CROSS_SLOT) {
    //                 addReplySds(c,sdsnew("-CROSSSLOT Keys in request don't hash to the same slot\r\n"));
    //             } else if (error_code == REDIS_CLUSTER_REDIR_UNSTABLE) {
    //                 /* The request spawns mutliple keys in the same slot,
    //                  * but the slot is not "stable" currently as there is
    //                  * a migration or import in progress. */
    //                 addReplySds(c,sdsnew("-TRYAGAIN Multiple keys request during rehashing of slot\r\n"));
    //             } else {
    //                 redisPanic("getNodeByQuery() unknown error.");
    //             }
    //             return REDIS_OK;
    // 
    //         // 命令针对的槽和键不是本节点处理的，进行转向
    //         } else if (n != server.cluster->myself) {
    //             flagTransaction(c);
    //             // -<ASK or MOVED> <slot> <ip>:<port>
    //             // 例如 -ASK 10086 127.0.0.1:12345
    //             addReplySds(c,sdscatprintf(sdsempty(),
    //                                        "-%s %d %s:%d\r\n",
    //                                        (error_code == REDIS_CLUSTER_REDIR_ASK) ? "ASK" : "MOVED",
    //                                        hashslot,n->ip,n->port));
    // 
    //             return REDIS_OK;
    //         }

    //         // 如果执行到这里，说明键 key 所在的槽由本节点处理
    //         // 或者客户端执行的是无参数命令
    //     }
    // }

    // 如果设置了最大内存，那么检查内存是否超过限制，并做相应的操作
    /* Handle the maxmemory directive.
     *
     * First we try to free some memory if possible (if there are volatile
     * keys in the dataset). If there are not the only thing we can do
     * is returning an error. */
    if (server.maxmemory) {
        // 如果内存已超过限制，那么尝试通过删除过期键来释放内存
        int retval = freeMemoryIfNeeded();
        // 如果即将要执行的命令可能占用大量内存（REDIS_CMD_DENYOOM）
        // 并且前面的内存释放失败的话，那么向客户端返回内存错误
        if ((c->cmd->flags & REDIS_CMD_DENYOOM) && retval == REDIS_ERR) {
            // TODO: 事务相关
            // flagTransaction(c);
            addReply(c, shared.oomerr);
            return REDIS_OK;
        }
    }

    // TODO: 复制相关
    // 如果这是一个主服务器，并且这个服务器之前持久化时发生了错误，那么不执行写命令
    /* Don't accept write commands if there are problems persisting on disk
     * and if this is a master instance. */
    if (((server.stop_writes_on_bgsave_err && server.saveparamslen > 0 && server.lastbgsave_status == REDIS_ERR) ||
        server.aof_last_write_status == REDIS_ERR) &&
        /* server.masterhost == NULL && */
        (c->cmd->flags & REDIS_CMD_WRITE /* || c->cmd->proc == pingCommand */))
    {
        // TODO: 事务相关
        // flagTransaction(c);
        if (server.aof_last_write_status == REDIS_OK)
            addReply(c, shared.bgsaveerr);
        else
            addReplySds(c,
                        sdscatprintf(sdsempty(),
                                     "-MISCONF Errors writing to the AOF file: %s\r\n",
                                     strerror(server.aof_last_write_errno)));
        return REDIS_OK;
    }

    // TODO: 复制相关
    // 如果服务器没有足够多的状态良好服务器，并且 min-slaves-to-write 选项已打开，那么不执行写命令
    /* Don't accept write commands if there are not enough good slaves and
     * user configured the min-slaves-to-write option. */
    // if (server.repl_min_slaves_to_write &&
    //     server.repl_min_slaves_max_lag &&
    //     c->cmd->flags & REDIS_CMD_WRITE &&
    //     server.repl_good_slaves_count < server.repl_min_slaves_to_write)
    // {
    //     // TODO: 事务相关
    //     // flagTransaction(c);
    //     addReply(c, shared.noreplicaserr);
    //     return REDIS_OK;
    // }

    // TODO: 复制相关
    // 如果这个服务器是一个只读 slave 的话，那么拒绝执行写命令
    /* Don't accept write commands if this is a read only slave. But
     * accept write commands if this is our master. */
    // if (server.masterhost && server.repl_slave_ro &&
    //     !(c->flags & REDIS_MASTER) &&
    //     c->cmd->flags & REDIS_CMD_WRITE)
    // {
    //     addReply(c, shared.roslaveerr);
    //     return REDIS_OK;
    // }

    // TODO: 发布/订阅相关
    // 在订阅与发布模式的上下文中，只能执行订阅和退订相关的命令
    /* Only allow SUBSCRIBE and UNSUBSCRIBE in the context of Pub/Sub */
    // if ((dictSize(c->pubsub_channels) > 0 || listLength(c->pubsub_patterns) > 0) &&
    //     c->cmd->proc != subscribeCommand &&
    //     c->cmd->proc != unsubscribeCommand &&
    //     c->cmd->proc != psubscribeCommand &&
    //     c->cmd->proc != punsubscribeCommand) {
    //     addReplyError(c,"only (P)SUBSCRIBE / (P)UNSUBSCRIBE / QUIT allowed in this context");
    //     return REDIS_OK;
    // }

    // TODO: 复制相关
    /* Only allow INFO and SLAVEOF when slave-serve-stale-data is no and
     * we are a slave with a broken link with master. */
    // if (server.masterhost && server.repl_state != REDIS_REPL_CONNECTED &&
    //     server.repl_serve_stale_data == 0 &&
    //     !(c->cmd->flags & REDIS_CMD_STALE))
    // {
    //     // TODO: 事务相关
    //     // flagTransaction(c);
    //     addReply(c, shared.masterdownerr);
    //     return REDIS_OK;
    // }

    // 正在加载数据库，只能执行带有REDIS_CMD_LOADING标志的命令
    /* Loading DB? Return an error if the command has not the
     * REDIS_CMD_LOADING flag. */
    if (server.loading && !(c->cmd->flags & REDIS_CMD_LOADING)) {
        addReply(c, shared.loadingerr);
        return REDIS_OK;
    }

    // TODO: Lua相关
    // Lua 脚本超时，只允许执行限定的操作，比如 SHUTDOWN 和 SCRIPT KILL
    /* Lua script too slow? Only allow a limited number of commands. */
    // if (server.lua_timedout &&
    //     c->cmd->proc != authCommand &&
    //     c->cmd->proc != replconfCommand &&
    //     !(c->cmd->proc == shutdownCommand &&
    //       c->argc == 2 &&
    //       tolower(((char*)c->argv[1]->ptr)[0]) == 'n') &&
    //     !(c->cmd->proc == scriptCommand &&
    //       c->argc == 2 &&
    //       tolower(((char*)c->argv[1]->ptr)[0]) == 'k'))
    // {
    //     // TODO: 事务相关
    //     // flagTransaction(c);
    //     addReply(c, shared.slowscripterr);
    //     return REDIS_OK;
    // }

    // TODO: 事务相关
    /* Exec the command */
    // if (c->flags & REDIS_MULTI &&
    //     c->cmd->proc != execCommand && c->cmd->proc != discardCommand &&
    //     c->cmd->proc != multiCommand && c->cmd->proc != watchCommand)
    // {
    //     // 在事务上下文中，除 EXEC 、 DISCARD 、 MULTI 和 WATCH 命令之外其他所有命令都会被入队到事务队列中
    //     queueMultiCommand(c);
    //     addReply(c,shared.queued);
    // } else {

    // 执行命令，默认启用慢日志 & 开启统计 & 开启命令传播
    call(c, REDIS_CALL_FULL);

    // TODO: 复制相关
    // c->woff = server.master_repl_offset;

    // TODO: 阻塞相关
    // 处理那些解除了阻塞的键
    // if (listLength(server.ready_keys))
    //     handleClientsBlockedOnLists();

    // }

    return REDIS_OK;
}

/* -----------------------------------------------------------------------------
 * 关闭redis服务器相关API
 * -------------------------------------------------------------------------- */

/*
 * 关闭监听套接字
 */
/* Close listening sockets. Also unlink the unix domain socket if
 * unlink_unix_socket is non-zero. */
void closeListeningSockets(int unlink_unix_socket) {
    int j;

    for (j = 0; j < server.ipfd_count; j++) close(server.ipfd[j]);

    if (server.sofd != -1) close(server.sofd);

    // TODO: 集群相关
    // if (server.cluster_enabled)
    //     for (j = 0; j < server.cfd_count; j++) close(server.cfd[j]);

    if (unlink_unix_socket && server.unixsocket) {
        printf("Removing the unix socket file.\n");
        unlink(server.unixsocket); /* don't care if this fails */
    }
}

/*
 * 执行关闭服务器前的准备工作
 */
int prepareForShutdown(int flags) {
    int save = flags & REDIS_SHUTDOWN_SAVE;
    int nosave = flags & REDIS_SHUTDOWN_NOSAVE;

    printf("User requested shutdown...\n");

    // RDB持久化
    // 如果有 BGSAVE 正在执行，那么杀死子进程，避免竞争条件
    /* Kill the saving child if there is a background saving in progress.
       We want to avoid race conditions, for instance our saving child may
       overwrite the synchronous saving did by SHUTDOWN. */
    if (server.rdb_child_pid != -1) {
        printf("There is a child saving an .rdb. Killing it!\n");
        kill(server.rdb_child_pid,SIGUSR1);
        // rdbRemoveTempFile(server.rdb_child_pid);
    }

    // 同理，杀死正在执行 BGREWRITEAOF 的子进程
    if (server.aof_state != REDIS_AOF_OFF) {
        /* Kill the AOF saving child as the AOF we already have may be longer
         * but contains the full dataset anyway. */
        if (server.aof_child_pid != -1) {
            printf("There is a child rewriting the AOF. Killing it!\n");
            kill(server.aof_child_pid,SIGUSR1);
        }
        /* Append only file: fsync() the AOF and exit */
        printf("Calling fsync() on the AOF file.\n");
        // 将缓冲区的内容写入到硬盘里面
        aof_fsync(server.aof_fd);
    }

    // RDB持久化
    // 如果客户端执行的是 SHUTDOWN save ，或者 SAVE 功能被打开，那么执行 SAVE 操作
    if ((server.saveparamslen > 0 && !nosave) || save) {
        printf("Saving the final RDB snapshot before exiting.\n");
        /* Snapshotting. Perform a SYNC SAVE and exit */
        // if (rdbSave(server.rdb_filename) != REDIS_OK) {
        //     /* Ooops.. error saving! The best we can do is to continue
        //      * operating. Note that if there was a background saving process,
        //      * in the next cron() Redis will be notified that the background
        //      * saving aborted, handling special stuff like slaves pending for
        //      * synchronization... */
        //     printf("Error trying to save the DB, can't exit.\n");
        //     return REDIS_ERR;
        // }
    }

    // 移除 pidfile 文件
    if (server.daemonize) {
        printf("Removing the pid file.\n");
        unlink(server.pidfile);
    }

    // TODO: 哨兵相关
    // 关闭监听套接字，这样在重启的时候会快一点
    /* Close the listening sockets. Apparently this allows faster restarts. */
    closeListeningSockets(1);
    printf("%s is now ready to exit, bye bye...\n",
             /* server.sentinel_mode ? "Sentinel" : */ "Redis");
    return REDIS_OK;
}

/* -----------------------------------------------------------------------------
 * redis服务器初始化相关API
 * -------------------------------------------------------------------------- */

/*
 * 创建共享对象
 */
void createSharedObjects(void) {
    int j;

    // 常用回复
    shared.crlf = createObject(REDIS_STRING,sdsnew("\r\n"));
    shared.ok = createObject(REDIS_STRING,sdsnew("+OK\r\n"));
    shared.err = createObject(REDIS_STRING,sdsnew("-ERR\r\n"));
    shared.emptybulk = createObject(REDIS_STRING,sdsnew("$0\r\n\r\n"));
    shared.czero = createObject(REDIS_STRING,sdsnew(":0\r\n"));
    shared.cone = createObject(REDIS_STRING,sdsnew(":1\r\n"));
    shared.cnegone = createObject(REDIS_STRING,sdsnew(":-1\r\n"));
    shared.nullbulk = createObject(REDIS_STRING,sdsnew("$-1\r\n"));
    shared.nullmultibulk = createObject(REDIS_STRING,sdsnew("*-1\r\n"));
    shared.emptymultibulk = createObject(REDIS_STRING,sdsnew("*0\r\n"));
    shared.pong = createObject(REDIS_STRING,sdsnew("+PONG\r\n"));
    shared.queued = createObject(REDIS_STRING,sdsnew("+QUEUED\r\n"));
    shared.emptyscan = createObject(REDIS_STRING,sdsnew("*2\r\n$1\r\n0\r\n*0\r\n"));
    // 常用错误回复
    shared.wrongtypeerr = createObject(REDIS_STRING,sdsnew(
            "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"));
    shared.nokeyerr = createObject(REDIS_STRING,sdsnew(
            "-ERR no such key\r\n"));
    shared.syntaxerr = createObject(REDIS_STRING,sdsnew(
            "-ERR syntax error\r\n"));
    shared.sameobjecterr = createObject(REDIS_STRING,sdsnew(
            "-ERR source and destination objects are the same\r\n"));
    shared.outofrangeerr = createObject(REDIS_STRING,sdsnew(
            "-ERR index out of range\r\n"));
    shared.noscripterr = createObject(REDIS_STRING,sdsnew(
            "-NOSCRIPT No matching script. Please use EVAL.\r\n"));
    shared.loadingerr = createObject(REDIS_STRING,sdsnew(
            "-LOADING Redis is loading the dataset in memory\r\n"));
    shared.slowscripterr = createObject(REDIS_STRING,sdsnew(
            "-BUSY Redis is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.\r\n"));
    shared.masterdownerr = createObject(REDIS_STRING,sdsnew(
            "-MASTERDOWN Link with MASTER is down and slave-serve-stale-data is set to 'no'.\r\n"));
    shared.bgsaveerr = createObject(REDIS_STRING,sdsnew(
            "-MISCONF Redis is configured to save RDB snapshots, but is currently not able to persist on disk. Commands that may modify the data set are disabled. Please check Redis logs for details about the error.\r\n"));
    shared.roslaveerr = createObject(REDIS_STRING,sdsnew(
            "-READONLY You can't write against a read only slave.\r\n"));
    shared.noautherr = createObject(REDIS_STRING,sdsnew(
            "-NOAUTH Authentication required.\r\n"));
    shared.oomerr = createObject(REDIS_STRING,sdsnew(
            "-OOM command not allowed when used memory > 'maxmemory'.\r\n"));
    shared.execaborterr = createObject(REDIS_STRING,sdsnew(
            "-EXECABORT Transaction discarded because of previous errors.\r\n"));
    shared.noreplicaserr = createObject(REDIS_STRING,sdsnew(
            "-NOREPLICAS Not enough good slaves to write.\r\n"));
    shared.busykeyerr = createObject(REDIS_STRING,sdsnew(
            "-BUSYKEY Target key name already exists.\r\n"));

    // 常用字符
    shared.space = createObject(REDIS_STRING,sdsnew(" "));
    shared.colon = createObject(REDIS_STRING,sdsnew(":"));
    shared.plus = createObject(REDIS_STRING,sdsnew("+"));

    // 常用 SELECT 命令
    for (j = 0; j < REDIS_SHARED_SELECT_CMDS; j++) {
        char dictid_str[64];
        int dictid_len;

        dictid_len = ll2string(dictid_str,sizeof(dictid_str),j);
        shared.select[j] = createObject(REDIS_STRING,
                                        sdscatprintf(sdsempty(),
                                                     "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
                                                     dictid_len, dictid_str));
    }

    // 发布与订阅的有关回复
    shared.messagebulk = createStringObject("$7\r\nmessage\r\n",13);
    shared.pmessagebulk = createStringObject("$8\r\npmessage\r\n",14);
    shared.subscribebulk = createStringObject("$9\r\nsubscribe\r\n",15);
    shared.unsubscribebulk = createStringObject("$11\r\nunsubscribe\r\n",18);
    shared.psubscribebulk = createStringObject("$10\r\npsubscribe\r\n",17);
    shared.punsubscribebulk = createStringObject("$12\r\npunsubscribe\r\n",19);

    // 常用命令
    shared.del = createStringObject("DEL",3);
    shared.rpop = createStringObject("RPOP",4);
    shared.lpop = createStringObject("LPOP",4);
    shared.lpush = createStringObject("LPUSH",5);

    // 常用整数
    for (j = 0; j < REDIS_SHARED_INTEGERS; j++) {
        shared.integers[j] = createObject(REDIS_STRING,(void*)(long)j);
        shared.integers[j]->encoding = REDIS_ENCODING_INT;
    }

    // 常用长度 bulk 或者 multi bulk 回复
    for (j = 0; j < REDIS_SHARED_BULKHDR_LEN; j++) {
        shared.mbulkhdr[j] = createObject(REDIS_STRING,
                                          sdscatprintf(sdsempty(),"*%d\r\n",j));
        shared.bulkhdr[j] = createObject(REDIS_STRING,
                                         sdscatprintf(sdsempty(),"$%d\r\n",j));
    }
    /* The following two shared objects, minstring and maxstrings, are not
     * actually used for their value but as a special object meaning
     * respectively the minimum possible string and the maximum possible
     * string in string comparisons for the ZRANGEBYLEX command. */
    shared.minstring = createStringObject("minstring",9);
    shared.maxstring = createStringObject("maxstring",9);
}


/*
 * SIGTERM信号的处理器
 */
static void sigtermHandler(int sig) {
    REDIS_NOTUSED(sig);

    printf("Received SIGTERM, scheduling shutdown...");

    // 打开关闭标识
    server.shutdown_asap = 1;
}

/*
 * 设置信号处理器
 */
void setupSignalHandlers(void) {
    struct sigaction act;

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used.
     * Otherwise, sa_handler is used. */
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigtermHandler;
    sigaction(SIGTERM, &act, NULL);

// #ifdef HAVE_BACKTRACE
//     sigemptyset(&act.sa_mask);
//     act.sa_flags = SA_NODEFER | SA_RESETHAND | SA_SIGINFO;
//     act.sa_sigaction = sigsegvHandler;
//     sigaction(SIGSEGV, &act, NULL);
//     sigaction(SIGBUS, &act, NULL);
//     sigaction(SIGFPE, &act, NULL);
//     sigaction(SIGILL, &act, NULL);
// #endif

    return;
}

/* This function will try to raise the max number of open files accordingly to
 * the configured max number of clients. It also reserves a number of file
 * descriptors (REDIS_MIN_RESERVED_FDS) for extra operations of
 * persistence, listening sockets, log files and so forth.
 *
 * If it will not be possible to set the limit accordingly to the configured
 * max number of clients, the function will do the reverse setting
 * server.maxclients to the value that we can actually handle. */
void adjustOpenFilesLimit(void) {
    rlim_t maxfiles = server.maxclients+REDIS_MIN_RESERVED_FDS;
    struct rlimit limit;

    if (getrlimit(RLIMIT_NOFILE,&limit) == -1) {
        printf("Unable to obtain the current NOFILE limit (%s), assuming 1024 and setting the max clients configuration accordingly.\n",
                 strerror(errno));
        server.maxclients = 1024-REDIS_MIN_RESERVED_FDS;
    } else {
        rlim_t oldlimit = limit.rlim_cur;

        /* Set the max number of files if the current limit is not enough
         * for our needs. */
        if (oldlimit < maxfiles) {
            rlim_t f;
            int setrlimit_error = 0;

            /* Try to set the file limit to match 'maxfiles' or at least
             * to the higher value supported less than maxfiles. */
            f = maxfiles;
            while(f > oldlimit) {
                int decr_step = 16;

                limit.rlim_cur = f;
                limit.rlim_max = f;
                if (setrlimit(RLIMIT_NOFILE,&limit) != -1) break;
                setrlimit_error = errno;

                /* We failed to set file limit to 'f'. Try with a
                 * smaller limit decrementing by a few FDs per iteration. */
                if (f < decr_step) break;
                f -= decr_step;
            }

            /* Assume that the limit we get initially is still valid if
             * our last try was even lower. */
            if (f < oldlimit) f = oldlimit;

            if (f != maxfiles) {
                int old_maxclients = server.maxclients;
                server.maxclients = f-REDIS_MIN_RESERVED_FDS;
                if (server.maxclients < 1) {
                    printf("Your current 'ulimit -n' "
                                           "of %llu is not enough for Redis to start. "
                                           "Please increase your open file limit to at least "
                                           "%llu. Exiting.\n",
                             (unsigned long long) oldlimit,
                             (unsigned long long) maxfiles);
                    exit(1);
                }
                printf("You requested maxclients of %d "
                                       "requiring at least %llu max file descriptors.\n",
                         old_maxclients,
                         (unsigned long long) maxfiles);
                printf("Redis can't set maximum open files "
                                       "to %llu because of OS error: %s.\n",
                         (unsigned long long) maxfiles, strerror(setrlimit_error));
                printf("Current maximum open files is %llu. "
                                       "maxclients has been reduced to %d to compensate for "
                                       "low ulimit. "
                                       "If you need higher maxclients increase 'ulimit -n'.\n",
                         (unsigned long long) oldlimit, server.maxclients);
            } else {
                printf("Increased maximum number of open files "
                                      "to %llu (it was originally set to %llu).\n",
                         (unsigned long long) maxfiles,
                         (unsigned long long) oldlimit);
            }
        }
    }
}

/*
 * redis服务器开始在指定端口上监听
 */
/* Initialize a set of file descriptors to listen to the specified 'port'
 * binding the addresses specified in the Redis server configuration.
 *
 * The listening file descriptors are stored in the integer array 'fds'
 * and their number is set in '*count'.
 *
 * The addresses to bind are specified in the global server.bindaddr array
 * and their number is server.bindaddr_count. If the server configuration
 * contains no specific addresses to bind, this function will try to
 * bind * (all addresses) for both the IPv4 and IPv6 protocols.
 *
 * On success the function returns REDIS_OK.
 *
 * On error the function returns REDIS_ERR. For the function to be on
 * error, at least one of the server.bindaddr addresses was
 * impossible to bind, or no bind addresses were specified in the server
 * configuration but the function is not able to bind * for at least
 * one of the IPv4 or IPv6 protocols. */
int listenToPort(int port, int *fds, int *count) {
    int j;

    // 0.0.0.0指的是为listenfd绑定服务器上所有ip地址，通过任意一个都能连接到redis服务器
    /* Force binding of 0.0.0.0 if no bind address is specified, always
     * entering the loop if j == 0. */
    if (server.bindaddr_count == 0) server.bindaddr[0] = NULL;
    for (j = 0; j < server.bindaddr_count || j == 0; j++) {
        if (server.bindaddr[j] == NULL) {
            /* Bind * for both IPv6 and IPv4, we enter here only if
             * server.bindaddr_count == 0. */
            fds[*count] = anetTcp6Server(server.neterr,port,NULL,
                                         server.tcp_backlog);
            if (fds[*count] != ANET_ERR) {
                anetNonBlock(NULL,fds[*count]);
                (*count)++;
            }

            // bind -> listen
            fds[*count] = anetTcpServer(server.neterr,port,NULL,
                                        server.tcp_backlog);
            if (fds[*count] != ANET_ERR) {
                // nonblock
                anetNonBlock(NULL,fds[*count]);
                (*count)++;
            }
            /* Exit the loop if we were able to bind * on IPv4 or IPv6,
             * otherwise fds[*count] will be ANET_ERR and we'll print an
             * error and return to the caller with an error. */
            if (*count) break;
        } else if (strchr(server.bindaddr[j],':')) {
            /* Bind IPv6 address. */
            fds[*count] = anetTcp6Server(server.neterr,port,server.bindaddr[j],
                                         server.tcp_backlog);
        } else {
            /* Bind IPv4 address. */
            fds[*count] = anetTcpServer(server.neterr,port,server.bindaddr[j],
                                        server.tcp_backlog);
        }
        if (fds[*count] == ANET_ERR) {
            printf(
                     "Creating Server TCP listening socket %s:%d: %s\n",
                     server.bindaddr[j] ? server.bindaddr[j] : "*",
                     port, server.neterr);
            return REDIS_ERR;
        }
        anetNonBlock(NULL,fds[*count]);
        (*count)++;
    }
    return REDIS_OK;
}

/*
 * 初始化redis服务器的配置信息
 */
void initServerConfig() {

    int j;

    // 对每一个redis服务器，生成惟一的运行时id
    getRandomHexChars(server.runid, REDIS_RUN_ID_SIZE);
    server.runid[REDIS_RUN_ID_SIZE] = '\0';

    // TODO: 配置文件相关
    // server.configfile = NULL;

    // 控制时间事件调用的频率
    server.hz = REDIS_DEFAULT_HZ;

    // 硬件架构32位/64位
    server.arch_bits = (sizeof(long) == 8) ? 64 : 32;

    // 默认监听的端口: 6379
    server.port = REDIS_SERVERPORT;

    // TCP listen监听队列长度
    server.tcp_backlog = REDIS_TCP_BACKLOG;

    // 需要绑定地址的数量
    server.bindaddr_count = 0;

    server.unixsocket = NULL;

    server.unixsocketperm = REDIS_DEFAULT_UNIX_SOCKET_PERM;

    // 初始化监听套接字数量
    server.ipfd_count = 0;

    server.sofd = -1;

    // 数据库数量
    server.dbnum = REDIS_DEFAULT_DBNUM;

    /* server.verbosity = REDIS_DEFAULT_VERBOSITY; */

    // 客户端最大空转时间
    server.maxidletime = REDIS_MAXIDLETIME;

    // tcp探活机制，默认不开启
    server.tcpkeepalive = REDIS_DEFAULT_TCP_KEEPALIVE;

    // 默认启用主动过期删除
    server.active_expire_enabled = 1;

    // 默认允许在serverCron时进行rehash
    server.activerehashing = REDIS_DEFAULT_ACTIVE_REHASHING;

    // 允许客户端的最大查询缓冲区长度
    server.client_max_querybuf_len = REDIS_MAX_QUERYBUF_LEN;

    // RDB持久化的条件
    server.saveparams = NULL;

    // 服务器正在载入的标志
    server.loading = 0;
    server.loading_process_events_interval_bytes = (1024*1024*2);

    // TODO: 日志相关
    /* server.logfile = zstrdup(REDIS_DEFAULT_LOGFILE); */
    /* server.syslog_enabled = REDIS_DEFAULT_SYSLOG_ENABLED; */
    /* server.syslog_ident = zstrdup(REDIS_DEFAULT_SYSLOG_IDENT); */
    /* server.syslog_facility = LOG_LOCAL0; */

    // 服务器是否以守护进程方式启动，默认为否
    server.daemonize = REDIS_DEFAULT_DAEMONIZE;
    server.pidfile = zstrdup(REDIS_DEFAULT_PID_FILE);

    // AOF持久化
    server.aof_state = REDIS_AOF_OFF;
    server.aof_fsync = REDIS_DEFAULT_AOF_FSYNC;
    server.aof_no_fsync_on_rewrite = REDIS_DEFAULT_AOF_NO_FSYNC_ON_REWRITE;
    server.aof_rewrite_perc = REDIS_AOF_REWRITE_PERC;
    server.aof_rewrite_min_size = REDIS_AOF_REWRITE_MIN_SIZE;
    server.aof_rewrite_base_size = 0;
    server.aof_rewrite_scheduled = 0;
    server.aof_last_fsync = time(NULL);
    server.aof_rewrite_time_last = -1;
    server.aof_rewrite_time_start = -1;
    server.aof_lastbgrewrite_status = REDIS_OK;
    server.aof_delayed_fsync = 0;
    server.aof_fd = -1;
    server.aof_selected_db = -1;    /* Make sure the first time will not match */
    server.aof_flush_postponed_start = 0;
    server.aof_rewrite_incremental_fsync = REDIS_DEFAULT_AOF_REWRITE_INCREMENTAL_FSYNC;
    server.aof_filename = zstrdup(REDIS_DEFAULT_AOF_FILENAME);

    // RDB持久化
    server.rdb_filename = zstrdup(REDIS_DEFAULT_RDB_FILENAME);
    server.rdb_compression = REDIS_DEFAULT_RDB_COMPRESSION;
    server.rdb_checksum = REDIS_DEFAULT_RDB_CHECKSUM;
    server.stop_writes_on_bgsave_err = REDIS_DEFAULT_STOP_WRITES_ON_BGSAVE_ERROR;

    // TODO: 认证相关
    // server.requirepass = NULL;

    // TODO: 发布/订阅相关
    // server.notify_keyspace_events = 0;

    // 服务端允许的连接数目上限
    server.maxclients = REDIS_MAX_CLIENTS;

    // TODO: 阻塞相关
    // server.bpop_blocked_clients = 0;

    // 内存达到最大限制时淘汰
    server.maxmemory = REDIS_DEFAULT_MAXMEMORY;
    server.maxmemory_policy = REDIS_DEFAULT_MAXMEMORY_POLICY;
    server.maxmemory_samples = REDIS_DEFAULT_MAXMEMORY_SAMPLES;

    // 底层编码转换
    server.hash_max_ziplist_entries = REDIS_HASH_MAX_ZIPLIST_ENTRIES;
    server.hash_max_ziplist_value = REDIS_HASH_MAX_ZIPLIST_VALUE;
    server.list_max_ziplist_entries = REDIS_LIST_MAX_ZIPLIST_ENTRIES;
    server.list_max_ziplist_value = REDIS_LIST_MAX_ZIPLIST_VALUE;
    server.set_max_intset_entries = REDIS_SET_MAX_INTSET_ENTRIES;
    server.zset_max_ziplist_entries = REDIS_ZSET_MAX_ZIPLIST_ENTRIES;
    server.zset_max_ziplist_value = REDIS_ZSET_MAX_ZIPLIST_VALUE;

    // TODO: 基数统计相关
    /* server.hll_sparse_max_bytes = REDIS_DEFAULT_HLL_SPARSE_MAX_BYTES; */

    // 服务器尽快关闭的标志
    server.shutdown_asap = 0;

    // TODO: 复制相关
    // server.repl_ping_slave_period = REDIS_REPL_PING_SLAVE_PERIOD;
    // server.repl_timeout = REDIS_REPL_TIMEOUT;
    // server.repl_min_slaves_to_write = REDIS_DEFAULT_MIN_SLAVES_TO_WRITE;
    // server.repl_min_slaves_max_lag = REDIS_DEFAULT_MIN_SLAVES_MAX_LAG;

    // TODO: 集群相关
    // server.cluster_enabled = 0;
    // server.cluster_node_timeout = REDIS_CLUSTER_DEFAULT_NODE_TIMEOUT;
    // server.cluster_migration_barrier = REDIS_CLUSTER_DEFAULT_MIGRATION_BARRIER;
    // server.cluster_configfile = zstrdup(REDIS_DEFAULT_CLUSTER_CONFIG_FILE);

    // TODO: LUA相关
    // server.lua_caller = NULL;
    // server.lua_time_limit = REDIS_LUA_TIME_LIMIT;
    // server.lua_client = NULL;
    // server.lua_timedout = 0;

    // TODO: 集群相关
    /* server.migrate_cached_sockets = dictCreate(&migrateCacheDictType,NULL); */

    // 服务器缓存的lru时间
    server.lruclock = getLRUClock();

    // RDB持久化
    resetServerSaveParams();
    appendServerSaveParams(60 * 60, 1);
    appendServerSaveParams(300, 100);
    appendServerSaveParams(60, 10000);

    // TODO: 复制相关
    // 初始化和复制相关的状态
    /* Replication related */
    // server.masterauth = NULL;
    // server.masterhost = NULL;
    // server.masterport = 6379;
    // server.master = NULL;
    // server.cached_master = NULL;
    // server.repl_master_initial_offset = -1;
    // server.repl_state = REDIS_REPL_NONE;
    // server.repl_syncio_timeout = REDIS_REPL_SYNCIO_TIMEOUT;
    // server.repl_serve_stale_data = REDIS_DEFAULT_SLAVE_SERVE_STALE_DATA;
    // server.repl_slave_ro = REDIS_DEFAULT_SLAVE_READ_ONLY;
    // server.repl_down_since = 0; /* Never connected, repl is down since EVER. */
    // server.repl_disable_tcp_nodelay = REDIS_DEFAULT_REPL_DISABLE_TCP_NODELAY;
    // server.slave_priority = REDIS_DEFAULT_SLAVE_PRIORITY;
    // server.master_repl_offset = 0;
    // 初始化 PSYNC 命令所使用的 backlog
    /* Replication partial resync backlog */
    // server.repl_backlog = NULL;
    // server.repl_backlog_size = REDIS_DEFAULT_REPL_BACKLOG_SIZE;
    // server.repl_backlog_histlen = 0;
    // server.repl_backlog_idx = 0;
    // server.repl_backlog_off = 0;
    // server.repl_backlog_time_limit = REDIS_DEFAULT_REPL_BACKLOG_TIME_LIMIT;
    // server.repl_no_slaves_since = time(NULL);

    // 客户端输出缓冲区长度限制
    /* Client output buffer limits */
    for (j = 0; j < REDIS_CLIENT_LIMIT_NUM_CLASSES; j++)
        server.client_obuf_limits[j] = clientBufferLimitsDefaults[j];

    // double类型常量初始化
    /* Double constants initialization */
    R_Zero = 0.0;
    R_PosInf = 1.0/R_Zero;
    R_NegInf = -1.0/R_Zero;
    R_Nan = R_Zero/R_Zero;

    // 命令表初始化
    /* Command table -- we initiialize it here as it is part of the
     * initial configuration, since command names may be changed via
     * redis.conf using the rename-command directive. */
    server.commands = dictCreate(&commandTableDictType, NULL);
    server.orig_commands = dictCreate(&commandTableDictType, NULL);
    populateCommandTable();
    server.delCommand = lookupCommandByCString("del");
    server.multiCommand = lookupCommandByCString("multi");
    server.lpushCommand = lookupCommandByCString("lpush");
    server.lpopCommand = lookupCommandByCString("lpop");
    server.rpopCommand = lookupCommandByCString("rpop");

    // TODO: 慢查询相关
    // 初始化慢查询日志
    /* Slow log */
    // server.slowlog_log_slower_than = REDIS_SLOWLOG_LOG_SLOWER_THAN;
    // server.slowlog_max_len = REDIS_SLOWLOG_MAX_LEN;

    // TODO: 调试相关
    // 初始化调试项
    /* Debugging */
    // server.assert_failed = "<no assertion failed>";
    // server.assert_file = "<no file>";
    // server.assert_line = 0;
    // server.bug_report_start = 0;
    // server.watchdog_period = 0;
}

/*
 * 重置服务器状态统计信息
 */
/* Resets the stats that we expose via INFO or other means that we want
 * to reset via CONFIG RESETSTAT. The function is also used in order to
 * initialize these fields in initServer() at server startup. */
void resetServerStats(void) {
    server.stat_numcommands = 0;
    server.stat_numconnections = 0;
    server.stat_expiredkeys = 0;
    server.stat_evictedkeys = 0;
    server.stat_keyspace_misses = 0;
    server.stat_keyspace_hits = 0;
    server.stat_fork_time = 0;
    server.stat_rejected_conn = 0;
    server.stat_sync_full = 0;
    server.stat_sync_partial_ok = 0;
    server.stat_sync_partial_err = 0;
    memset(server.ops_sec_samples,0,sizeof(server.ops_sec_samples));
    server.ops_sec_idx = 0;
    server.ops_sec_last_sample_time = mstime();
    server.ops_sec_last_sample_ops = 0;
}

/*
 * 初始化redis服务器
 */
void initServer() {
    int j;

    // 设置信号处理函数
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setupSignalHandlers();

    // 初始化服务器的客户端结构
    server.current_client = NULL;
    server.clients = listCreate();
    server.clients_to_close = listCreate();

    // TODO: 复制相关
    // server.slaves = listCreate();
    // server.slaveseldb = -1;    /* Force to emit the first SELECT command. */
    // server.clients_waiting_acks = listCreate();
    // server.get_ack_from_slaves = 0;

    // TODO: 监视器相关
    // server.monitors = listCreate();

    // TODO: 阻塞相关
    // server.unblocked_clients = listCreate();
    // server.ready_keys = listCreate();

    // TODO: 客户端停止相关
    // server.clients_paused = 0;

    // 创建共享对象
    createSharedObjects();
    // 调整打开文件限制
    adjustOpenFilesLimit();
    // 创建并初始化事件循环处理器
    server.el = aeCreateEventLoop(server.maxclients + REDIS_EVENTLOOP_FDSET_INCR);
    // 创建数据库
    server.db = zmalloc(sizeof(redisDb) * server.dbnum);

    // 开启TCP端口监听
    /* Open the TCP listening socket for the user commands. */
    if (server.port != 0 && listenToPort(server.port, server.ipfd, &server.ipfd_count) == REDIS_ERR)
        exit(1);

    /* Open the listening Unix domain socket. */
    if (server.unixsocket != NULL) {
        unlink(server.unixsocket);    /* don't care if this fails */
        server.sofd = anetUnixServer(server.neterr, server.unixsocket, server.unixsocketperm, server.tcp_backlog);
        if (server.sofd == ANET_ERR) {
            printf("Opening socket: %s\n", server.neterr);
            exit(1);
        }
        anetNonBlock(NULL, server.sofd);
    }

    /* Abort if there are no listening sockets at all. */
    if (server.ipfd_count == 0 && server.sofd < 0) {
        printf("Configured to not listen anywhere, exiting.");
        exit(1);
    }

    // 初始化数据库状态
    /* Create the Redis databases, and initialize other internal state. */
    for (j = 0; j < server.dbnum; j++) {
        server.db[j].dict = dictCreate(&dbDictType, NULL);
        server.db[j].expires = dictCreate(&keyptrDictType, NULL);

        // TODO: 阻塞相关
        // server.db[j].blocking_keys = dictCreate(&keylistDictType,NULL);
        // server.db[j].ready_keys = dictCreate(&setDictType,NULL);
        // TODO: 事务相关
        // server.db[j].watched_keys = dictCreate(&keylistDictType,NULL);

        server.db[j].eviction_pool = evictionPoolAlloc();
        server.db[j].id = j;
        server.db[j].avg_ttl = 0;
    }

    // TODO: 发布/订阅相关
    // 创建 PUBSUB 相关结构
    // server.pubsub_channels = dictCreate(&keylistDictType,NULL);
    // server.pubsub_patterns = listCreate();
    // listSetFreeMethod(server.pubsub_patterns,freePubsubPattern);
    // listSetMatchMethod(server.pubsub_patterns,listMatchPubsubPattern);

    // 初始化serverCron函数的运行次数计数器
    server.cronloops = 0;

    // AOF持久化 & RDB持久化
    server.rdb_child_pid = -1;
    server.aof_child_pid = -1;
    aofRewriteBufferReset();
    server.aof_buf = sdsempty();
    server.lastsave = time(NULL);    /* At startup we consider the DB saved. */
    server.lastbgsave_try = 0;             /* At startup we never tried to BGSAVE. */
    server.rdb_save_time_last = -1;
    server.rdb_save_time_start = -1;

    // 统计信息初始化
    server.dirty = 0;
    resetServerStats();
    /* A few stats we don't want to reset: server startup time, and peak mem. */
    server.stat_starttime = time(NULL);
    server.stat_peak_memory = 0;
    server.resident_set_size = 0;
    server.lastbgsave_status = REDIS_OK;
    server.aof_last_write_status = REDIS_OK;
    server.aof_last_write_errno = 0;

    // TODO: 复制相关
    // server.repl_good_slaves_count = 0;

    // 初始化服务器缓存的时间
    updateCachedTime();

    // 创建serverCron时间事件
    /* Create the serverCron() time event, that's our main way to process
     * background operations. */
    if (aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL) == AE_ERR) {
        printf("Can't create the serverCron time event.");
        exit(1);
    }

    // 为监听描述符的读事件绑定acceptTcpHandler处理器
    /* Create an event handler for accepting new connections in TCP and Unix
     * domain sockets. */
    for (j = 0; j < server.ipfd_count; j++) {
        if (aeCreateFileEvent(server.el, server.ipfd[j], AE_READABLE, acceptTcpHandler, NULL) == AE_ERR) {
            printf("Unrecoverable error creating server.ipfd file event.");
            exit(1);
        }
    }

    if (server.sofd > 0 && aeCreateFileEvent(server.el, server.sofd, AE_READABLE, acceptUnixHandler, NULL) == AE_ERR) {
        printf("Unrecoverable error creating server.sofd file event.");
        exit(1);
    }

    /* Open the AOF file if needed. */
    if (server.aof_state == REDIS_AOF_ON) {
        server.aof_fd = open(server.aof_filename, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (server.aof_fd == -1) {
            printf("Can't open the append-only file: %s\n", strerror(errno));
            exit(1);
        }
    }

    // 对于 32 位实例来说，默认将最大可用内存限制在 3 GB
    /* 32 bit instances are limited to 4GB of address space, so if there is
     * no explicit limit in the user provided configuration we set a limit
     * at 3 GB using maxmemory with 'noeviction' policy'. This avoids
     * useless crashes of the Redis instance for out of memory. */
    if (server.arch_bits == 32 && server.maxmemory == 0) {
        printf("Warning: 32 bit instance detected but no memory limit set. Setting 3 GB maxmemory limit with 'noeviction' policy now.");
        server.maxmemory = 3072LL*(1024*1024); /* 3 GB */
        server.maxmemory_policy = REDIS_MAXMEMORY_NO_EVICTION;
    }

    // TODO: 集群相关
    // 如果服务器以 cluster 模式打开，那么初始化 cluster
    // if (server.cluster_enabled) clusterInit();

    // TODO: 复制相关
    // 初始化复制功能有关的脚本缓存
    // replicationScriptCacheInit();

    // TODO: LUA相关
    // 初始化脚本系统
    // scriptingInit();

    // TODO: 慢查询相关
    // 初始化慢查询日志功能
    // slowlogInit();

    // 初始化 BIO 系统
    bioInit();
}

/*
 * 载入RDB或AOF文件
 */
/* Function called at startup to load RDB or AOF file in memory. */
void loadDataFromDisk(void) {
    // 记录开始时间
    long long start = ustime();

    // AOF 持久化已打开
    if (server.aof_state == REDIS_AOF_ON) {
        // 尝试载入 AOF 文件
        if (loadAppendOnlyFile(server.aof_filename) == REDIS_OK)
            // 打印载入信息，并计算载入耗时长度
            printf("DB loaded from append only file: %.3f seconds\n",(float)(ustime()-start)/1000000);
    // AOF 持久化未打开
    } else {
        // RDB持久化
        // 尝试载入 RDB 文件
        // if (rdbLoad(server.rdb_filename) == REDIS_OK) {
        //     // 打印载入信息，并计算载入耗时长度
        //     printf("DB loaded from disk: %.3f seconds\n", (float)(ustime()-start)/1000000);
        // } else if (errno != ENOENT) {
        //     printf("Fatal error loading the DB: %s. Exiting.\n",strerror(errno));
        //     exit(1);
        // }
    }
}

/*
 * 处理下一轮事件前，需要执行的函数
 */
void beforeSleep(struct aeEventLoop* eventLoop) {
    REDIS_NOTUSED(eventLoop);

    // TODO: 复制相关
    // 执行ACTIVE_EXPIRE_CYCLE_FAST模式的主动删除
    /* Run a fast expire cycle (the called function will return
     * ASAP if a fast cycle is not needed). */
    if (server.active_expire_enabled /* && server.masterhost == NULL */)
        activeExpireCycle(ACTIVE_EXPIRE_CYCLE_FAST);

    // TODO: 复制相关
    /* Send all the slaves an ACK request if at least one client blocked
     * during the previous event loop iteration. */
    // if (server.get_ack_from_slaves) {
    //     robj *argv[3];
    // 
    //     argv[0] = createStringObject("REPLCONF",8);
    //     argv[1] = createStringObject("GETACK",6);
    //     argv[2] = createStringObject("*",1); /* Not used argument. */
    //     replicationFeedSlaves(server.slaves, server.slaveseldb, argv, 3);
    //     decrRefCount(argv[0]);
    //     decrRefCount(argv[1]);
    //     decrRefCount(argv[2]);
    //     server.get_ack_from_slaves = 0;
    // }

    // TODO: 阻塞相关
    /* Unblock all the clients blocked for synchronous replication
     * in WAIT. */
    // if (listLength(server.clients_waiting_acks))
    //     processClientsWaitingReplicas();

    /* Try to process pending commands for clients that were just unblocked. */
    // if (listLength(server.unblocked_clients))
    //     processUnblockedClients();

    // AOF持久化
    /* Write the AOF buffer on disk */
    // 将 AOF 缓冲区的内容写入到 AOF 文件
    flushAppendOnlyFile(0);

    // TODO: 集群相关
    // 在进入下个事件循环前，执行一些集群收尾工作
    /* Call the Redis Cluster before sleep function. */
    // if (server.cluster_enabled) clusterBeforeSleep();
}

/*
 * 内存不足的处理函数
 */
void redisOutOfMemoryHandler(size_t allocation_size) {
    exit(1);
}

int main(int argc, char** argv) {
    struct timeval tv;

    setlocale(LC_COLLATE, "");
    zmalloc_enable_thread_safeness();
    zmalloc_set_oom_handler(redisOutOfMemoryHandler);
    srand(time(NULL) ^ getpid());
    gettimeofday(&tv, NULL);
    dictSetHashFunctionSeed(tv.tv_sec ^ tv.tv_usec ^ getpid());

    // TODO: 哨兵相关
    // server.sentinel_mode = checkForSentinelMode(argc, argv);

    // 初始化服务器配置
    initServerConfig();

    // TODO: 哨兵相关
    // if (server.sentinel_mode) {
    //     initSentinelConfig();
    //     initSentinel();
    // }

    // TODO: 守护进程相关
    // if (server.daemonize) daemonize();

    // 初始化服务器
    initServer();

    // TODO: 守护进程相关
    /* if (server.daemonize) createPidFile(); */

    /* redisSetProcTitle(argv[0]); */

    /* redisAsciiArt(); */

    // TODO: 哨兵相关
    // if (!server.sentinel_mode) {
        printf("Server started, Redis version %s\n", REDIS_VERSION);

        // 加载AOF或RDB文件
        loadDataFromDisk();

        if (server.ipfd_count > 0)
            printf("The server is now ready to accept connections on port %d\n", server.port);
        if (server.sofd > 0)
            printf("The server is now ready to accept connections on port %s\n", server.unixsocket);
    // }

    // 设置每一次事件处理器循环之前需要执行的函数
    aeSetBeforeSleepProc(server.el, beforeSleep);

    // 开启事件处理器循环
    aeMain(server.el);

    // 删除事件处理器
    aeDeleteEventLoop(server.el);

    return 0;
}
