//
// Created by zouyi on 2021/9/15.
//

#ifndef TINYREDISDATABASE_AE_H
#define TINYREDISDATABASE_AE_H

/*
 * 事件执行状态
 */
#define AE_OK 0
#define AE_ERR -1

/*
 * 文件事件状态
 */
// 无
#define AE_NONE 0
// 可读
#define AE_READABLE 1
// 可写
#define AE_WRITABLE 2

/* 文件事件 */
#define AE_FILE_EVENTS 1
/* 时间事件 */
#define AE_TIME_EVENTS 2
/* 所有事件 */
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
/* 不阻塞 */
#define AE_DONT_WAIT 4

#define AE_NOMORE -1

/*
 * 事件处理器状态
 */
struct aeEventLoop;

/*
 * 事件接口
 */
// 文件事件处理器
typedef void aeFileProc(struct aeEventLoop* eventLoop, int fd, void* clientData, int mask);
// 时间事件处理器
typedef int aeTimeProc(struct aeEventLoop* eventLoop, long long id, void* clientData);
// 时间事件释放函数
typedef void aeEventFinalizerProc(struct aeEventLoop* eventLoop, void* clientData);
// 在处理事件前要执行的函数
typedef void aeBeforeSleepProc(struct aeEventLoop* eventLoop);

/*
 * 文件事件结构体定义
 */
typedef struct aeFileEvent {

    // 监听事件类型掩码，值可以是AE_READABLE或AE_WRITABLE或AE_READABLE|AE_WRITABLE
    int mask;

    // 读事件处理器
    aeFileProc* rfileProc;

    // 写事件处理器
    aeFileProc* wfileProc;

    // 多路复用库的私有数据
    void* clientData;

} aeFileEvent;

/*
 * 时间事件结构体定义
 */
typedef struct aeTimeEvent {

    // 时间事件的唯一标识符
    long long id;    /* time event identifier */

    // 事件的到达时间，秒部分
    long when_sec;

    // 事件的到达时间，毫秒部分
    long when_ms;

    // 时间事件处理函数
    aeTimeProc* timeProc;

    // 时间事件释放函数
    aeEventFinalizerProc* finalizerProc;

    // 多路复用库的私有数据
    void* clientData;

    // 指向下一个时间事件结构，形成链表
    struct aeTimeEvent* next;

} aeTimeEvent;

/*
 * 已就绪事件结构体定义
 */
typedef struct aeFiredEvent {

    // 已就绪文件描述符
    int fd;

    // 事件类型掩码，值可以是AE_READABLE或AE_WRITABLE或AE_READABLE|AE_WRITABLE
    int mask;

} aeFiredEvent;

/*
 * 事件处理器状态结构体定义
 */
typedef struct aeEventLoop {

    // 目前已注册的最大描述符
    int maxfd;        /* highest file descriptor currently registered */

    // 目前已追踪的最大描述符
    int setsize;      /* max number of file descriptors tracked */

    // 用于生成时间事件id
    long long timeEventNextId;

    // 最后一次执行时间事件的时间
    time_t lastTime;

    // 已注册的文件事件，数组
    aeFileEvent* events;

    // 已就绪的文件事件，数组
    aeFiredEvent* fired;

    // 时间事件，链表
    aeTimeEvent* timeEventHead;

    // 事件处理器开关
    int stop;

    // 多路复用库的私有数据
    void* apidata;    /* This is used for polling API specific data */

    // 在处理事件前要执行的函数
    aeBeforeSleepProc* beforesleep;

} aeEventLoop;

/* Prototypes */
aeEventLoop* aeCreateEventLoop(int setsize);
void aeDeleteEventLoop(aeEventLoop* eventLoop);
void aeStop(aeEventLoop* eventLoop);
int aeCreateFileEvent(aeEventLoop* eventLoop, int fd, int mask, aeFileProc* proc, void* clientData);
void aeDeleteFileEvent(aeEventLoop* eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop* eventLoop, int fd);
long long aeCreateTimeEvent(aeEventLoop* eventLoop, long long milliseconds, aeTimeProc* proc, void* clientData, aeEventFinalizerProc* finalizerProc);
int aeDeleteTimeEvent(aeEventLoop* eventLoop, long long id);
int aeProcessEvents(aeEventLoop* eventLoop, int flags);
int aeWait(int fd, int mask, long long milliseconds);
void aeMain(aeEventLoop* eventLoop);
char* aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop* eventLoop, aeBeforeSleepProc* beforesleep);
int aeGetSetSize(aeEventLoop* eventLoop);
int aeResizeSetSize(aeEventLoop* eventLoop, int setsize);

#endif //TINYREDISDATABASE_AE_H
