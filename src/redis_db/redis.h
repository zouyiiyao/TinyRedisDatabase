//
// Created by zouyi on 2021/8/31.
//

#ifndef TINYREDIS_REDIS_H
#define TINYREDIS_REDIS_H

#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>

#include "ae.h"
#include "anet.h"
#include "redis_obj.h"
#include "zmalloc.h"
#include "utils.h"
#include "sds.h"
#include "adlist.h"
#include "ziplist.h"
#include "zskiplist.h"
#include "dict.h"
#include "intset.h"

/* 执行函数的状态码 */
/* Error codes */
#define REDIS_OK 0
#define REDIS_ERR -1

/* 默认的服务器配置 */
/* Static server configuration */
#define REDIS_RUN_ID_SIZE 40
#define REDIS_OPS_SEC_SAMPLES 16
#define REDIS_BINDADDR_MAX 16
#define REDIS_SHARED_INTEGERS 10000

/* Protocol and I/O related defines */
#define REDIS_REPLY_CHUNK_BYTES (16*1024)    /* 16k output buffer */
#define REDIS_LONGSTR_SIZE 21                /* Bytes needed for long -> str */

/* 字典负载参数，小于该值则缩小字典 */
/* Hash table parameters */
#define REDIS_HT_MINFILL 10

/* 对象类型 */
/* Object types */
#define REDIS_STRING 0    /* 字符串类型 */
#define REDIS_LIST 1      /* 列表类型 */
#define REDIS_SET 2       /* 集合类型 */
#define REDIS_ZSET 3      /* 有序集合类型 */
#define REDIS_HASH 4      /* 哈希类型 */

/* 对象编码 */
/* Object encoding */
#define REDIS_ENCODING_RAW 0           /* 底层sds */
#define REDIS_ENCODING_INT 1           /* 底层long */
#define REDIS_ENCODING_HT 2            /* 底层字典 */
#define REDIS_ENCODING_ZIPMAP 3
#define REDIS_ENCODING_LINKEDLIST 4    /* 底层双向链表 */
#define REDIS_ENCODING_ZIPLIST 5       /* 底层压缩列表 */ 
#define REDIS_ENCODING_INTSET 6        /* 底层整数集合 */
#define REDIS_ENCODING_SKIPLIST 7      /* 底层跳表&字典 */
#define REDIS_ENCODING_EMBSTR 8        /* 底层sds */

/* 列表方向 */
/* List related stuff */
#define REDIS_HEAD 0
#define REDIS_TAIL 1

/*
 * 时钟最大值&时钟精度 
 */
#define REDIS_LRU_CLOCK_MAX ((1<<REDIS_LRU_BITS)-1)
#define REDIS_LRU_CLOCK_RESOLUTION 1000

/*
 * 更换底层编码有关的宏
 */
#define HASH_MAX_ZIPLIST_ENTRIES 512
#define HASH_MAX_ZIPLIST_VALUE 64
#define LIST_MAX_ZIPLIST_ENTRIES 512
#define LIST_MAX_ZIPLIST_VALUE 64
#define SET_MAX_INTSET_ENTRIES 512

/* Anti-warning macro */
#define REDIS_NOTUSED(V) ((void) V)

/*
 * 根据客户端类型不同可以有多种客户端缓冲区限制
 */
/* Client classes for client limits, currently used only for
 * the max-client-output-buffer limit implementation. */
#define REDIS_CLIENT_LIMIT_CLASS_NORMAL 0
#define REDIS_CLIENT_LIMIT_CLASS_SLAVE 1
#define REDIS_CLIENT_LIMIT_CLASS_PUBSUB 2
#define REDIS_CLIENT_LIMIT_NUM_CLASSES 3

/*
 * 时钟精度，秒或毫秒
 */
/* Units */
#define UNIT_SECONDS 0
#define UNIT_MILLISECONDS 1

/*
 * SHUTDOWN标志
 */
/* SHUTDOWN flags */
#define REDIS_SHUTDOWN_SAVE 1
#define REDIS_SHUTDOWN_NOSAVE 2

/*
 * AOF状态
 */
/* AOF states */
#define REDIS_AOF_OFF 0             /* AOF is off */
#define REDIS_AOF_ON 1              /* AOF is on */
#define REDIS_AOF_WAIT_REWRITE 2    /* AOF waits rewrite to start appending */


/*
 * 有序集合结构体定义
 */
typedef struct zset {

    dict* dict;

    zskiplist* zsl;

} zset;

/*
 * 有序集合，表示开区间/闭区间范围的结构
 */
typedef struct {

    double min, max;

    int minex, maxex;
} zrangespec;

/*
 * 列表类型迭代器结构体定义
 */
typedef struct {

    robj* subject;

    unsigned char encoding;

    unsigned char direction;

    unsigned char* zi;

    listNode* ln;

} listTypeIterator;

/*
 * 迭代列表时使用的记录结构，用于保存迭代器，以及迭代器返回的列表节点
 */
typedef struct {

    listTypeIterator* li;

    unsigned char* zi;

    listNode* ln;

} listTypeEntry;

/*
 * 集合类型迭代器结构体定义
 */
typedef struct {

    robj* subject;

    int encoding;

    int ii;

    dictIterator* di;

} setTypeIterator;

/*
 * 哈希类型迭代器结构体定义
 */
typedef struct {

    robj* subject;

    int encoding;

    unsigned char* fptr;

    unsigned char* vptr;

    dictIterator* di;

    dictEntry* de;

} hashTypeIterator;

#define REDIS_HASH_KEY 1
#define REDIS_HASH_VALUE 2

/*
 * 客户端缓冲区限制
 */
typedef struct clientBufferLimitsConfig {

    // 硬限制
    unsigned long long hard_limit_bytes;

    // 软限制
    unsigned long long soft_limit_bytes;

    // 软限制时限
    time_t soft_limit_seconds;

} clientBufferLimitsConfig;

// 多个客户端缓冲区限制
extern clientBufferLimitsConfig clientBufferLimitsDefaults[REDIS_CLIENT_LIMIT_NUM_CLASSES];

/*
 * 定义一个redis操作的结构体
 *
 * 目前只用于在传播被执行命令之后，传播附加的其他命令到AOF或Replication中
 */
typedef struct redisOp {

    // 参数
    robj** argv;

    // 参数数量
    int argc;

    // 数据库id
    int dbid;

    // 传播目标
    int target;

    // 被执行命令的指针
    struct redisCommand* cmd;

} redisOp;

/* Defines an array of Redis operations. There is an API to add to this
 * structure in a easy way.
 *
 * redisOpArrayInit();
 * redisOpArrayAppend();
 * redisOpArrayFree();
 */
typedef struct redisOpArray {

    redisOp* ops;

    int numops;

} redisOpArray;

/*
 * millisecond time type
 */
typedef long long mstime_t;

/* To improve the quality of the LRU approximation we take a set of keys
 * that are good candidate for eviction across freeMemoryIfNeeded() calls.
 *
 * Entries inside the eviciton pool are taken ordered by idle time, putting
 * greater idle times to the right (ascending order).
 *
 * Empty entries have the key pointer set to NULL. */
#define REDIS_EVICTION_POOL_SIZE 16
struct evictionPoolEntry {
    unsigned long long idle;    /* Object idle time */
    sds key;                    /* Key name */
};

/*
 * 通过复用来减少内存碎片，以及减少操作耗时的共享对象
 */
struct sharedObjectsStruct {
    robj* crlf;
    robj* ok;
    robj* err;
    robj* czero;
    robj* cone;
    robj* colon;
    robj* cnegone;
    robj* nullbulk;
    robj* emptymultibulk;
    robj* syntaxerr;
    robj* sameobjecterr;
    robj* nokeyerr;
    robj* outofrangeerr;
    robj* wrongtypeerr;
    robj* del;
    robj* rpop;
    robj* lpop;
    robj* lpush;
    robj* integers[REDIS_SHARED_INTEGERS];
};

/*
 * Redis数据库结构体定义
 */
/* Redis database representation. There are multiple databases identified
 * by integers from 0 (the default database) up to the max configured
 * database. The database number is the 'id' field in the structure. */
typedef struct redisDb {

    // 数据库键空间
    dict* dict;       /* The keyspace for this DB */

    // 过期字典
    dict* expires;    /* Timeout of keys with a timeout set */

    dict* blocking_keys;

    dict* ready_keys;

    dict* watched_keys;

    struct evictionPoolEntry* eviction_pool;

    int id;           /* Database ID */

    long long avg_ttl;

} redisDb;

typedef struct multiCmd {

    robj** argv;

    int argc;

    struct redisCommand* cmd;

} multiCmd;

typedef struct multiState {

    multiCmd* commands;

    int count;

    int minreplicas;

    time_t minreplicas_timeout;

} multiState;

typedef struct blockingState {

    mstime_t timeout;

    dict* keys;

    robj* target;

    int numreplicas;

    long long reploffset;

} blockingState;

/*
 * 因为I/O多路复用的缘故，需要为每个客户端维护一个状态，多个客户端状态被服务器用链表连接起来
 */
/* With multiplexing we need to take per-client state.
 * Clients are taken in a liked list.
 */
typedef struct redisClient {

    // 套接字描述符
    int fd;

    // 客户端当前使用的目标数据库指针
    redisDb* db;

    // 客户端当前使用的目标数据库id
    int dictid;

    // 客户端名字
    robj* name;

    // 查询缓冲区
    sds querybuf;

    // 查询缓冲区长度峰值
    size_t querybuf_peak;   /* Recent (100ms or more) peak of querybuf size */

    // 参数数目
    int argc;

    // 参数
    robj** argv;

    // 被客户端执行的命令，不完全类型，在后面定义
    /* uncompleted type */
    struct redisCommand* cmd;

    struct redisCommand* lastcmd;
  
    // 请求类型
    int reqtype;

    // 剩余未读取的命令内容数量
    int multibulklen;       /* number of multi bulk arguments left to read */

    // 命令内容的长度
    long bulklen;           /* length of bulk argument in multi bulk request */

    // 回复链表
    list* reply;

    // 回复链表中对象的总字节数
    unsigned long reply_bytes;

    // 已发送字节
    int sentlen;

    // 创建客户端的时间
    time_t ctime;

    // 客户端最后一次和服务器互动的时间
    time_t lastinteraction;

    // 客户端的输出缓冲区超过软性限制的时间
    time_t obuf_soft_limit_reached_time;

    // 客户端状态标志
    int flags;              /* REDIS_SLAVE | REDIS_MONITOR | REDIS_MULTI ... */

    // 客户端认证状态
    int authenticated;      /* when requirepass is non-NULL */

    // 复制状态
    int replstate;

    // 用于保存主服务器传来的RDB文件的文件描述符
    int repldbfd;

    // 读取主服务器传来的RDB文件的偏移量
    off_t repldboff;

    // 主服务器传来的RDB文件的大小
    off_t repldbsize;

    sds replpreamble;

    // 主服务器的复制偏移量
    long long reploff;

    // 从服务器最后一次发送REPLCONF ACK时的偏移量
    long long repl_ack_off;

    // 从服务器最后一次发送REPLCONF ACK的时间
    long long repl_ack_time;

    // 主服务器的运行id，保存在客户端，用于执行部分重同步
    char replrunid[REDIS_RUN_ID_SIZE + 1];

    // 从服务器的监听端口号
    int slave_listening_port;

    // 事务状态
    multiState mstate;

    // 阻塞类型
    int btype;

    // 阻塞状态
    blockingState bpop;

    // 最后被写入的全局复制偏移量
    long long woff;

    // 被监视的键
    list* watched_keys;

    // 记录客户端所有订阅的频道的集合
    dict* pubsub_channels;

    // 记录所有订阅频道的客户端信息的链表
    list* pubsub_patterns;

    sds peerid;

    // 回复偏移量
    int bufpos;

    // 回复缓冲区
    char buf[REDIS_REPLY_CHUNK_BYTES];

} redisClient;

/*
 * 服务器的保存条件: BGSAVE自动执行的条件
 */
struct saveparam {

    // 多少秒之内
    time_t seconds;

    // 发生多少次修改
    int changes;
};

/*
 * Redis command
 */
typedef void redisCommandProc(redisClient* c);
typedef int* redisGetKeysProc(struct redisCommand* cmd, robj** argv, int argc, int* numkeys);

/*
 * Redis命令
 */
struct redisCommand {

    // 命令名字
    char* name;

    // 实现函数
    redisCommandProc* proc;

    // 参数个数
    int arity;

    // 字符串表示的flags
    char* sflags;

    // 实际的flags
    int flags;

    /* Use a function to determine keys arguments in a command line.
     * Used for Redis Cluster redirect. */
    redisGetKeysProc* getkeys_proc;

    /* What keys should be loaded in background when calling this command? */

    int firstkey;    /* The first argument that's a key (0 = no keys) */

    int lastkey;     /* The last argument that's a key */

    int keystep;     /* The step between first and last key */

    // 统计信息，记录命令执行耗费的总时间
    long long microseconds;

    // 统计信息，记录命令被执行的总次数
    long long calls;
};

/*
 * Global server state
 */

struct clusterState;

/* struct lua_State; */

struct redisServer {

    /* 通用字段 */
    /* General */

    // 配置文件的绝对路径
    char* configfile;

    // serverCron每秒调用的次数
    int hz;

    // 一个数组，保存着服务器中的所有数据库
    redisDb* db;

    // 命令表
    dict* commands;

    dict* origin_commands;

    // 事件状态
    aeEventLoop* el;

    // 最近一次使用时钟
    unsigned lruclock:REDIS_LRU_BITS;

    // 关闭服务器的标识
    int shutdown_asap;

    // 在执行serverCron时进行渐进式rehash
    int activerehashing;

    // 是否设置了密码
    char* requirepass;

    // PID文件路径
    char* pidfile;

    // 架构类型
    int arch_bits;

    // serverCron函数的运行次数计数器
    int cronloops;

    // 本服务器的运行id
    char runid[REDIS_RUN_ID_SIZE + 1];

    // 服务器是否运行在sentinel模式
    int sentinel_mode;

    /* 网络字段 */
    /* Networking */

    // TCP监听端口
    int port;

    int tcp_backlog;

    // 地址
    char* bindaddr[REDIS_BINDADDR_MAX];

    // 地址数量
    int bindaddr_count;

    // UNIX套接字
    char* unixsocket;

    mode_t unixsocketperm;

    // TCP套接字描述符
    int ipfd[REDIS_BINDADDR_MAX];

    int ipfd_count;

    int sofd;

    int cfd[REDIS_BINDADDR_MAX];

    int cfd_count;

    // 保存所有客户端状态的链表
    list* clients;

    // 保存所有待关闭客户端状态的链表
    list* clients_to_close;

    // 保存所有从服务器的链表
    list* slaves;

    // 保存所有监视器的链表
    list* monitors;

    // 服务器的当前客户端，仅用于崩溃的报告
    redisClient* current_client;

    int clients_paused;

    mstime_t clients_pause_end_time;

    // 网络错误
    char neterr[ANET_ERR_LEN];

    dict* migrate_cached_sockets;

    /* RDB/AOF载入信息 */
    /* RDB / AOF loading information */

    // 该值为真时，表示服务器正在进行载入
    int loading;

    // 正在载入的数据的大小
    off_t loading_total_bytes;

    // 已经载入的数据的大小
    off_t loading_loaded_bytes;

    // 开始进行载入的时间
    time_t loading_start_time;

    off_t loading_process_events_interval_bytes;

    /* 常用命令的快捷链接 */
    /* Fast pointers to often looked up command */

    struct redisCommand* delCommand;
    struct redisCommand* multiCommand;
    struct redisCommand* lpushCommand;
    struct redisCommand* lpopCommand;
    struct redisCommand* rpopCommand;

    /* 统计信息 */
    /* Fields used only for stats */

    // 服务器启动时间
    time_t stat_starttime;

    // 已处理命令的数量
    long long stat_numcommands;

    // 服务器接到的连接请求数量
    long long stat_numconnections;

    // 已过期键的数量
    long long stat_expiredkeys;

    // 因为回收内存而被释放的键的数量
    long long stat_evictedkeys;

    // 键命中次数
    long long stat_keyspace_hits;

    // 键未命中次数
    long long stat_keyspace_misses;

    // 已使用内存峰值
    size_t stat_peak_memory;

    // 最后一次执行fork消耗的时间
    long long stat_fork_time;

    // 服务器因为客户端数量过多而拒绝客户端连接的次数
    long long stat_rejected_conn;

    // 执行full sync的次数
    long long stat_sync_full;

    // psync成功执行的次数
    long long stat_sync_partial_ok;

    // psync失败的次数
    long long stat_sync_partial_err;

    /* 慢日志 */
    /* slowlog */

    list* slowlog;

    long long slowlog_entry_id;

    long long slowlog_log_slower_than;

    unsigned long slowlog_max_len;

    size_t resident_set_size;

    long long ops_sec_last_sample_time;

    long long ops_sec_last_sample_ops;

    long long ops_sec_samples[REDIS_OPS_SEC_SAMPLES];

    int ops_sec_idx;

    /* 配置字段 */
    /* Configuration */

    // 日志可见性
    int verbosity;

    // 客户端最大空转时间
    int maxidletime;

    // 是否开启SO_KEEPALIVE
    int tcpkeepalive;

    int active_expire_enabled;

    // 客户端最大查询缓冲区长度
    size_t client_max_querybuf_len;

    // 服务器数据库总数目
    int dbnum;

    int daemonize;

    // 客户端输出缓冲区大小限制，每一类客户端有不同的限制
    clientBufferLimitsConfig client_obuf_limits[REDIS_CLIENT_LIMIT_NUM_CLASSES];

    /* AOF持久化字段 */
    /* AOF persistence */

    // AOF状态: 开启，关闭，可写
    int aof_state;

    // 使用的fsync策略: 总是同步，每秒同步，从不同步
    int aof_fsync;

    // aof文件名
    char* aof_filename;

    int aof_no_fsync_on_rewrite;

    int aof_rewrite_perc;

    off_t aof_rewrite_min_size;

    off_t aof_rewrite_base_size;

    off_t aof_current_size;

    int aof_rewrite_scheduled;

    // 负责进行AOF重写的子进程PID
    pid_t aof_child_pid;

    list* aof_rewrite_buf_blocks;

    sds aof_buf;

    int aof_fd;

    int aof_selected_db;

    time_t aof_flush_postponed_start;

    time_t aof_last_fsync;

    time_t aof_rewrite_time_last;

    time_t aof_rewrite_time_start;

    int aof_lastbgrewrite_status;

    unsigned long aof_delayed_fsync;

    int aof_rewrite_incremental_fsync;

    int aof_last_write_status;

    int aof_last_write_errno;

    /* RDB持久化字段 */
    /* RDB persistence */

    // 距离上一次成功执行SAVE或者BGSAVE之后，服务器对数据库状态进行了多少次修改
    long long dirty;

    // BGSAVE执行前数据库被修改次数
    long long dirty_before_bgsave;

    // 负责执行BGSAVE的子进程PID；未在执行BGSAVE时，设为-1
    pid_t rdb_child_pid;

    // 保存条件的数组
    struct saveparam* saveparams;

    // 保存条件的数组长度
    int saveparamslen;

    // rdb文件名
    char* rdb_filename;

    // 是否启用压缩
    int rdb_compression;

    // 是否启用校验和
    int rdb_checksum;

    // 最后一次完成SAVE的时间
    time_t lastsave;

    time_t lastbgsave_try;

    time_t rdb_save_time_last;

    time_t rdb_save_time_start;

    int lastbgsave_status;

    int stop_writes_on_bgsave_err;

    /* 命令传播字段 */
    /* Propagation of commands in AOF / replication */

    redisOpArray also_propagate;

    /* 日志 */
    /* Logging */

    char* logfile;

    int syslog_enabled;

    char* syslog_ident;

    int syslog_facility;

    /* 复制 (主节点) */
    /* Replication (master) */

    int slaveseldb;

    long long master_repl_offset;

    int repl_ping_slave_period;

    char* repl_backlog;

    long long repl_backlog_size;

    long long repl_backlog_histlen;

    long long repl_backlog_idx;

    long long repl_backlog_off;

    time_t repl_backlog_time_limit;

    time_t repl_no_slaves_since;

    int repl_min_slaves_to_write;

    int repl_min_slaves_max_lag;

    int repl_good_slaves_count;

    /* 复制 (从节点) */
    /* Replication (slave) */

    char* masterauth;

    // 主服务器地址
    char* masterhost;

    int masterport;

    int repl_timeout;

    redisClient* master;

    redisClient* cached_master;

    int repl_syncio_timeout;

    int repl_state;

    off_t repl_transfer_size;

    off_t repl_transfer_read;

    off_t repl_transfer_last_fsync_off;

    int repl_transfer_s;

    int repl_transfer_fd;

    char* repl_transfer_tmpfile;

    time_t repl_transfer_lastio;

    int repl_serve_stale_data;

    int repl_slave_ro;

    time_t repl_down_since;

    int repl_disable_tcp_nodelay;

    int slave_priority;

    char repl_master_runid[REDIS_RUN_ID_SIZE + 1];

    long long repl_master_initial_offset;

    /* Replication script cache */

    dict* repl_scriptcache_dict;

    list* repl_scriptcache_fifo;

    int repl_scriptcache_size;

    /* Synchronous replication */

    list* clients_waiting_acks;

    int get_ack_from_slaves;

    /* 限制字段 */
    /* Limits */

    int maxclients;

    // 最大内存使用量
    unsigned long long maxmemory;

    int maxmemory_policy;

    int maxmemory_samples;

    /* Blocked clients */

    unsigned int bpop_blocked_clients;

    list* unblocked_clients;

    list* ready_keys;

    /* Sort parameters - qsort_r() is only available under BSD so we
     * have to take this state global, in order to pass it to sortCompare() */

    int sort_desc;

    int sort_alpha;

    int sort_bypattern;

    int sort_store;

    /* 底层编码转换的条件 */
    /* Zip structure config, see redis.conf for more information */

    size_t hash_max_ziplist_entries;

    size_t hash_max_ziplist_value;

    size_t list_max_ziplist_entries;

    size_t list_max_ziplist_value;

    size_t set_max_intset_entries;

    size_t zset_max_ziplist_entries;

    size_t zset_max_ziplist_value;

    size_t hll_sparse_max_bytes;

    time_t unixtime;

    long long mstime;

    /* 订阅 */
    /* Pubsub */

    dict* pubsub_channels;

    list* pubsub_patterns;

    int notify_keyspace_events;

    /* 集群 */
    /* Cluster */

    int cluster_enabled;

    mstime_t cluster_node_timeout;

    char* cluster_configfile;

    struct clusterState* cluster;

    int cluster_migration_barrier;

    /* lua脚本 */
    /* Scripting */

    /* lua_State* lua; */

    redisClient* lua_client;

    redisClient* lua_caller;

    dict* lua_scripts;

    mstime_t lua_time_limit;

    mstime_t lua_time_start;

    int lua_write_dirty;

    int lua_random_dirty;

    int lua_timedout;

    int lua_kill;

    /* Assert & bug reporting */

    char* assert_failed;

    char* assert_file;

    int assert_line;

    int bug_report_start;

    int watchdog_period;

};

/*
 * 外部使用的全局变量声明
 */
/*
 * Extern declarations
 */
extern struct redisServer server;
extern struct sharedObjectsStruct shared;
extern dictType setDictType;
extern dictType zsetDictType;
extern dictType hashDictType;

/*
 * 判断是否要缩小字典
 */
int htNeedsResize(dict* dict);

/*
 * 获取LRU时钟
 */
unsigned int getLRUClock(void);

/* Redis object implementation */
void decrRefCount(robj* o);
void decrRefCountVoid(void* o);
void incrRefCount(robj* o);
robj* resetRefCount(robj* obj);
void freeStringObject(robj* o);
void freeListObject(robj* o);
void freeSetObject(robj* o);
void freeZsetObject(robj* o);
void freeHashObject(robj* o);
robj* createObject(int type, void* ptr);
robj* createStringObject(char* ptr, size_t len);
robj* createRawStringObject(char* ptr, size_t len);
robj* createEmbeddedStringObject(char* ptr, size_t len);
robj* dupStringObject(robj* o);
int isObjectRepresentableAsLongLong(robj* o, long long* llongval);
robj* tryObjectEncoding(robj* o);
robj* getDecodedObject(robj* o);
size_t stringObjectLen(robj* o);
robj* createStringObjectFromLongLong(long long value);
robj* createStringObjectFromLongDouble(long double value);
robj* createListObject(void);
robj* createZiplistObject(void);
robj* createSetObject(void);
robj* createIntsetObject(void);
robj* createHashObject(void);
robj* createZsetObject(void);
robj* createZsetZiplistObject(void);
int checkType(redisClient* c, robj* o, int type);
int getLongFromObjectOrReply(redisClient* c, robj* o, long* target, const char* msg);
int getLongLongFromObjectOrReply(redisClient* c, robj* o, long long* target, const char* msg);
int getDoubleFromObjectOrReply(redisClient* c, robj* o, double* target, const char* msg);
int getLongDoubleFromObjectOrReply(redisClient* c, robj* o, long double* target, const char* msg);
int getLongLongFromObject(robj* o, long long* target);
int getLongDoubleFromObject(robj* o, long double* target);
char* strEncoding(int encoding);
int compareStringObjects(robj* a, robj* b);
int collateStringObjects(robj* a, robj* b);
int equalStringObjects(robj* a, robj* b);
unsigned long long estimateObjectIdleTime(robj* o);
#define sdsEncodedObject(objptr) (objptr->encoding == REDIS_ENCODING_RAW || objptr->encoding == REDIS_ENCODING_EMBSTR)

/* List data type */
void listTypeTryConversion(robj* subject, robj* value);
void listTypePush(robj* subject, robj* value, int where);
robj* listTypePop(robj* subject, int where);
unsigned long listTypeLength(robj* subject);
listTypeIterator* listTypeInitIterator(robj* subject, long index, unsigned char direction);
void listTypeReleaseIterator(listTypeIterator* li);
int listTypeNext(listTypeIterator* li, listTypeEntry* entry);
robj* listTypeGet(listTypeEntry* entry);
void listTypeInsert(listTypeEntry* entry, robj* value, int where);
int listTypeEqual(listTypeEntry* entry, robj* o);
void listTypeDelete(listTypeEntry* entry);
void listTypeConvert(robj* subject, int enc);

/* Set data type */
robj* setTypeCreate(robj* value);
int setTypeAdd(robj* subject, robj* value);
int setTypeRemove(robj* subject, robj* value);
int setTypeIsMember(robj* subject, robj* value);
setTypeIterator* setTypeInitIterator(robj* subject);
void setTypeReleaseIterator(setTypeIterator* si);
int setTypeNext(setTypeIterator* si, robj** objele, int64_t* llele);
robj* setTypeNextObject(setTypeIterator* si);
int setTypeRandomElement(robj* setobj, robj** objele, int64_t* llele);
unsigned long setTypeSize(robj* subject);
void setTypeConvert(robj* subject, int enc);

/* Hash data type */
void hashTypeConvert(robj* o, int enc);
void hashTypeTryConversion(robj* subject, robj** argv, int start, int end);
/* void hashTypeTryObjectEncoding(robj* subject, robj** o1, robj** o2); */
robj* hashTypeGetObject(robj* o, robj* key);
int hashTypeExists(robj* o, robj* key);
int hashTypeSet(robj* o, robj* key, robj* value);
int hashTypeDelete(robj* o, robj* key);
unsigned long hashTypeLength(robj* o);
hashTypeIterator* hashTypeInitIterator(robj* subject);
void hashTypeReleaseIterator(hashTypeIterator* hi);
int hashTypeNext(hashTypeIterator* hi);
void hashTypeCurrentFromZiplist(hashTypeIterator* hi, int what, unsigned char** vstr, unsigned int* vlen, long long* vll);
void hashTypeCurrentFromHashTable(hashTypeIterator* hi, int what, robj** dst);
robj* hashTypeCurrentObject(hashTypeIterator* hi, int what);

/* Sorted sets data type */
zskiplist* zslCreate(void);
void zslFree(zskiplist* zsl);
void zslFreeNode(zskiplistNode* node);
zskiplistNode* zslInsert(zskiplist* zsl, double score, robj* obj);
int zslDelete(zskiplist* zsl, double score, robj* obj);
zskiplistNode* zslFirstInRange(zskiplist* zsl, zrangespec* range);
zskiplistNode* zslLastInRange(zskiplist* zsl, zrangespec* range);
unsigned long zslGetRank(zskiplist* zsl, double score, robj* o);
zskiplistNode* zslGetElementByRank(zskiplist* zsl, unsigned long rank);
int zslValueGteMin(double value, zrangespec* spec);
int zslValueLteMax(double value, zrangespec* spec);
int zslParseRange(robj* min, robj* max, zrangespec* spec);
// encoding is ziplist
unsigned char* zzlInsert(unsigned char* zl, robj* ele, double score);
double zzlGetScore(unsigned char* sptr);
void zzlNext(unsigned char* zl, unsigned char** eptr, unsigned char** sptr);
void zzlPrev(unsigned char* zl, unsigned char** eptr, unsigned char** sptr);
// zset
unsigned int zsetLength(robj* zobj);
void zsetConvert(robj* zobj, int encoding);

/* db.c -- Keyspace access API */
int removeExpire(redisDb* db, robj* key);
void propagateExpire(redisDb* db, robj* key);
int expireIfNeeded(redisDb* db, robj* key);
long long getExpire(redisDb* db, robj* key);
void setExpire(redisDb* db, robj* key, long long when);
robj* lookupKey(redisDb* db, robj* key);
robj* lookupKeyRead(redisDb* db, robj* key);
robj* lookupKeyWrite(redisDb* db, robj* key);
robj* lookupKeyReadOrReply(redisClient* c, robj* key, robj* reply);
robj* lookupKeyWriteOrReply(redisClient* c, robj* key, robj* reply);
void dbAdd(redisDb* db, robj* key, robj* val);
void dbOverWrite(redisDb* db, robj* key, robj* val);
void setKey(redisDb* db, robj* key, robj* val);
int dbExists(redisDb* db, robj* key);
robj* dbRandomKey(redisDb* db);
int dbDelete(redisDb* db, robj* key);
robj* dbUnshareStringValue(redisDb* db, robj* key, robj* o);
long long emptyDb(void(callback)(void*));
int selectDb(redisClient* c, int id);
void signalModifiedKey(redisDb* db, robj* key);
void signalFlushedDb(int dbid);

/* networking.c -- Networking and Client related operations */
redisClient* createClient(int fd);
void freeClient(redisClient* c);
void freeClientAsync(redisClient* c);
void resetClient(redisClient* c);
void sendReplyToClient(aeEventLoop* el, int fd, void* privdata, int mask);
void addReply(redisClient* c, robj* obj);
void* addDeferredMultiBulkLength(redisClient* c);
void setDeferredMultiBulkLength(redisClient* c, void* node, long length);
void addReplySds(redisClient* c, sds s);
void addReplyError(redisClient* c, char* err);
void addReplyStatus(redisClient* c, char* status);
void addReplyDouble(redisClient* c, double d);
void addReplyLongLong(redisClient* c, long long ll);
void addReplyBulk(redisClient* c, robj* obj);
void addReplyBulkCString(redisClient* c, char* s);
void addReplyBulkCBuffer(redisClient* c, void* p, size_t len);
void addReplyBulkLongLong(redisClient* c, long long ll);
void addReplyMultiBulkLen(redisClient* c, long length);
// 修改客户端的参数数组
void rewriteClientCommandVector(redisClient* c, int argc, ...);
// 修改客户端的单个参数
void rewriteClientCommandArgument(redisClient* c, int i, robj* newval);

/* Core functions */
struct redisCommand* lookupCommand(sds name);
void call(redisClient* c, int flags);
int prepareForShutdown(int flags);

/* RDB persistence */
#include "rdb.h"

/* AOF persistence */
void flushAppendOnlyFile(int force);
void feedAppendOnlyFile(struct redisCommand* cmd, int dictid, robj** argv, int argc);
void aofRemoveTempFile(pid_t childpid);
int rewriteAppendOnlyFileBackground(void);
int loadAppendOnlyFile(char* filename);
void stopAppendOnly(void);
void startAppendOnly(void);
void backgroundRewriteDoneHandler(int exitcode, int bysignal);
void aofRewriteBufferReset(void);
unsigned long aofRewriteBufferSize(void);

/* Utils */
long long ustime(void);
long long mstime(void);

/* Commands prototypes */

/* Db commands */
void delCommand(redisClient* c);
void existsCommand(redisClient* c);
void selectCommand(redisClient* c);
void randomkeyCommand(redisClient* c);
void keysCommand(redisClient* c);
void scanCommand(redisClient* c);
void dbsizeCommand(redisClient* c);
void lastsaveCommand(redisClient* c);
void typeCommand(redisClient* c);
void shutdownCommand(redisClient* c);
void moveCommand(redisClient* c);
void renameCommand(redisClient* c);
void renamenxCommand(redisClient* c);

/* String commands */
void setCommand(redisClient* c);
void setnxCommand(redisClient* c);
void setexCommand(redisClient* c);
void psetexCommand(redisClient* c);
void getCommand(redisClient* c);
void appendCommand(redisClient* c);
void incrCommand(redisClient* c);
void decrCommand(redisClient* c);
void incrbyCommand(redisClient* c);
void decrbyCommand(redisClient* c);
void incrbyfloatCommand(redisClient* c);

/* List commands */
void lpushCommand(redisClient* c);
void rpushCommand(redisClient* c);
void lpushxCommand(redisClient* c);
void rpushxCommand(redisClient* c);
void linsertCommand(redisClient* c);
void lpopCommand(redisClient* c);
void rpopCommand(redisClient* c);
void llenCommand(redisClient* c);
void lindexCommand(redisClient* c);
void lremCommand(redisClient* c);
void ltrimCommand(redisClient* c);
void lsetCommand(redisClient* c);

/* Hash commands */
void hsetCommand(redisClient* c);
void hsetnxCommand(redisClient* c);
void hgetCommand(redisClient* c);
void hexistsCommand(redisClient* c);
void hdelCommand(redisClient* c);
void hlenCommand(redisClient* c);
void hgetallCommand(redisClient* c);

/* Set commands */
void saddCommand(redisClient* c);
void sremCommand(redisClient* c);
void scardCommand(redisClient* c);
void sismemberCommand(redisClient* c);
void sinterCommand(redisClient* c);
void sunionCommand(redisClient* c);
void sdiffCommand(redisClient* c);
void srandmemberCommand(redisClient* c);
void spopCommand(redisClient* c);

/* Sorted set commands */
void zaddCommand(redisClient* c);
void zcardCommand(redisClient* c);
void zcountCommand(redisClient* c);
void zrangeCommand(redisClient* c);
void zrevrangeCommand(redisClient* c);
void zrankCommand(redisClient* c);
void zrevrankCommand(redisClient* c);
void zremCommand(redisClient* c);
void zscoreCommand(redisClient* c);

#endif //TINYREDIS_REDIS_H
