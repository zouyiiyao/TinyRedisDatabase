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
#define AE_NONE
#define AE_READABLE 1
#define AE_WRITABLE 2

/* 文件事件 */
#define AE_FILE_EVENTS 1
/* 时间事件 */
#define AE_TIME_EVENTS 2
/* 所有事件 */
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
#define AE_DONT_WAIT 4

#define AE_NOMORE -1

/*
 * 事件处理器状态
 */
struct aeEventLoop;

/*
 * 事件接口
 */
typedef void aeFileProc(struct aeEventLoop* eventLoop, int fd, void* clientData, int mask);
typedef int aeTimeProc(struct aeEventLoop* eventLoop, long long id, void* clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop* eventLoop, void* clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop* eventLoop);

/*
 * 文件事件结构体定义
 */
typedef struct aeFileEvent {

    int mask;

    aeFileProc* rfileProc;

    aeFileProc* wfileProc;

    void* clientData;

} aeFileEvent;

/*
 * 时间事件结构体定义
 */
typedef struct aeTimeEvent {

    long long id;    /* time event identifier */

    long when_sec;

    long when_ms;

    aeTimeProc* timeProc;

    aeEventFinalizerProc* finalizerProc;

    void* clientData;

    struct aeTimeEvent* next;

} aeTimeEvent;

/*
 * 已就绪事件结构体定义
 */
typedef struct aeFiredEvent {

    int fd;

    int mask;

} aeFiredEvent;

/*
 * 事件处理器状态结构体定义
 */
typedef struct aeEventLoop {

    int maxfd;

    int setsize;

    long long timeEventNextId;

    time_t lastTime;

    aeFileEvent* events;

    aeFiredEvent* fired;

    aeTimeEvent* timeEventHead;

    int stop;

    void* apidata;    /* This is used for polling API specific data */

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
