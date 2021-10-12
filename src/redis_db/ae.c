//
// Created by zouyi on 2021/9/26.
//

#include <sys/time.h>
#include <sys/types.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <poll.h>

/* Include the best multiplexing layer supported by this system.
 * The following should be ordered by performances, descending. */
#include "ae_epoll.c"

/*
 * 事件处理
 */

/*
 * 创建并初始化事件处理器状态，参数setsize指定事件槽大小
 */
aeEventLoop* aeCreateEventLoop(int setsize) {

    aeEventLoop* eventLoop;
    int i;

    // 创建事件处理器状态结构
    if ((eventLoop = zmalloc(sizeof(aeEventLoop))) == NULL) goto err;

    // 初始化文件事件结构和已就绪文件事件结构数组
    eventLoop->events = zmalloc(sizeof(aeFileEvent) * setsize);
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent) * setsize);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;
    // 设置数组大小
    eventLoop->setsize = setsize;

    // 设置最后一次处理时间事件的时间
    eventLoop->lastTime = time(NULL);
    // 初始化时间事件结构
    eventLoop->timeEventHead= NULL;
    // 时间事件id从0开始
    eventLoop->timeEventNextId = 0;

    // 事件处理器开关
    eventLoop->stop = 0;

    eventLoop->maxfd = -1;

    // 在处理事件之前需要执行的函数
    eventLoop->beforesleep = NULL;

    // 创建多路复用库的特有数据
    if (aeApiCreate(eventLoop) == -1) goto err;

    // 初始化监听事件类型
    /* Events with mask == AE_NONE are not set. So let's initialize the
     * vector with it. */
    for (i = 0; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;

    return eventLoop;

err:
    if (eventLoop) {
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }
    return NULL;
}

/*
 * 返回当前事件槽大小
 */
/* Return the current set size. */
int aeGetSetSize(aeEventLoop* eventLoop) {
    return eventLoop->setsize;
}

/*
 * 调整事件槽大小
 */
/* Resize the maximum set size of the event loop.
 *
 * If the requested set size is smaller than the current set size, but
 * there is already a file descriptor in use that is >= the requested
 * set size minus one, AE_ERR is returned and the operation is not
 * performed at all.
 *
 * Otherwise AE_OK is returned and the operation is successful.
 */
int aeResizeSetSize(aeEventLoop* eventLoop, int setsize) {
    int i;

    if (setsize == eventLoop->setsize) return AE_OK;
    if (eventLoop->maxfd >= setsize) return AE_ERR;
    if (aeApiResize(eventLoop, setsize) == -1) return AE_ERR;

    eventLoop->events = zrealloc(eventLoop->events, sizeof(aeFileEvent) * setsize);
    eventLoop->fired = zrealloc(eventLoop->fired, sizeof(aeFiredEvent) * setsize);
    eventLoop->setsize = setsize;

    /* Make sure that if we created new slots, they are initialized with
     * an AE_NONE mask. */
    for (i = eventLoop->maxfd + 1; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return AE_OK;
}

/*
 * 删除事件处理器
 */
void aeDeleteEventLoop(aeEventLoop* eventLoop) {
    aeApiFree(eventLoop);
    zfree(eventLoop->events);
    zfree(eventLoop->fired);
    zfree(eventLoop);
}

/*
 * 停止事件处理器
 */
void aeStop(aeEventLoop* eventLoop) {
    eventLoop->stop = 1;
}

/*
 * 创建文件事件
 * 根据mask参数的值，监听fd文件的状态，当fd可用时，执行proc函数
 */
int aeCreateFileEvent(aeEventLoop* eventLoop, int fd, int mask, aeFileProc* proc, void* clientData) {

    if (fd >= eventLoop->setsize) {
        errno = ERANGE;
        return AE_ERR;
    }

    if (fd >= eventLoop->setsize) return AE_ERR;

    // fd作为索引找到对应的文件事件
    aeFileEvent* fe = &eventLoop->events[fd];

    // 监听指定fd的指定事件类型
    if (aeApiAddEvent(eventLoop, fd, mask) == -1)
        return AE_ERR;

    // 设置文件事件类型，以及事件处理器
    fe->mask |= mask;
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;

    // 设置文件事件私有数据: 如redisClient*，NULL，...
    fe->clientData = clientData;

    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;

    return AE_OK;
}

/*
 * 删除文件事件
 * 取消对给定fd的给定事件的监视
 */
void aeDeleteFileEvent(aeEventLoop* eventLoop, int fd, int mask) {
    if (fd >= eventLoop->setsize) return;

    aeFileEvent* fe = &eventLoop->events[fd];

    if (fe->mask == AE_NONE) return;

    fe->mask = fe->mask & (~mask);
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        /* Update the max fd */
        int j;

        for (j = eventLoop->maxfd - 1; j >= 0; j--)
            if (eventLoop->events[j].mask != AE_NONE) break;
        eventLoop->maxfd = j;
    }

    aeApiDelEvent(eventLoop, fd, mask);
}

/*
 * 获取给定fd正在监听的事件类型
 */
int aeGetFileEvents(aeEventLoop* eventLoop, int fd) {
    if (fd >= eventLoop->setsize) return 0;
    aeFileEvent* fe = &eventLoop->events[fd];

    return fe->mask;
}

/*
 * 获取当前时间(秒和毫秒)，存在*seconds和*milliseconds中
 */
static void aeGetTime(long* seconds, long* milliseconds) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec / 1000;
}

/*
 * 在当前时间上加上milliseconds毫秒，结果存在*sec和*ms中
 */
static void aeAddMillisecondsToNow(long long milliseconds, long* sec, long* ms) {
    long cur_sec;
    long cur_ms;
    long when_sec;
    long when_ms;

    aeGetTime(&cur_sec, &cur_ms);

    when_sec = cur_sec + milliseconds / 1000;
    when_ms = cur_ms + milliseconds % 1000;

    if (when_ms >= 1000) {
        when_sec++;
        when_ms -= 1000;
    }

    *sec = when_sec;
    *ms = when_ms;
}

/*
 * 创建时间事件
 */
long long aeCreateTimeEvent(aeEventLoop* eventLoop, long long milliseconds, aeTimeProc* proc, void* clientData, aeEventFinalizerProc* finalizerProc) {

    // 更新下一个时间事件id
    long long id = eventLoop->timeEventNextId++;

    // 创建时间事件结构
    aeTimeEvent* te;

    te = zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;

    // 设置id
    te->id = id;

    // 设定处理该时间事件的时间，当前时间的milliseconds毫秒后
    aeAddMillisecondsToNow(milliseconds, &te->when_sec, &te->when_ms);
    // 设置时间事件处理器
    te->timeProc = proc;
    // 设置时间事件释放函数
    te->finalizerProc = finalizerProc;
    // 设置时间事件的私有数据
    te->clientData = clientData;
    // 将新时间事件插入表头，redis的时间事件列表是非按时间排序的
    te->next = eventLoop->timeEventHead;
    eventLoop->timeEventHead = te;

    // 返回当前时间事件的id
    return id;
}

/*
 * 删除指定id的时间事件
 */
int aeDeleteTimeEvent(aeEventLoop* eventLoop, long long id) {

    aeTimeEvent* te;
    aeTimeEvent* prev = NULL;

    // 遍历时间事件链表
    te = eventLoop->timeEventHead;
    while (te) {

        // 发现指定id时间事件
        if (te->id == id) {

            // 从时间事件链表中删除该事件
            if (prev == NULL)
                eventLoop->timeEventHead = te->next;
            else
                prev->next = te->next;

            // 执行时间事件释放函数，使用时间事件的私有数据
            if (te->finalizerProc)
                te->finalizerProc(eventLoop, te->clientData);

            // 释放时间事件
            zfree(te);

            return AE_OK;
        }
        prev = te;
        te = te->next;
    }

    return AE_ERR;    /* NO event with the specified ID found */
}

/*
 * 寻找距离当前时间最近的时间事件，处理时间都大于当前时间
 * 由于链表是乱序的，时间服杂度为: O(N)
 */
/* Search the first timer to fire.
 * This operation is useful to know how many time the select can be
 * put in sleep without to delay any event.
 * If there are no timers NULL is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed by Redis so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 */
static aeTimeEvent* aeSearchNearestTimer(aeEventLoop* eventLoop) {
    aeTimeEvent* te = eventLoop->timeEventHead;
    aeTimeEvent* nearest = NULL;

    while (te) {
        if (!nearest || te->when_sec < nearest->when_sec || (te->when_sec == nearest->when_sec && te->when_ms < nearest->when_ms))
            nearest = te;
        te = te->next;
    }
    return nearest;
}

/*
 * 处理时间事件
 */
/*
 * Process time events
 */
static int processTimeEvents(aeEventLoop* eventLoop) {
    int processed = 0;
    aeTimeEvent* te;
    long long maxId;
    time_t now = time(NULL);

    /* If the system clock is moved to the future, and then set back to the
     * right value, time events may be delayed in a random way. Often this
     * means that scheduled operations will not be performed soon enough.
     *
     * Here we try to detect system clock skews, and force all the time
     * events to be processed ASAP when this happens: the idea is that
     * processing events earlier is less dangerous than delaying them
     * indefinitely, and practice suggests it is. */
    if (now < eventLoop->lastTime) {
        te = eventLoop->timeEventHead;
        while (te) {
            te->when_sec = 0;
            te = te->next;
        }
    }
    // 更新最后一次处理时间事件的时间
    eventLoop->lastTime = now;

    // 遍历时间事件链表，处理已经到达的时间事件
    te = eventLoop->timeEventHead;
    maxId = eventLoop->timeEventNextId - 1;
    while (te) {
        long now_sec;
        long now_ms;
        long long id;

        if (te->id > maxId) {
            te = te->next;
            continue;
        }

        // 获取当前时间
        aeGetTime(&now_sec, &now_ms);

        // 若事件已经到达，则执行该事件
        if (now_sec > te->when_sec || (now_sec == te->when_sec && now_ms >= te->when_ms)) {
            int retval;

            id = te->id;
            // 执行时间事件处理器，获取返回值
            retval = te->timeProc(eventLoop, id, te->clientData);
            processed++;

            /* After an event is processed our time event list may
             * no longer be the same, so we restart from head.
             * Still we make sure to don't process events registered
             * by event handlers itself in order to don't loop forever.
             * To do so we saved the max ID we want to handle.
             *
             * FUTURE OPTIMIZATIONS:
             * Note that this is NOT great algorithmically. Redis uses
             * a single time event so it's not a problem but the right
             * way to do this is to add the new elements on head, and
             * to flag deleted elements in a special way for later
             * deletion (putting references to the nodes to delete into
             * another linked list). */

            // retval毫秒后循环执行这个时间事件 
            if (retval != AE_NOMORE) {
                aeAddMillisecondsToNow(retval, &te->when_sec, &te->when_ms);
            // 不需要循环执行这个时间事件
            } else {
                aeDeleteTimeEvent(eventLoop, id);
            }

            // 由于执行时间事件之后，时间事件链表可能发生改变，因此重新将te置为表头，开始执行时间事件
            te = eventLoop->timeEventHead;

        // 事件未到达，判断下一个事件
        } else {
            te = te->next;
        }
    }
    return processed;
}

/* Process every pending time event, then every pending file event
 * (that may be registered by time event callbacks just processed).
 *
 * 处理所有已到达的时间事件，以及所有已就绪的文件事件。
 *
 * Without special flags the function sleeps until some file event
 * fires, or when the next time event occurs (if any).
 *
 * 如果不传入特殊 flags 的话，那么函数睡眠直到文件事件就绪，
 * 或者下个时间事件到达（如果有的话）。
 *
 * If flags is 0, the function does nothing and returns.
 * 如果 flags 为 0 ，那么函数不作动作，直接返回。
 *
 * if flags has AE_ALL_EVENTS set, all the kind of events are processed.
 * 如果 flags 包含 AE_ALL_EVENTS ，所有类型的事件都会被处理。
 *
 * if flags has AE_FILE_EVENTS set, file events are processed.
 * 如果 flags 包含 AE_FILE_EVENTS ，那么处理文件事件。
 *
 * if flags has AE_TIME_EVENTS set, time events are processed.
 * 如果 flags 包含 AE_TIME_EVENTS ，那么处理时间事件。
 *
 * if flags has AE_DONT_WAIT set the function returns ASAP until all
 * the events that's possible to process without to wait are processed.
 * 如果 flags 包含 AE_DONT_WAIT ，
 * 那么函数在处理完所有不许阻塞的事件之后，即刻返回。
 *
 * The function returns the number of events processed.
 * 函数的返回值为已处理事件的数量
 */
int aeProcessEvents(aeEventLoop* eventLoop, int flags) {

    int processed = 0;
    int numevents;

    /* Nothing to do? return ASAP */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /* Note that we want call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */
    if (eventLoop->maxfd != -1 || ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        int j;
        aeTimeEvent* shortest = NULL;
        struct timeval tv;
        struct timeval* tvp;

        // 获取执行时间距离当前最近的时间事件
        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
            shortest = aeSearchNearestTimer(eventLoop);

        if (shortest) {
            long now_sec;
            long now_ms;

            // 计算执行时间距离当前最近的时间事件还需要多久到达
            /* Calculate the time missing for the nearest
             * timer to fire. */
            aeGetTime(&now_sec, &now_ms);
            tvp = &tv;
            tvp->tv_sec = shortest->when_sec - now_sec;
            if (shortest->when_ms < now_ms) {
                tvp->tv_usec = ((shortest->when_ms + 1000) - now_ms) * 1000;
                tvp->tv_sec--;
            } else {
                tvp->tv_usec = (shortest->when_ms - now_ms) * 1000;
            }

            // 时间差小于0，说明时间事件已经可以执行
            if (tvp->tv_sec < 0) tvp->tv_sec = 0;
            if (tvp->tv_usec < 0) tvp->tv_usec = 0;
        } else {

            /* If we have to check for events but need to return
             * ASAP because of AE_DONT_WAIT we need to set the timeout
             * to zero */
            if (flags & AE_DONT_WAIT) {
                // 处理文件事件不阻塞
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {
                // 处理文件事件一直阻塞，直到有文件事件到达为止
                /* Otherwise we can block */
                tvp = NULL;    /* wait forever */
            }
        }

        // 执行文件事件，阻塞时间由tvp指出
        /* 调用aeApiPoll函数得到就绪事件数组 */
        numevents = aeApiPoll(eventLoop, tvp);
        for (j = 0; j < numevents; j++) {
            // 从已就绪文件事件数组中获取文件事件 
            aeFileEvent* fe = &eventLoop->events[eventLoop->fired[j].fd];

            int mask = eventLoop->fired[j].mask;
            int fd = eventLoop->fired[j].fd;
            int rfired = 0;

            // 读文件事件
            /* note the fe->mask & mask & ... code: maybe an already processed
             * event removed an element that fired and we still didn't
             * processed, so we check if the event is still valid. */
            if (fe->mask & mask & AE_READABLE) {
                rfired = 1;
                fe->rfileProc(eventLoop, fd, fe->clientData, mask);
            }
            // 写文件事件
            if (fe->mask & mask & AE_WRITABLE) {
                if (!rfired || fe->wfileProc != fe->rfileProc)
                    fe->wfileProc(eventLoop, fd, fe->clientData, mask);
            }

            processed++;
        }
    }

    // 处理时间事件
    /* Check time events */
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents(eventLoop);

    return processed;    /* return the number of processed file/time events */
}

/*
 * 在给定毫秒内等待，直到fd变为可写、可读或异常
 */
/* Wait for milliseconds until the given file descriptor becomes
 * writable/readable/exception
 */
int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    int retmask = 0;
    int retval;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    if (mask & AE_READABLE) pfd.events |= POLLIN;
    if (mask & AE_WRITABLE) pfd.events |= POLLOUT;

    if ((retval = poll(&pfd, 1, milliseconds)) == 1) {
        if (pfd.revents & POLLIN) retmask |= AE_READABLE;
        if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLERR) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLHUP) retmask |= AE_WRITABLE;
        return retmask;
    } else {
        return retval;
    }
}

/*
 * 事件处理器的主循环
 */
void aeMain(aeEventLoop* eventLoop) {

    eventLoop->stop = 0;

    // 一直处理事件，直到事件处理器状态变为停止
    while (!eventLoop->stop) {

        // 如果有需要在处理事件之前执行的函数，则执行它
        if (eventLoop->beforesleep != NULL)
            eventLoop->beforesleep(eventLoop);

        // 开始处理一轮事件(所有类型)
        aeProcessEvents(eventLoop, AE_ALL_EVENTS);
    }
}

/*
 * 获取底层使用的I/O多路复用库的名称
 */
char* aeGetApiName(void) {
    return aeApiName();
}

/*
 * 设置在处理事件之前需要被执行的函数
 */
void aeSetBeforeSleepProc(aeEventLoop* eventLoop, aeBeforeSleepProc* beforesleep) {
    eventLoop->beforesleep = beforesleep;
}
