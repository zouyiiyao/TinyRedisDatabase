//
// Created by zouyi on 2021/10/1.
//

#include <math.h>

#include "redis.h"

/* To evaluate the output buffer size of a client we need to get size of
 * allocated objects, however we can't used zmalloc_size() directly on sds
 * strings because of the trick they use to work (the header is before the
 * returned pointer), so we use this helper function. */
size_t zmalloc_size_sds(sds s) {
    return zmalloc_size(s - sizeof(struct sdshdr));
}

/* Return the amount of memory used by the sds string at object->ptr
 * for a string object. */
size_t getStringObjectSdsUseMemory(robj* o) {
    assert(o->type == REDIS_STRING);

    switch (o->encoding) {
        case REDIS_ENCODING_RAW:
            return zmalloc_size_sds(o->ptr);
        case REDIS_ENCODING_EMBSTR:
            return sdslen(o->ptr);
        default:
            return 0;    /* Just integer encoding for now. */
    }
}

/*
 * ...
 */
void* dupClientReplyValue(void* o) {
    incrRefCount((robj*)o);
    return o;
}

/*
 * ...
 */
int listMatchObjects(void* a, void* b) {
    return equalStringObjects(a, b);
}

/*
 * 创建一个新的客户端，
 * 调用链: acceptTcpHandler -> acceptCommonHandler -> createClient
 */
redisClient* createClient(int fd) {

    redisClient* c = zmalloc(sizeof(redisClient));

    /* passing -1 as fd it is possible to create a non connected client.
     * This is useful since all the Redis commands needs to be executed
     * in the context of a client. When commands are executed in other
     * contexts (for instance a Lua script) we need a non connected client. */
    if (fd != -1) {
        // 将clientfd设置为非阻塞
        anetNonBlock(NULL, fd);
        // 打开clientfd的TCP_NODELAY选项
        anetEnableTcpNoDelay(NULL, fd);
        // 根据server.tcpkeepalive配置决定是否开启SO_KEEPALIVE选项
        if (server.tcpkeepalive)
            anetKeepAlive(NULL, fd, server.tcpkeepalive);
        // 为clientfd的读事件绑定命令请求处理器readQueryFromClient
        if (aeCreateFileEvent(server.el, fd, AE_READABLE, readQueryFromClient, c) == AE_ERR) {
            close(fd);
            zfree(c);
            return NULL;
        }
    }

    // 设置客户端当前使用数据库为0号数据库
    selectDb(c, 0);
    // 设置与客户端通信的fd
    c->fd = fd;
    // 设置客户端名字
    c->name = NULL;
    // 设置回复缓冲区的偏移量，客户端的回复缓冲区c->buf大小为16K
    c->bufpos = 0;
    // 创建查询缓冲区
    c->querybuf = sdsempty();
    // 设置查询缓冲区峰值
    c->querybuf_peak = 0;
    // 命令请求的类型: 枚举值: REDIS_REQ_INLINE，REDIS_REQ_MULTIBULK
    c->reqtype = 0;
    // 命令参数数量
    c->argc = 0;
    // 命令参数
    c->argv = NULL;
    // 当前执行的命令和上一次执行的命令
    c->cmd = c->lastcmd = NULL;
    // 查询缓冲区中未读入的命令内容数量
    c->multibulklen = 0;
    // 读入的参数的长度
    c->bulklen = -1;
    // 已发送字节数
    c->sentlen = 0;
    // 状态标志
    c->flags = 0;
    // 客户端创建时间和最后一次与服务器交互时间
    c->ctime = c->lastinteraction = server.unixtime;

    // 认证状态
    /* c->authenticated = 0; */
    // TODO: 复制相关，复制状态
    /* c->replstate = REDIS_REPL_NONE; */
    // TODO: 复制相关，复制偏移量
    /* c->reploff = 0; */
    // TODO: 复制相关，通过ACK命令接收到的偏移量
    /* c->repl_ack_off = 0; */
    // TODO: 复制相关，通过ACK命令接收到偏移量的时间
    /* c->repl_ack_time = 0; */
    // TODO: 复制相关，客户端作为从服务器时使用，记录从服务器所使用的端口号
    /* c->slave_listening_port = 0; */

    // 创建回复链表
    c->reply = listCreate();
    // 回复链表的字节数目
    c->reply_bytes = 0;
    // 回复缓冲区大小达到软限制的时间
    c->obuf_soft_limit_reached_time = 0;
    // 设置回复链表的释放和复制函数
    listSetFreeMethod(c->reply, decrRefCountVoid);
    listSetDupMethod(c->reply, dupClientReplyValue);

    // TODO: 阻塞相关
    /* c->btype = REDIS_BLOCKED_NONE; */
    /* c->bpop.timeout = 0; */
    /* c->bpop.keys = dictCreate(&setDictType, NULL); */
    /* c->bpop.target = NULL; */
    /* c->bpop.numreplicas = 0; */
    /* c->bpop.reploffset = 0; */

    // TODO: 复制相关，最后被写入的全局复制偏移量
    /* c->woff = 0; */

    // TODO: 事务相关，事务进行时监视的键
    /* c->watched_keys = listCreate(); */

    // TODO: 发布/订阅相关
    /* c->pubsub_channels = dictCreate(&setDictType, NULL); */
    /* c->pubsub_patterns = listCreate(); */
    /* listSetFreeMethod(c->pubsub_patterns, decrRefCountVoid); */
    /* listSetMatchMethod(c->pubsub_patterns, listMatchObjects); */

    // ip:port对
    c->peerid = NULL;

    // 如果是带连接的客户端，则添加到服务器的客户端链表中
    if (fd != -1) listAddNodeTail(server.clients, c);

    // TODO: 事务相关，初始化客户端的事务状态
    /* initClientMultiState(c); */

    return c;
}

/* This function is called every time we are going to transmit new data
 * to the client. The behavior is the following:
 *
 * 这个函数在每次向客户端发送数据时都会被调用。函数的行为如下：
 *
 * If the client should receive new data (normal clients will) the function
 * returns REDIS_OK, and make sure to install the write handler in our event
 * loop so that when the socket is writable new data gets written.
 *
 * 当客户端可以接收新数据时（通常情况下都是这样），函数返回 REDIS_OK ，
 * 并将写处理器（write handler）安装到事件循环中，
 * 这样当套接字可写时，新数据就会被写入。
 *
 * If the client should not receive new data, because it is a fake client,
 * a master, a slave not yet online, or because the setup of the write handler
 * failed, the function returns REDIS_ERR.
 *
 * 对于那些不应该接收新数据的客户端，
 * 比如伪客户端、 master 以及未 ONLINE 的 slave ，
 * 或者写处理器安装失败时，
 * 函数返回 REDIS_ERR 。
 *
 * Typically gets called every time a reply is built, before adding more
 * data to the clients output buffers. If the function returns REDIS_ERR no
 * data should be appended to the output buffers. 
 *
 * 通常在每个回复被创建时调用，如果函数返回 REDIS_ERR ，
 * 那么没有数据会被追加到输出缓冲区。
 */
int prepareClientToWrite(redisClient* c) {

    // TODO: LUA相关，LUA脚本环境所使用的伪客户端总是可写的
    /* if (c->flags & REDIS_LUA_CLIENT) return REDIS_OK; */

    // TODO: 复制相关，主服务器并且不接受查询，那么它是不可写的，出错
    /* if ((c->flags & REDIS_MASTER) && !(c->flags & REDIS_MASTER_FORCE_REPLY)) */
    /*     return REDIS_ERR; */

    // 无连接的伪客户端总是不可写的
    if (c->fd <= 0) return REDIS_ERR;    /* Fake client */

    // 一般情况下，为clientfd安装写事件处理器到事件循环
    if (c->bufpos == 0 && listLength(c->reply) == 0 && /* (c->replstate == REDIS_REPL_NONE || c->replstate == REDIS_REPL_ONLINE) && */
        aeCreateFileEvent(server.el, c->fd, AE_WRITABLE, sendReplyToClient, c) == AE_ERR)
        return REDIS_ERR;

    return REDIS_OK;
}

/*
 * 当回复链表的最后一个对象不是专属于该回复链表时，创建一个该对象的副本
 */
/* Create a duplicate of the last object in the reply list when
 * it is not exclusively owned by the reply list. */
robj* dupLastObjectIfNeeded(list* reply) {
    robj* new;
    robj* cur;
    listNode* ln;

    assert(listLength(reply) > 0);

    ln = listLast(reply);
    cur = listNodeValue(ln);
    if (cur->refcount > 1) {
        new = dupStringObject(cur);
        decrRefCount(cur);
        listNodeValue(ln) = new;
    }
    return listNodeValue(ln);
}

/* -----------------------------------------------------------------------------
 * Low level functions to add more data to output buffers.
 * -------------------------------------------------------------------------- */

/*
 * 尝试将回复s添加到c->buf中
 */
int _addReplyToBuffer(redisClient* c, char* s, size_t len) {
    // 客户端回复缓冲区可用余量
    size_t available = sizeof(c->buf) - c->bufpos;

    // 如果客户端标志位REDIS_CLOSE_AFTER_REPLY置位，说明客户端要被关闭，不发送消息
    if (c->flags & REDIS_CLOSE_AFTER_REPLY) return REDIS_OK;

    // 如果回复链表c->reply中已经有缓冲块(元素)，则不能添加任何内容到回复缓冲区c->buf
    /* If there already are entries in the reply list, we cannot
     * add anything more to the static buffer. */
    if (listLength(c->reply) > 0) return REDIS_ERR;

    // 检查回复缓冲区余量是否足够存放s
    /* Check that the buffer has enough space available for this string. */
    if (len > available) return REDIS_ERR;

    // 执行复制操作
    memcpy(c->buf + c->bufpos, s, len);
    c->bufpos += len;

    return REDIS_OK;
}

/*
 * 尝试将回复对象(底层为sds)添加到c->reply中
 */
void _addReplyObjectToList(redisClient* c, robj* o) {
    robj* tail;

    // 如果客户端标志位REDIS_CLOSE_AFTER_REPLY置位，说明客户端要被关闭，不发送消息
    if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

    // 如果回复链表当前没有缓冲块，直接将对象添加到回复链表尾
    if (listLength(c->reply) == 0) {
        incrRefCount(o);
        listAddNodeTail(c->reply, o);

        c->reply_bytes += getStringObjectSdsUseMemory(o);
    // 如果回复链表当前已经有缓冲块，如果表尾缓冲块空间足够，则追加到表尾缓冲块内容后；否则创建一个新的缓冲块，添加到回复链表尾；
    } else {

        tail = listNodeValue(listLast(c->reply));

        // 回复内容追加到表尾缓冲块内容后
        /* Append to this object when possible. */
        if (tail->ptr != NULL && tail->encoding == REDIS_ENCODING_RAW &&
            sdslen(tail->ptr) + sdslen(o->ptr) <= REDIS_REPLY_CHUNK_BYTES) {
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            tail->ptr = sdscatlen(tail->ptr, o->ptr, sdslen(o->ptr));
            c->reply_bytes += zmalloc_size_sds(tail->ptr);

        // 创建一个新的缓冲块
        } else {
            incrRefCount(o);
            listAddNodeTail(c->reply, o);
            c->reply_bytes += getStringObjectSdsUseMemory(o);

        }
    }

    // 检查客户端的回复链表字节总数(虚拟的)是否达到软性或硬性限制，如果达到，则异步关闭客户端
    asyncCloseClientOnOutputBufferLimitReached(c);
}

/*
 * 尝试将sds添加到c->reply中
 */
/* This method takes responsibility over the sds. When it is no longer
 * needed it will be free'd, otherwise it ends up in a robj. */
void _addReplySdsToList(redisClient* c, sds s) {
    robj* tail;

    if (c->flags & REDIS_CLOSE_AFTER_REPLY) {
        sdsfree(s);
        return;
    }

    if (listLength(c->reply) == 0) {
        listAddNodeTail(c->reply, createObject(REDIS_STRING, s));
        c->reply_bytes += zmalloc_size_sds(s);
    } else {
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        if (tail->ptr != NULL && tail->encoding == REDIS_ENCODING_RAW &&
            sdslen(tail->ptr) + sdslen(s) <= REDIS_REPLY_CHUNK_BYTES) {
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            tail->ptr = sdscatlen(tail->ptr, s, sdslen(s));
            c->reply_bytes += zmalloc_size_sds(tail->ptr);
            sdsfree(s);

        } else {
            listAddNodeTail(c->reply, createObject(REDIS_STRING, s));
            c->reply_bytes += zmalloc_size_sds(s);
        }
    }

    asyncCloseClientOnOutputBufferLimitReached(c);
}

/*
 * 尝试将C-Style字符串添加到c->reply中
 */
void _addReplyStringToList(redisClient* c, char* s, size_t len) {
    robj* tail;

    if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

    if (listLength(c->reply) == 0) {
        robj* o = createStringObject(s, len);
        listAddNodeTail(c->reply, o);
        c->reply_bytes += getStringObjectSdsUseMemory(o);

    } else {
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        if (tail->ptr != NULL && tail->encoding == REDIS_ENCODING_RAW &&
            sdslen(tail->ptr) + len <= REDIS_REPLY_CHUNK_BYTES) {
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            tail->ptr = sdscatlen(tail->ptr, s, len);
            c->reply_bytes += zmalloc_size_sds(tail->ptr);

        } else {
            robj* o = createStringObject(s, len);
            listAddNodeTail(c->reply, o);
            c->reply_bytes += getStringObjectSdsUseMemory(o);

        }
    }

    asyncCloseClientOnOutputBufferLimitReached(c);
}

/* -----------------------------------------------------------------------------
 * Higher level functions to queue data on the client output buffer.
 * The following functions are the ones that commands implementations will call.
 * -------------------------------------------------------------------------- */

/*
 * RESP协议解释
 *
 * RESP协议在Redis1.2被引入，直到Redis2.0才成为和Redis服务器通信的标准，这个协议需要在你的Redis客户端实现。
 * RESP是一个支持多种数据类型的序列化协议: 简单字符串(Simple Strings)，错误(Errors)，整型(Integers)，大容量字符串(Bulk Strings)和数组(Arrays)。
 *
 * RESP在Redis中作为一个请求-响应协议以如下方式使用:
 * 1. 客户端以大容量字符串RESP数组的方式发送命令给服务器端。
 * 2. 服务器端根据命令的具体实现返回某一种RESP数据类型。
 * 
 * 在RESP中，数据的类型依赖于首字节:
 * 1. 单行字符串(Simple Strings): 响应的首字节是'+'，如"+Ok\r\n"
 * 2. 错误(Errors): 响应的首字节是'-'，如"-Error message\r\n"
 * 3. 整型(Integers): 响应的首字节是':'，如":0\r\n"
 * 4. 大容量字符串(Bulk Strings): 响应的首字节是'$'，如"$6\r\nfoobar\r\n"
 * 5. 数组(Arrays): 响应的首字节是'*'，如"*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n"
 *
 * 另外，RESP可以使用大容量字符串或者数组类型的特殊变量表示空值。RESP协议的不同部分总是以"\r\n"(CRLF)结束。
 */

/*
 * 添加对象到回复，提供给命令调用的高层函数
 */
void addReply(redisClient* c, robj* obj) {

    // 为clientfd安装写事件处理器到事件循环
    if (prepareClientToWrite(c) != REDIS_OK) return;

    // 针对copy-on-write进行优化
    /* This is an important place where we can avoid copy-on-write
     * when there is a saving child running, avoiding touching the
     * refcount field of the object if it's not needed.
     *
     * If the encoding is RAW and there is room in the static buffer
     * we'll be able to send the object to the client without
     * messing with its page.
     */
    if (sdsEncodedObject(obj)) {
        // 首先尝试复制内容到 c->buf 中，这样可以避免内存分配，并且不修改对象的refcount域
        if (_addReplyToBuffer(c, obj->ptr, sdslen(obj->ptr)) != REDIS_OK)
            // 如果 c->buf 中的空间不够，就复制到 c->reply 链表中，可能会引起内存分配，并且会修改对象的refcount域
            _addReplyObjectToList(c, obj);
    } else if (obj->encoding == REDIS_ENCODING_INT) {
        // 优化，如果c->buf中有等于或多于32个字节的空间，那么将整数直接以字符串的形式复制到 c->buf 中
        /* Optimization: if there is room in the static buffer for 32 bytes
         * (more than the max chars a 64 bit integer can take as string) we
         * avoid decoding the object and go for the lower level approach. */
        if (listLength(c->reply) == 0 && (sizeof(c->buf) - c->bufpos) >= 32) {
            char buf[32];
            int len;

            len = ll2string(buf, sizeof(buf), (long)obj->ptr);
            if (_addReplyToBuffer(c, buf, len) == REDIS_OK)
                return;
            /* else... continue with the normal code path, but should never
             * happen actually since we verified there is room. */
        }

        obj = getDecodedObject(obj);
        if (_addReplyToBuffer(c, obj->ptr, sdslen(obj->ptr)) != REDIS_OK)
            _addReplyObjectToList(c, obj);
        decrRefCount(obj);
    } else {
        exit(1);
    }
}

/*
 * 添加sds到回复，提供给命令调用的高层函数
 */
void addReplySds(redisClient* c, sds s) {
    if (prepareClientToWrite(c) != REDIS_OK) {
        /* The caller expects the sds to be free'd. */
        sdsfree(s);
        return;
    }

    if (_addReplyToBuffer(c, s, sdslen(s)) == REDIS_OK) {
        sdsfree(s);
    } else {
        /* This method free's the sds when it is no longer needed. */
        _addReplySdsToList(c, s);
    }
}

/*
 * 添加C-Style字符串到回复，提供给命令调用的高层函数
 */
void addReplyString(redisClient* c, char* s, size_t len) {
    if (prepareClientToWrite(c) != REDIS_OK) return;

    if (_addReplyToBuffer(c, s, len) != REDIS_OK)
        _addReplyStringToList(c, s, len);
}

/*
 * 添加错误字符串到回复，提供给命令调用的高层函数
 */
void addReplyErrorLength(redisClient* c, char* s, size_t len) {
    addReplyString(c, "-ERR", 5);
    addReplyString(c, s, len);
    addReplyString(c, "\r\n", 2);
}

/*
 * 添加错误字符串到回复，提供给命令调用的高层函数，addReplyErrorLength的封装
 */
void addReplyError(redisClient* c, char* err) {
    addReplyErrorLength(c, err, strlen(err));
}

/*
 * 添加错误字符串到回复(以格式fmt&不定参数)，提供给命令调用的高层函数，addReplyErrorLength的封装
 */
void addReplyErrorFormat(redisClient *c, const char *fmt, ...) {
    size_t l, j;
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted. */
    l = sdslen(s);
    for (j = 0; j < l; j++) {
        if (s[j] == '\r' || s[j] == '\n') s[j] = ' ';
    }
    addReplyErrorLength(c,s,sdslen(s));
    sdsfree(s);
}

void addReplyStatusLength(redisClient* c, char* s, size_t len) {
    addReplyString(c, "+", 1);
    addReplyString(c, s, len);
    addReplyString(c, "\r\n", 2);
}

/*
 * 添加类型字符串到回复，提供给命令调用的高层函数，addReplyStatusLength的封装
 */
void addReplyStatus(redisClient* c, char* status) {
    addReplyStatusLength(c, status, strlen(status));
}

/*
 * 增加一个空对象到回复链表，该对象将会包含一个多行大容量字符串的行数，在调用该函数时并不确定其具体的值，
 * 之后通过调用setDeferredMultiBulkLength函数设置
 */
/* Adds an empty object to the reply list that will contain the multi bulk
 * length, which is not known when this function is called. */
void* addDeferredMultiBulkLength(redisClient* c) {
    // 为clientfd安装写事件处理器到事件循环
    /* Note that we install the write event here even if the object is not
     * ready to be sent, since we are sure that before returning to the
     * event loop setDeferredMultiBulkLength() will be called. */
    if (prepareClientToWrite(c) != REDIS_OK) return NULL;

    // 添加到客户端回复链表尾部
    listAddNodeTail(c->reply, createObject(REDIS_STRING, NULL));

    // 返回这个空对象指针
    return listLast(c->reply);
}

/*
 * 为addDeferredMultiBulkLength添加的空对象填入具体的值，该值是一个多行大容量字符串回复的行数
 */
/* Populate the length object and try gluing it to the next chunk. */
void setDeferredMultiBulkLength(redisClient* c, void* node, long length) {
    listNode* ln = (listNode*)node;
    robj* len;
    robj* next;

    /* Abort when *node is NULL (see addDeferredMultiBulkLength). */
    if (node == NULL) return;

    // 为len对象填入具体的值length
    len = listNodeValue(ln);
    len->ptr = sdscatprintf(sdsempty(), "*%ld\r\n", length);
    len->encoding = REDIS_ENCODING_RAW;    /* in case it was an EMBSTR */
    c->reply_bytes += zmalloc_size_sds(len->ptr);
    if (ln->next != NULL) {
        next = listNodeValue(ln->next);

        /* Only glue when the next node is non-NULL (an sds in this case) */
        if (next->ptr != NULL) {
            c->reply_bytes -= zmalloc_size_sds(len->ptr);
            c->reply_bytes -= getStringObjectSdsUseMemory(next);
            len->ptr = sdscatlen(len->ptr, next->ptr, sdslen(next->ptr));
            c->reply_bytes += zmalloc_size_sds(len->ptr);
            listDelNode(c->reply, ln->next);
        }
    }

    asyncCloseClientOnOutputBufferLimitReached(c);
}

/*
 * 以大容量字符串的形式添加一个double到回复
 */
/* Add a double as a bulk reply */
void addReplyDouble(redisClient* c, double d) {
    char dbuf[128];
    char sbuf[128];
    int dlen;
    int slen;
    if (isinf(d)) {
        /* Libc in odd systems (Hi Solaris!) will format infinite in a
         * different way, so better to handle it in an explicit way. */
        addReplyBulkCString(c, d > 0 ? "inf" : "-inf");
    } else {
        dlen = snprintf(dbuf, sizeof(dbuf), "%.17g", d);
        slen = snprintf(sbuf, sizeof(sbuf), "$%d\r\n%s\r\n", dlen, dbuf);
        addReplyString(c, sbuf, slen);
    }
}

/*
 * 带前缀prefix添加一个long long到回复，一般该long long是大容量字符串字节数($ll\r\n)或多行大容量字符串的行数(*ll\r\n)
 */
/* Add a long long as integer reply or bulk len / multi bulk count.
 *
 * Basically this is used to output <prefix><long long><crlf>.
 */
void addReplyLongLongWithPrefix(redisClient* c, long long ll, char prefix) {
    char buf[128];
    int len;

    /* Things like $3\r\n or *2\r\n are emitted very often by the protocol
     * so we have a few shared objects to use if the integer is small
     * like it is most of the times. */
    if (prefix == '*' && ll < REDIS_SHARED_BULKHDR_LEN) {
        addReply(c, shared.mbulkhdr[ll]);
        return;
    } else if (prefix == '$' && ll < REDIS_SHARED_BULKHDR_LEN) {
        addReply(c, shared.bulkhdr[ll]);
        return;
    }

    buf[0] = prefix;
    len = ll2string(buf + 1, sizeof(buf) - 1, ll);
    buf[len + 1] = '\r';
    buf[len + 2] = '\n';
    addReplyString(c, buf, len + 3);
}

/*
 * 以整型的形式添加一个long long到回复
 */
void addReplyLongLong(redisClient* c, long long ll) {
    if (ll == 0)
        addReply(c, shared.czero);
    else if (ll == 1)
        addReply(c, shared.cone);
    else
        addReplyLongLongWithPrefix(c, ll, ':');
}

/*
 * 添加多行大容量字符串类型回复的行数到回复
 */
void addReplyMultiBulkLen(redisClient* c, long length) {
    if (length < REDIS_SHARED_BULKHDR_LEN)
        addReply(c, shared.mbulkhdr[length]);
    else
        addReplyLongLongWithPrefix(c, length, '*');
}

/*
 * 添加大容量字符串字节数到回复
 */
/* Create the length prefix of a bulk reply, example: $2234 */
void addReplyBulkLen(redisClient* c, robj* obj) {
    size_t len;

    if (sdsEncodedObject(obj)) {
        len = sdslen(obj->ptr);
    } else {
        long n = (long)obj->ptr;

        /* Compute how many bytes will take this integer as a radix 10 string */
        len = 1;
        if (n < 0) {
            len++;
            n = -n;
        }
        while ((n = n / 10) != 0) {
            len++;
        }
    }

    if (len < REDIS_SHARED_BULKHDR_LEN)
        addReply(c, shared.bulkhdr[len]);
    else
        addReplyLongLongWithPrefix(c, len, '$');
}

/*
 * 以大容量字符串的形式添加一个redis对象到回复
 */
/* Add a Redis Object as a bulk reply */
void addReplyBulk(redisClient* c, robj* obj) {
    // 长度: $ll\r\n
    addReplyBulkLen(c, obj);
    // 内容: content
    addReply(c, obj);
    // CRLF: \r\n
    addReply(c, shared.crlf);
}

/*
 * 以大容量字符串的形式添加一个C-Style字符串到回复
 */
/* Add a C buffer as bulk reply */
void addReplyBulkCBuffer(redisClient* c, void* p, size_t len) {
    addReplyLongLongWithPrefix(c, len, '$');
    addReplyString(c, p, len);
    addReply(c, shared.crlf);
}

/*
 * 以大容量字符串的形式添加一个C-Style字符串到回复，调用addReplyBulkCBuffer完成实际的工作
 */
/* Add a C nul term string as bulk reply */
void addReplyBulkCString(redisClient* c, char* s) {
    if (s == NULL) {
        addReply(c, shared.nullbulk);
    } else {
        addReplyBulkCBuffer(c, s, strlen(s));
    }
}

/*
 * 以大容量字符串的形式添加一个ll到回复，调用addReplyBulkCBuffer完成实际的工作
 */
/* Add a long long as a bulk reply */
void addReplyBulkLongLong(redisClient* c, long long ll) {
    char buf[64];
    int len;

    len = ll2string(buf, 64, ll);
    addReplyBulkCBuffer(c, buf, len);
}

/* -----------------------------------------------------------------------------
 * 事件处理器相关API
 * -------------------------------------------------------------------------- */

/*
 * 当一个新的TCP连接被创建时调用，完成必要的工作
 */
#define MAX_ACCEPTS_PER_CALL 1000
static void acceptCommonHandler(int fd, int flags) {

    redisClient* c;
    // 创建一个新的客户端并初始化
    if ((c = createClient(fd)) == NULL) {
        printf("Error registering fd event for the new client: %s (fd=%d)", strerror(errno), fd);
        close(fd);    /* May be already closed, just ignore errors */
        return;
    }

    // 连接数目达到服务端允许的上限，给客户端返回错误后直接关闭该客户端
    /* If maxclient directive is set and this is one client more... close the
     * connection. Note that we create the client instead to check before
     * for this condition, since now the socket is already set in non-blocking
     * mode and we can send an error for free using the Kernel I/O */
    if (listLength(server.clients) > server.maxclients) {
        char* err = "-ERR max number of clients reached\r\n";

        /* That's a best effort error message, don't check write errors */
        if (write(c->fd, err, strlen(err)) == -1) {
            /* Nothing to do, Just to avoid the warning... */
        }
        // 维护统计信息: 拒绝连接次数
        server.stat_rejected_conn++;
        freeClient(c);
        return;
    }

    // 维护统计信息: 允许连接次数
    server.stat_numconnections++;

    c->flags |= flags;
}

/*
 * 连接应答处理器
 *
 * 为服务端监听描述符listenfd注册的读事件处理器，调用max次accept(取出clientfd)，然后调用acceptCommonHandler完成带连接的客户端初始化工作
 */
void acceptTcpHandler(aeEventLoop* el, int fd, void* privdata, int mask) {
    int cport;
    int cfd;
    int max = MAX_ACCEPTS_PER_CALL;
    char cip[REDIS_IP_STR_LEN];
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);

    // 调用max次accept
    while (max--) {
        // accept的封装
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        // 遇到ANET_ERR错误，提前返回
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                printf("Accepting client connection: %s", server.neterr);
            return;
        }
        printf("Accepted %s:%d", cip, cport);
        // 创建redisClient结构，设置clientfd，为clientfd读事件绑定命令请求处理器等
        acceptCommonHandler(cfd, 0);
    }
}

/*
 * 与acceptTcpHandler功能一样，只不过是处理本地连接
 */
void acceptUnixHandler(aeEventLoop* el, int fd, void* privdata, int mask) {
    int cfd;
    int max = MAX_ACCEPTS_PER_CALL;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);

    while (max--) {
        cfd = anetUnixAccept(server.neterr, fd);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                printf("Accepting client connection: %s", server.neterr);
            return;
        }
        printf("Accepted connection to %s", server.unixsocket);
        acceptCommonHandler(cfd, REDIS_UNIX_SOCKET);
    }
}

/*
 * 释放客户端的参数相关域
 */
static void freeClientArgv(redisClient* c) {
    int j;
    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    c->argc = 0;
    c->cmd = NULL;
}

/*
 * 释放客户端结构
 */
void freeClient(redisClient* c) {
    listNode* ln;

    // 如果是服务器当前客户端，则重置
    /* If this is marked as current client unset it */
    if (server.current_client == c) server.current_client = NULL;

    // TODO: 复制相关
    /* If it is our master that's beging disconnected we should make sure
     * to cache the state to try a partial resynchronization later.
     *
     * Note that before doing this we make sure that the client is not in
     * some unexpected state, by checking its flags. */
    // if (server.master && c->flags & REDIS_MASTER) {
    //     redisLog(REDIS_WARNING,"Connection with master lost.");
    //     if (!(c->flags & (REDIS_CLOSE_AFTER_REPLY|
    //                       REDIS_CLOSE_ASAP|
    //                       REDIS_BLOCKED|
    //                       REDIS_UNBLOCKED)))
    //     {
    //         replicationCacheMaster(c);
    //         return;
    //     }
    // }

    /* Log link disconnection with slave */
    // if ((c->flags & REDIS_SLAVE) && !(c->flags & REDIS_MONITOR)) {
    //     char ip[REDIS_IP_STR_LEN];
    // 
    //     if (anetPeerToString(c->fd,ip,sizeof(ip),NULL) != -1) {
    //         redisLog(REDIS_WARNING,"Connection with slave %s:%d lost.",
    //                  ip, c->slave_listening_port);
    //     }
    // }

    // 释放客户端查询缓冲区
    /* Free the query buffer */
    sdsfree(c->querybuf);
    c->querybuf = NULL;

    // TODO: 阻塞相关
    /* Deallocate structures used to block on blocking ops. */
    // if (c->flags & REDIS_BLOCKED) unblockClient(c);
    // dictRelease(c->bpop.keys);

    // TODO: 事务相关
    /* UNWATCH all the keys */
    // unwatchAllKeys(c);
    // listRelease(c->watched_keys);

    // TODO: 发布/订阅相关
    /* Unsubscribe from all the pubsub channels */
    // pubsubUnsubscribeAllChannels(c,0);
    // pubsubUnsubscribeAllPatterns(c,0);
    // dictRelease(c->pubsub_channels);
    // listRelease(c->pubsub_patterns);

    // 关闭clientfd，删除监听的读/写事件
    /* Close socket, unregister events, and remove list of replies and
     * accumulated arguments. */
    if (c->fd != -1) {
        aeDeleteFileEvent(server.el, c->fd, AE_READABLE);
        aeDeleteFileEvent(server.el, c->fd, AE_WRITABLE);
        close(c->fd);
    }

    // 释放回复链表
    listRelease(c->reply);

    // 释放参数相关域
    freeClientArgv(c);

    // 从服务器的客户端链表中删除该客户端
    /* Remove from the list of clients */
    if (c->fd != -1) {
        ln = listSearchKey(server.clients, c);
        assert(ln != NULL);
        listDelNode(server.clients, ln);
    }

    // TODO: 阻塞相关
    /* When client was just unblocked because of a blocking operation,
     * remove it from the list of unblocked clients. */
    // if (c->flags & REDIS_UNBLOCKED) {
    //     ln = listSearchKey(server.unblocked_clients,c);
    //     redisAssert(ln != NULL);
    //     listDelNode(server.unblocked_clients,ln);
    // }

    // TODO: 复制相关
    /* Master/slave cleanup Case 1:
     * we lost the connection with a slave. */
    // if (c->flags & REDIS_SLAVE) {
    //     if (c->replstate == REDIS_REPL_SEND_BULK) {
    //         if (c->repldbfd != -1) close(c->repldbfd);
    //         if (c->replpreamble) sdsfree(c->replpreamble);
    //     }
    //     list *l = (c->flags & REDIS_MONITOR) ? server.monitors : server.slaves;
    //     ln = listSearchKey(l,c);
    //     redisAssert(ln != NULL);
    //     listDelNode(l,ln);
    //     /* We need to remember the time when we started to have zero
    //      * attached slaves, as after some time we'll free the replication
    //      * backlog. */
    //     if (c->flags & REDIS_SLAVE && listLength(server.slaves) == 0)
    //         server.repl_no_slaves_since = server.unixtime;
    //     refreshGoodSlavesCount();
    // }

    /* Master/slave cleanup Case 2:
     * we lost the connection with the master. */
    // if (c->flags & REDIS_MASTER) replicationHandleMasterDisconnection();

    /*
     * 如果客户端的异步关闭标识REDIS_CLOSE_ASAP被打开，则将其从服务器的待关闭客户端链表中移除
     */
    /* If this client was scheduled for async freeing we need to remove it
     * from the queue. */
    if (c->flags & REDIS_CLOSE_ASAP) {
        ln = listSearchKey(server.clients_to_close, c);
        assert(ln != NULL);
        listDelNode(server.clients_to_close, ln);
    }

    // 释放客户端其他域
    /* Release other dynamically allocated client structure fields,
     * and finally release the client structure itself. */
    if (c->name) decrRefCount(c->name);
    zfree(c->argv);
    
    // TODO: 事务相关
    // freeClientMultiState(c);

    sdsfree(c->peerid);
    zfree(c);
}

/*
 * 打开客户端的REDIS_CLOSE_ASAP标识，然后在serverCron时间事件中异步地将其关闭，
 * 需要该设计的原因是调用该函数时还不能直接释放客户端freeClient
 */
/* Schedule a client to free it at a safe time in the serverCron() function.
 * This function is useful when we need to terminate a client but we are in
 * a context where calling freeClient() is not possible, because the client
 * should be valid for the continuation of the flow of the program. */
void freeClientAsync(redisClient* c) {
    if (c->flags & REDIS_CLOSE_ASAP) return;
    // 打开客户端的REDIS_CLOSE_ASAP标识
    c->flags |= REDIS_CLOSE_ASAP;
    // 添加到server.clients_to_close链表中
    listAddNodeTail(server.clients_to_close, c);
}

/*
 * 释放需要异步释放的客户端，全部存放在server.clients_to_close链表中
 */
void freeClientsInAsyncFreeQueue(void) {

    while (listLength(server.clients_to_close)) {
        listNode* ln = listFirst(server.clients_to_close);
        redisClient* c = listNodeValue(ln);

        c->flags &= ~REDIS_CLOSE_ASAP;
        freeClient(c);
        listDelNode(server.clients_to_close, ln);
    }
}

/*
 * 命令回复处理器
 *
 * 为clientfd绑定的写事件处理器，将回复链表和回复缓冲区的内容发送出去(调用write)，绑定操作在prepareClientToWrite中完成
 */
void sendReplyToClient(aeEventLoop* el, int fd, void* privdata, int mask) {
    redisClient* c = privdata;
    int nwritten = 0;
    int totwritten = 0;
    int objlen;
    size_t objmem;
    robj* o;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    // 回复缓冲区中还有内容或者回复链表还有节点，正常情况下把回复链表和回复缓冲区的内容全部发送
    while (c->bufpos > 0 || listLength(c->reply)) {

        // 发送回复缓冲区内容
        if (c->bufpos > 0) {

            nwritten = write(fd, c->buf + c->sentlen, c->bufpos - c->sentlen);
            if (nwritten <= 0) break;
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If the buffer was sent, set bufpos to zero to continue with
             * the remainder of the reply. */
            if (c->sentlen == c->bufpos) {
                c->bufpos = 0;
                c->sentlen = 0;
            }

        // 发送回复链表内容
        } else {

            o = listNodeValue(listFirst(c->reply));
            objlen = sdslen(o->ptr);
            objmem = getStringObjectSdsUseMemory(o);

            // 跳过空节点
            if (objlen == 0) {
                listDelNode(c->reply, listFirst(c->reply));
                c->reply_bytes -= objmem;
                continue;
            }

            nwritten = write(fd, ((char*)o->ptr) + c->sentlen, objlen - c->sentlen);
            if (nwritten <= 0) break;
            c->sentlen += nwritten;
            totwritten += nwritten;

            // 处理下一节点
            /* If we fully sent the object on head go to the next one */
            if (c->sentlen == objlen) {
                listDelNode(c->reply, listFirst(c->reply));
                c->sentlen = 0;
                c->reply_bytes -= objmem;
            }
        }

        /* Note that we avoid to send more than REDIS_MAX_WRITE_PER_EVENT
         * bytes, in a single threaded server it's a good idea to serve
         * other clients as well, even if a very large request comes from
         * super fast link that is always able to accept data (in real world
         * scenario think about 'KEYS *' against the loopback interface).
         *
         * 为了避免一个非常大的回复独占服务器，
         * 当写入的总数量大于 REDIS_MAX_WRITE_PER_EVENT ，
         * 临时中断写入，将处理时间让给其他客户端，
         * 剩余的内容等下次写入就绪再继续写入
         *
         * However if we are over the maxmemory limit we ignore that and
         * just deliver as much data as it is possible to deliver.
         *
         * 不过，如果服务器的内存占用已经超过了限制，
         * 那么为了将回复缓冲区中的内容尽快写入给客户端，
         * 然后释放回复缓冲区的空间来回收内存，
         * 这时即使写入量超过了 REDIS_MAX_WRITE_PER_EVENT ，
         * 程序也继续进行写入
         */
        if (totwritten > REDIS_MAX_WRITE_PER_EVENT && (server.maxmemory == 0 || zmalloc_used_memory() < server.maxmemory))
            break;
    }

    // 处理写入错误
    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            printf("Error writing to client: %s", strerror(errno));
            freeClient(c);
            return;
        }
    }

    if (totwritten > 0) {
        /* For clients representing masters we don't count sending data
         * as an interaction, since we always send REPLCONF ACK commands
         * that take some time to just fill the socket output buffer.
         * We just rely on data / pings received for timeout detection. */
        if (!(c->flags & REDIS_MASTER)) c->lastinteraction = server.unixtime;
    }

    // 如果已经发送完毕
    if (c->bufpos == 0 && listLength(c->reply) == 0) {
        c->sentlen = 0;

        // 删除为clientfd绑定的写文件事件
        aeDeleteFileEvent(server.el, c->fd, AE_WRITABLE);

        // 如果客户端的REDIS_CLOSE_AFTER_REPLY标识被打开，则发送完回复后释放客户端
        if (c->flags & REDIS_CLOSE_AFTER_REPLY) freeClient(c);
    }
}

/*
 * 重置客户端以处理下一个命令
 */
/* resetClient prepare the client to process the next command */
void resetClient(redisClient* c) {
    /* redisCommandProc* prevcmd = c->cmd ? c->cmd->proc : NULL; */

    freeClientArgv(c);

    // 初始化命令类型
    c->reqtype = 0;
    // 初始化参数数量
    c->multibulklen = 0;
    // 初始化单个参数字节数
    c->bulklen = -1;

    // TODO: 集群相关
    /* We clear the ASKING flag as well if we are not inside a MULTI, and
     * if what we just executed is not the ASKING command itself. */
    // if (!(c->flags & REDIS_MULTI) && prevcmd != askingCommand)
    //     c->flags &= (~REDIS_ASKING);
}

/*
 * 协议出错时的辅助函数，打开客户端REDIS_CLOSE_AFTER_REPLY标识
 */
/* Helper function. Trims query buffer to make the function that processes
 * multi bulk requests idempotent. */
static void setProtocolError(redisClient* c, int pos) {
    /* if (server.verbosity >= REDIS_VERBOSE) { */
        sds client = catClientInfoString(sdsempty(),c);
        printf("Protocol error from client: %s\n", client);
        sdsfree(client);
    /* } */
    c->flags |= REDIS_CLOSE_AFTER_REPLY;
    // 截取查询缓冲区pos后的内容
    sdsrange(c->querybuf, pos, -1);
}

/*
 * 预处理单行命令，按照协议对输入命令进行分隔，创建参数对象
 */
/*
 * 处理内联命令，并创建参数对象
 *
 * 内联命令的各个参数以空格分开，并以 \r\n 结尾
 * 例子：
 *
 * <arg0> <arg1> <arg...> <argN>\r\n
 *
 * 这些内容会被用于创建参数对象，
 * 比如
 *
 * argv[0] = arg0
 * argv[1] = arg1
 * argv[2] = arg2
 */
int processInlineBuffer(redisClient* c) {
    char* newline;
    int argc;
    int j;
    sds* argv;
    sds aux;
    size_t querylen;

    /* Search for end of line */
    newline = strchr(c->querybuf, '\n');

    /* Nothing to do without a \r\n */
    if (newline == NULL) {
        if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
            addReplyError(c, "Protocol error: too big inline request");
            setProtocolError(c, 0);
        }
        return REDIS_ERR;
    }

    /* Handle the \r\n case. */
    if (newline && newline != c->querybuf && *(newline - 1) == '\r')
        newline--;

    /* Split the input buffer up to the \r\n */
    querylen = newline - c->querybuf;
    aux = sdsnewlen(c->querybuf, querylen);
    argv = sdssplitargs(aux, &argc);
    sdsfree(aux);
    if (argv == NULL) {
        addReplyError(c, "Protocol error: unbalanced quotes in request");
        setProtocolError(c, 0);
        return REDIS_ERR;
    }

    // TODO: 复制相关
    /* Newline from slaves can be used to refresh the last ACK time.
     * This is useful for a slave to ping back while loading a big
     * RDB file. */
    /* if (querylen == 0 && c->flags & REDIS_SLAVE) */
    /*    c->repl_ack_time = server.unixtime; */

    // 删除查询缓冲区中第一行的内容
    /* Leave data after the first line of the query in the buffer */
    sdsrange(c->querybuf, querylen + 2, -1);

    /* Setup argv array on client structure */
    if (c->argv) zfree(c->argv);
    c->argv = zmalloc(sizeof(robj*) * argc);

    // 为各个参数创建redis对象
    /* Create redis objects for all arguments. */
    for (c->argc = 0, j = 0; j < argc; j++) {
        if (sdslen(argv[j])) {
            c->argv[c->argc] = createObject(REDIS_STRING, argv[j]);
            c->argc++;
        } else {
            sdsfree(argv[j]);
        }
    }

    zfree(argv);

    return REDIS_OK;
}

/*
 * 预处理多行命令
 */
/*
 * 将 c->querybuf 中的协议内容转换成 c->argv 中的参数对象
 * 
 * 比如 *3\r\n$3\r\nSET\r\n$3\r\nMSG\r\n$5\r\nHELLO\r\n
 * 将被转换为：
 * argv[0] = SET
 * argv[1] = MSG
 * argv[2] = HELLO
 */
int processMultibulkBuffer(redisClient* c) {
    char* newline = NULL;
    int pos = 0;
    int ok;
    long long ll;

    if (c->multibulklen == 0) {
        /* The client should have been reset */
        assert(c->argc == 0);

        /* Multi bulk length cannot be read without a \r\n */
        newline = strchr(c->querybuf, '\r');
        if (newline == NULL) {
            if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
                addReplyError(c, "Protocol error: too big mbulk count string");
                setProtocolError(c, 0);
            }
            return REDIS_ERR;
        }

        /* Buffer should also contain \n */
        if (newline - (c->querybuf) > ((signed)sdslen(c->querybuf) - 2))
            return REDIS_ERR;

        /* We know for sure there is a whole line since newline != NULL,
         * so go ahead and find out the multi bulk length. */
        assert(c->querybuf[0] == '*');
        ok = string2ll(c->querybuf + 1, newline - (c->querybuf + 1), &ll);
        if (!ok || ll > 1024 * 1024) {
            addReplyError(c, "Protocol error: invalid multibulk length");
            setProtocolError(c, pos);
            return REDIS_ERR;
        }

        pos = (newline - c->querybuf) + 2;
        if (ll <= 0) {
            sdsrange(c->querybuf, pos, -1);
            return REDIS_OK;
        }

        // 多行命令的行数，也即参数数量
        c->multibulklen = ll;

        /* Setup argv array on client structure */
        if (c->argv) zfree(c->argv);
        c->argv = zmalloc(sizeof(robj*) * c->multibulklen);
    }

    assert(c->multibulklen > 0);

    while (c->multibulklen) {

        /* Read bulk length if unknown */
        if (c->bulklen == -1) {
            newline = strchr(c->querybuf + pos, '\r');
            if (newline == NULL) {
                if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
                    addReplyError(c, "Protocol error: too big bulk count string");
                    setProtocolError(c, 0);
                    return REDIS_ERR;
                }
                break;
            }

            /* Buffer should also contain \n */
            if (newline - (c->querybuf) > ((signed)sdslen(c->querybuf) - 2))
                break;

            if (c->querybuf[pos] != '$') {
                addReplyErrorFormat(c, "Protocol error: expected '$', got '%c'", c->querybuf[pos]);
                setProtocolError(c, pos);
                return REDIS_ERR;
            }

            ok = string2ll(c->querybuf + pos + 1, newline - (c->querybuf + pos + 1), &ll);
            if (!ok || ll < 0 || ll > 512 * 1024 * 1024) {
                addReplyError(c, "Protocol error: invalid bulk length");
                setProtocolError(c, pos);
                return REDIS_ERR;
            }

            pos += newline - (c->querybuf + pos) + 2;
            if (ll >= REDIS_MBULK_BIG_ARG) {
                size_t qblen;

                /* If we are going to read a large object from network
                 * try to make it likely that it will start at c->querybuf
                 * boundary so that we can optimize object creation
                 * avoiding a large copy of data. */
                sdsrange(c->querybuf, pos, -1);
                pos = 0;
                qblen = sdslen(c->querybuf);
                /* Hint the sds library about the amount of bytes this string is
                 * going to contain. */
                if (qblen < ll + 2)
                    c->querybuf = sdsMakeRoomFor(c->querybuf, ll + 2 - qblen);
            }
            // 一个参数的长度
            c->bulklen = ll;
        }

        /* Read bulk argument */
        if (sdslen(c->querybuf) - pos < (unsigned)(c->bulklen + 2)) {
            /* Not enough data (+2 == trailing \r\n) */
            break;
        } else {
            /* Optimization: if the buffer contains JUST our bulk element
             * instead of creating a new object by *copying* the sds we
             * just use the current sds string. */
            if (pos == 0 && c->bulklen >= REDIS_MBULK_BIG_ARG && (signed)sdslen(c->querybuf) == c->bulklen + 2) {
                c->argv[c->argc++] = createObject(REDIS_STRING, c->querybuf);
                sdsIncrLen(c->querybuf, -2);    /* remove CRLF */
                c->querybuf = sdsempty();
                /* Assume that if we saw a fat argument we'll see another one
                 * likely... */
                c->querybuf = sdsMakeRoomFor(c->querybuf, c->bulklen + 2);
                pos = 0;
            } else {
                c->argv[c->argc++] = createStringObject(c->querybuf + pos, c->bulklen);
                pos += c->bulklen + 2;
            }

            c->bulklen = -1;

            c->multibulklen--;
        }
    }

    /* Trim to pos */
    if (pos) sdsrange(c->querybuf, pos, -1);

    /* We're done when c->multibulk == 0 */
    if (c->multibulklen == 0) return REDIS_OK;

    // 还有参数未读取完，返回REDIS_ERR，下次处理
    /* Still not read to process the command */
    return REDIS_ERR;
}

/*
 * 处理查询缓冲区中的请求
 */
void processInputBuffer(redisClient* c) {

    // 处理查询缓冲区中的内容直至为空，查询缓冲区的内容可能滞留，等待下一次读事件的就绪
    /* Keep processing while there is something in the input buffer */
    while (sdslen(c->querybuf)) {

        // TODO: 客户端暂停相关
        /* Return if clients are paused. */
        /* if (!(c->flags & REDIS_SLAVE) && clientsArePaused()) return; */

        // TODO: 阻塞相关
        /* Immediately abort if the client is in the middle of something. */
        /* if (c->flags & REDIS_BLOCKED) return; */

        // 如果客户端的REDIS_CLOSE_AFTER_REPLY被设置，则该客户端在回复发送出去之后关闭，此处为了确保REDIS_CLOSE_AFTER_REPLY被设置后不处理更多的命令
        /* REDIS_CLOSE_AFTER_REPLY closes the connection once the reply is
         * written to the client. Make sure to not let the reply grow after
         * this flag has been set (i.e. don't process more commands). */
        if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

        /* Determine request type when unknown. */
        // 判断请求的类型，两种类型的区别可以在 Redis 的通讯协议上查到：
        // http://redis.readthedocs.org/en/latest/topic/protocol.html
        // 简单来说，多条查询是一般客户端发送来的，而内联查询则是 TELNET 发送来的
        if (!c->reqtype) {
            if (c->querybuf[0] == '*') {
                // 多行命令
                c->reqtype = REDIS_REQ_MULTIBULK;
            } else {
                // 单行命令
                c->reqtype = REDIS_REQ_INLINE;
            }
        }

        // 根据请求的类型，对命令进行预处理
        if (c->reqtype == REDIS_REQ_INLINE) {
            if (processInlineBuffer(c) != REDIS_OK) break;
        } else if (c->reqtype == REDIS_REQ_MULTIBULK) {
            if (processMultibulkBuffer(c) != REDIS_OK) break;
        } else {
            exit(1);
        }

        /* Multibulk processing could see a <= 0 length. */
        if (c->argc == 0) {
            resetClient(c);
        } else {
            // 预处理完毕后，调用redis.c中的processCommand函数真正执行一条命令
            /* Only reset the client when the command was executed. */
            if (processCommand(c) == REDIS_OK)
                // 执行成功，重置客户端以处理下一命令
                resetClient(c);
        }
    }
}

/*
 * 命令请求处理器
 *
 * 为clientfd绑定的读事件处理器，读取内容到客户端的查询缓冲区中
 */
void readQueryFromClient(aeEventLoop* el, int fd, void* privdata, int mask) {

    // 该文件事件的privdata域是客户端
    redisClient* c = (redisClient*) privdata;
    int nread;
    int readlen;
    size_t qblen;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    server.current_client = c;

    // 一次读取16M
    readlen = REDIS_IOBUF_LEN;

    // 如果正在处理足够大的参数(大于32M)
    /* If this is a multi bulk request, and we are processing a bulk reply
     * that is large enough, try to maximize the probability that the query
     * buffer contains exactly the SDS string representing the object, even
     * at the risk of requiring more read(2) calls. This way the function
     * processMultiBulkBuffer() can avoid copying buffers to create the
     * Redis Object representing the argument. */
    if (c->reqtype == REDIS_REQ_MULTIBULK && c->multibulklen && c->bulklen != -1 && c->bulklen >= REDIS_MBULK_BIG_ARG) {
        int remaining = (unsigned)(c->bulklen + 2) - sdslen(c->querybuf);

        if (remaining < readlen) readlen = remaining;
    }

    // 获取查询缓冲区当前剩余内容的长度
    qblen = sdslen(c->querybuf);
    // 更新统计信息: 查询缓冲区使用内存峰值
    if (c->querybuf_peak < qblen) c->querybuf_peak = qblen;
    // 为查询缓冲区分配足量的额外空间
    c->querybuf = sdsMakeRoomFor(c->querybuf, readlen);
    // 调用read，读取内容到查询缓冲区
    nread = read(fd, c->querybuf + qblen, readlen);

    // ERR
    if (nread == -1) {
        if (errno == EAGAIN) {
            nread = 0;
        } else {
            printf("Reading from client: %s\n", strerror(errno));
            freeClient(c);
            return;
        }
    // EOF
    } else if (nread == 0) {
        printf("Client closed connection\n");
        freeClient(c);
        return;
    }

    if (nread) {
        sdsIncrLen(c->querybuf, nread);
        c->lastinteraction = server.unixtime;
        // TODO: 复制相关
        /* if (c->flags & REDIS_MASTER) c->reploff += nread; */
    } else {
        server.current_client = NULL;
        return;
    }

    // 如果查询缓冲区长度超出服务器最大缓冲区长度，则清空缓冲区并释放客户端
    if (sdslen(c->querybuf) > server.client_max_querybuf_len) {
        sds ci = catClientInfoString(sdsempty(), c);
        sds bytes = sdsempty();

        bytes = sdscatrepr(bytes, c->querybuf, 64);
        printf("Closing client that reached max query buffer length: %s (qbuf initial bytes: %s)\n", ci, bytes);
        sdsfree(ci);
        sdsfree(bytes);
        freeClient(c);
        return;
    }

    // 调用processInputBuffer处理查询缓冲区的请求
    processInputBuffer(c);

    server.current_client = NULL;
}

/*
 * 辅助函数，提供给genClientPeerId函数调用，产生格式化的ip:port对
 */
/* This is a helper function for genClientPeerId().
 * It writes the specified ip/port to "peerid" as a null termiated string
 * in the form ip:port if ip does not contain ":" itself, otherwise
 * [ip]:port format is used (for IPv6 addresses basically). */
void formatPeerId(char *peerid, size_t peerid_len, char *ip, int port) {
    if (strchr(ip,':'))
        snprintf(peerid,peerid_len,"[%s]:%d",ip,port);
    else
        snprintf(peerid,peerid_len,"%s:%d",ip,port);
}

/*
 * 获取客户端连接的ip:port
 */
/* A Redis "Peer ID" is a colon separated ip:port pair.
 * For IPv4 it's in the form x.y.z.k:pork, example: "127.0.0.1:1234".
 * For IPv6 addresses we use [] around the IP part, like in "[::1]:1234".
 * For Unix socekts we use path:0, like in "/tmp/redis:0".
 *
 * A Peer ID always fits inside a buffer of REDIS_PEER_ID_LEN bytes, including
 * the null term.
 *
 * The function returns REDIS_OK on succcess, and REDIS_ERR on failure.
 *
 * On failure the function still populates 'peerid' with the "?:0" string
 * in case you want to relax error checking or need to display something
 * anyway (see anetPeerToString implementation for more info). */
int genClientPeerId(redisClient *client, char *peerid, size_t peerid_len) {
    char ip[REDIS_IP_STR_LEN];
    int port;

    if (client->flags & REDIS_UNIX_SOCKET) {
        /* Unix socket client. */
        snprintf(peerid,peerid_len,"%s:0",server.unixsocket);
        return REDIS_OK;
    } else {
        /* TCP client. */
        int retval = anetPeerToString(client->fd,ip,sizeof(ip),&port);
        formatPeerId(peerid,peerid_len,ip,port);
        return (retval == -1) ? REDIS_ERR : REDIS_OK;
    }
}

/*
 * 获取客户端连接的ip:port，调用genClientPeerId完成实际的工作(用peerid域缓存避免重复调用)
 */
/* This function returns the client peer id, by creating and caching it
 * if client->perrid is NULL, otherwise returning the cached value.
 * The Peer ID never changes during the life of the client, however it
 * is expensive to compute. */
char *getClientPeerId(redisClient *c) {
    char peerid[REDIS_PEER_ID_LEN];

    if (c->peerid == NULL) {
        genClientPeerId(c,peerid,sizeof(peerid));
        c->peerid = sdsnew(peerid);
    }
    return c->peerid;
}

// 获取客户端的各项信息，将它们储存到 sds 值 s 里面，并返回
/* Concatenate a string representing the state of a client in an human
 * readable format, into the sds string 's'. */
sds catClientInfoString(sds s, redisClient *client) {
    char flags[16], events[3], *p;
    int emask;

    p = flags;
    if (client->flags & REDIS_SLAVE) {
        if (client->flags & REDIS_MONITOR)
            *p++ = 'O';
        else
            *p++ = 'S';
    }
    if (client->flags & REDIS_MASTER) *p++ = 'M';
    if (client->flags & REDIS_MULTI) *p++ = 'x';
    if (client->flags & REDIS_BLOCKED) *p++ = 'b';
    if (client->flags & REDIS_DIRTY_CAS) *p++ = 'd';
    if (client->flags & REDIS_CLOSE_AFTER_REPLY) *p++ = 'c';
    if (client->flags & REDIS_UNBLOCKED) *p++ = 'u';
    if (client->flags & REDIS_CLOSE_ASAP) *p++ = 'A';
    if (client->flags & REDIS_UNIX_SOCKET) *p++ = 'U';
    if (client->flags & REDIS_READONLY) *p++ = 'r';
    if (p == flags) *p++ = 'N';
    *p++ = '\0';

    emask = client->fd == -1 ? 0 : aeGetFileEvents(server.el,client->fd);
    p = events;
    if (emask & AE_READABLE) *p++ = 'r';
    if (emask & AE_WRITABLE) *p++ = 'w';
    *p = '\0';
    return sdscatfmt(s,
                     "addr=%s fd=%i name=%s age=%I idle=%I flags=%s db=%i sub=%i psub=%i multi=%i qbuf=%U qbuf-free=%U obl=%U oll=%U omem=%U events=%s cmd=%s",
                     getClientPeerId(client),
                     client->fd,
                     client->name ? (char*)client->name->ptr : "",
                     (long long)(server.unixtime - client->ctime),
                     (long long)(server.unixtime - client->lastinteraction),
                     flags,
                     client->db->id,
                     // TODO: 发布/订阅相关
                     /* (int) dictSize(client->pubsub_channels) */ -1,
                     /* (int) listLength(client->pubsub_patterns) */ -1,
                     // TODO: 事务相关
                     /* (client->flags & REDIS_MULTI) ? client->mstate.count : -1 */ -1,
                     (unsigned long long) sdslen(client->querybuf),
                     (unsigned long long) sdsavail(client->querybuf),
                     (unsigned long long) client->bufpos,
                     (unsigned long long) listLength(client->reply),
                     (unsigned long long) getClientOutputBufferMemoryUsage(client),
                     events,
                     client->lastcmd ? client->lastcmd->name : "NULL");
}

/*
 * 打印出所有连接到服务器的客户端的信息
 */
sds getAllClientsInfoString(void) {
    listNode *ln;
    listIter li;
    redisClient *client;
    sds o = sdsempty();

    o = sdsMakeRoomFor(o,200*listLength(server.clients));
    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        client = listNodeValue(ln);
        o = catClientInfoString(o,client);
        o = sdscatlen(o,"\n",1);
    }
    return o;
}

/*
 * CLIENT命令
 */
void clientCommand(redisClient* c) {
    listNode* ln;
    listIter li;
    redisClient* client;

    // CLIENT LIST
    if (!strcasecmp(c->argv[1]->ptr, "list") && c->argc == 2) {
        sds o = getAllClientsInfoString();
        addReplyBulkCBuffer(c, o, sdslen(o));
        sdsfree(o);

    // CLIENT KILL
    } else if (!strcasecmp(c->argv[1]->ptr, "kill") && c->argc == 3) {

        // 遍历客户端链表，杀死指定地址的客户端
        listRewind(server.clients, &li);
        while ((ln = listNext(&li)) != NULL) {
            char* peerid;

            client = listNodeValue(ln);
            peerid = getClientPeerId(client);
            if (strcmp(peerid, c->argv[2]->ptr) == 0) {
                addReply(c, shared.ok);
                if (c == client) {
                    client->flags |= REDIS_CLOSE_AFTER_REPLY;
                } else {
                    freeClient(client);
                }
                return;
            }
        }
        addReplyError(c, "No such client");

    // CLIENT SETNAME
    } else if (!strcasecmp(c->argv[1]->ptr, "setname") && c->argc == 3) {

        int j;
        int len = sdslen(c->argv[2]->ptr);
        char* p = c->argv[2]->ptr;

        /* Setting the client name to an empty string actually removes
         * the current name. */
        if (len == 0) {
            if (c->name) decrRefCount(c->name);
            c->name = NULL;
            addReply(c, shared.ok);
            return;
        }

        /* Otherwise check if the charset is ok. We need to do this otherwise
         * CLIENT LIST format will break. You should always be able to
         * split by space to get the different fields. */
        for (j = 0; j < len; j++) {
            /* ASCII is assumed. */
            if (p[j] < '!' || p[j] > '~') {
                addReplyError(c, "Client names cannot contain spaces, newlines or special characters.");
                return;
            }
        }
        if (c->name) decrRefCount(c->name);
        c->name = c->argv[2];
        incrRefCount(c->name);
        addReply(c, shared.ok);

    // CLIENT GETNAME
    } else if (!strcasecmp(c->argv[1]->ptr, "getname") && c->argc == 2) {
        if (c->name)
            addReplyBulk(c, c->name);
        else
            addReply(c, shared.nullbulk);

    // TODO: 客户端暂停相关
    // CLIENT PAUSE
    } else if (!strcasecmp(c->argv[1]->ptr, "pause") && c->argc == 3) {
        // long long duration;
        // 
        // if (getTimeoutFromObjectOrReply(c,c->argv[2],&duration,UNIT_MILLISECONDS)
        //     != REDIS_OK) return;
        // pauseClients(duration);
        // addReply(c,shared.ok);
    } else {
        addReplyError(c, "Syntax error, try CLIENT (LIST | KILL ip:port | GETNAME | SETNAME connection-name)");
    }
}

// 修改客户端的参数数组
/* Rewrite the command vector of the client. All the new objects ref count
 * is incremented. The old command vector is freed, and the old objects
 * ref count is decremented. */
void rewriteClientCommandVector(redisClient *c, int argc, ...) {
    va_list ap;
    int j;
    robj **argv; /* The new argument vector */

    // 创建新参数
    argv = zmalloc(sizeof(robj*)*argc);
    va_start(ap,argc);
    for (j = 0; j < argc; j++) {
        robj *a;

        a = va_arg(ap, robj*);
        argv[j] = a;
        incrRefCount(a);
    }
    /* We free the objects in the original vector at the end, so we are
     * sure that if the same objects are reused in the new vector the
     * refcount gets incremented before it gets decremented. */
    // 释放旧参数
    for (j = 0; j < c->argc; j++) decrRefCount(c->argv[j]);
    zfree(c->argv);

    /* Replace argv and argc with our new versions. */
    // 用新参数替换
    c->argv = argv;
    c->argc = argc;
    c->cmd = lookupCommandOrOriginal(c->argv[0]->ptr);
    assert(c->cmd != NULL);
    va_end(ap);
}

// 修改单个参数
/* Rewrite a single item in the command vector.
 * The new val ref count is incremented, and the old decremented. */
void rewriteClientCommandArgument(redisClient *c, int i, robj *newval) {
    robj *oldval;

    assert(i < c->argc);
    oldval = c->argv[i];
    c->argv[i] = newval;
    incrRefCount(newval);
    decrRefCount(oldval);

    /* If this is the command name make sure to fix c->cmd. */
    if (i == 0) {
        c->cmd = lookupCommandOrOriginal(c->argv[0]->ptr);
        assert(c->cmd != NULL);
    }
}

/* This function returns the number of bytes that Redis is virtually
 * using to store the reply still not read by the client.
 * It is "virtual" since the reply output list may contain objects that
 * are shared and are not really using additional memory.
 *
 * 函数返回用于保存目前仍未返回给客户端的回复的虚拟大小（以字节为单位）。
 * 之所以说是虚拟大小，因为回复列表中可能有包含共享的对象。
 *
 * The function returns the total sum of the length of all the objects
 * stored in the output list, plus the memory used to allocate every
 * list node. The static reply buffer is not taken into account since it
 * is allocated anyway.
 *
 * 函数返回回复列表中所包含的全部对象的体积总和，
 * 加上列表节点所分配的空间。
 * 静态回复缓冲区不会被计算在内，因为它总是会被分配的。
 *
 * Note: this function is very fast so can be called as many time as
 * the caller wishes. The main usage of this function currently is
 * enforcing the client output length limits.
 *
 * 注意：这个函数的速度很快，所以它可以被随意地调用多次。
 * 这个函数目前的主要作用就是用来强制客户端输出长度限制。
 */
unsigned long getClientOutputBufferMemoryUsage(redisClient *c) {
    unsigned long list_item_size = sizeof(listNode)+sizeof(robj);

    return c->reply_bytes + (list_item_size*listLength(c->reply));
}

/* Get the class of a client, used in order to enforce limits to different
 * classes of clients.
 *
 * 获取客户端的类型，用于对不同类型的客户端应用不同的限制。
 *
 * The function will return one of the following:
 *
 * 函数将返回以下三个值的其中一个：
 *
 * REDIS_CLIENT_LIMIT_CLASS_NORMAL -> Normal client
 *                                    普通客户端
 *
 * REDIS_CLIENT_LIMIT_CLASS_SLAVE  -> Slave or client executing MONITOR command
 *                                    从服务器，或者正在执行 MONITOR 命令的客户端
 *
 * REDIS_CLIENT_LIMIT_CLASS_PUBSUB -> Client subscribed to Pub/Sub channels
 *                                    正在进行订阅操作（SUBSCRIBE/PSUBSCRIBE）的客户端
 */
int getClientLimitClass(redisClient *c) {
    // TODO: 复制相关
    /* if (c->flags & REDIS_SLAVE) return REDIS_CLIENT_LIMIT_CLASS_SLAVE; */
    // TODO: 发布/订阅相关
    /* if (dictSize(c->pubsub_channels) || listLength(c->pubsub_patterns)) */
    /*     return REDIS_CLIENT_LIMIT_CLASS_PUBSUB; */
    return REDIS_CLIENT_LIMIT_CLASS_NORMAL;
}

/* The function checks if the client reached output buffer soft or hard
 * limit, and also update the state needed to check the soft limit as
 * a side effect.
 *
 * 这个函数检查客户端是否达到了输出缓冲区的软性（soft）限制或者硬性（hard）限制，
 * 并在到达软限制时，对客户端进行标记。
 *
 * Return value: non-zero if the client reached the soft or the hard limit.
 *               Otherwise zero is returned.
 *
 * 返回值：到达软性限制或者硬性限制时，返回非 0 值。
 *         否则返回 0 。
 */
int checkClientOutputBufferLimits(redisClient *c) {
    int soft = 0, hard = 0, class;

    // 获取客户端回复缓冲区的大小
    unsigned long used_mem = getClientOutputBufferMemoryUsage(c);

    // 获取客户端的限制大小
    class = getClientLimitClass(c);

    // 检查硬性限制
    if (server.client_obuf_limits[class].hard_limit_bytes &&
        used_mem >= server.client_obuf_limits[class].hard_limit_bytes)
        hard = 1;

    // 检查软性限制
    if (server.client_obuf_limits[class].soft_limit_bytes &&
        used_mem >= server.client_obuf_limits[class].soft_limit_bytes)
        soft = 1;

    /* We need to check if the soft limit is reached continuously for the
     * specified amount of seconds. */
    // 达到软性限制
    if (soft) {

        // 第一次达到软性限制
        if (c->obuf_soft_limit_reached_time == 0) {
            // 记录时间
            c->obuf_soft_limit_reached_time = server.unixtime;
            // 关闭软性限制 flag
            soft = 0; /* First time we see the soft limit reached */

            // 再次达到软性限制
        } else {
            // 软性限制的连续时长
            time_t elapsed = server.unixtime - c->obuf_soft_limit_reached_time;

            // 如果没有超过最大连续时长的话，那么关闭软性限制 flag
            // 如果超过了最大连续时长的话，软性限制 flag 就会被保留
            if (elapsed <=
                server.client_obuf_limits[class].soft_limit_seconds) {
                soft = 0; /* The client still did not reached the max number of
                             seconds for the soft limit to be considered
                             reached. */
            }
        }
    } else {
        // 未达到软性限制，或者已脱离软性限制，那么清空软性限制的进入时间
        c->obuf_soft_limit_reached_time = 0;
    }

    return soft || hard;
}

/* Asynchronously close a client if soft or hard limit is reached on the
 * output buffer size. The caller can check if the client will be closed
 * checking if the client REDIS_CLOSE_ASAP flag is set.
 *
 * 如果客户端达到输出缓冲区大小的软性或者硬性限制，那么打开客户端的 ``REDIS_CLOSE_ASAP`` 状态，
 * 让服务器异步地关闭客户端。
 *
 * Note: we need to close the client asynchronously because this function is
 * called from contexts where the client can't be freed safely, i.e. from the
 * lower level functions pushing data inside the client output buffers.
 *
 * 注意：
 * 我们不能直接关闭客户端，而要异步关闭的原因是客户端正处于一个不能被安全地关闭的上下文中。
 * 比如说，可能有底层函数正在推入数据到客户端的输出缓冲区里面。
 */
void asyncCloseClientOnOutputBufferLimitReached(redisClient *c) {
    assert(c->reply_bytes < ULONG_MAX-(1024*64));

    // 已经被标记了
    if (c->reply_bytes == 0 || c->flags & REDIS_CLOSE_ASAP) return;

    // 检查客户端输出缓冲区限制
    if (checkClientOutputBufferLimits(c)) {
        sds client = catClientInfoString(sdsempty(),c);

        // 异步关闭
        freeClientAsync(c);
        printf("Client %s scheduled to be closed ASAP for overcoming of output buffer limits.\n", client);
        sdsfree(client);
    }
}
