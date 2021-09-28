//
// Created by zouyi on 2021/9/26.
//

#include <sys/epoll.h>
#include <unistd.h>

#include "ae.h"
#include "zmalloc.h"
#include "config.h"

/*
 * epoll
 */

/*
 * 事件状态
 */
typedef struct aeApiState {

    int epfd;

    // 事件槽
    struct epoll_event* events;

} aeApiState;

/*
 * 创建一个新的epoll实例，并将它赋值给eventLoop
 */
static int aeApiCreate(aeEventLoop* eventLoop) {

    aeApiState* state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;

    // 初始化事件槽空间
    state->events = zmalloc(sizeof(struct epoll_event) * eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }

    // 创建epoll实例
    state->epfd = epoll_create(1024);    /* 1024 is just a hint for the kernel */
    if (state->epfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }

    // 赋值给eventLoop
    eventLoop->apidata = state;
    return 0;
}

/*
 * 调整事件槽大小
 */
static int aeApiResize(aeEventLoop* eventLoop, int setsize) {
    aeApiState* state = eventLoop->apidata;

    state->events = zrealloc(state->events, sizeof(struct epoll_event) * setsize);
    return 0;
}

/*
 * 释放epoll实例和事件槽
 */
static void aeApiFree(aeEventLoop* eventLoop) {
    aeApiState* state = eventLoop->apidata;

    close(state->epfd);
    zfree(state->events);
    zfree(state);
}

/*
 * 关联给定事件到fd
 */
static int aeApiAddEvent(aeEventLoop* eventLoop, int fd, int mask) {
    aeApiState* state = eventLoop->apidata;
    struct epoll_event ee;

    // 如果fd没有关联任何事件，那么这是一个ADD操作；如果已经关联了某个/某些事件，那么这是一个MOD操作；
    /* If the fd was already monitored for some event, we need a MOD
     * operation. Otherwise we need an ADD operation.
     */
    int op = eventLoop->events[fd].mask == AE_NONE ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    // 注册事件到epoll
    ee.events = 0;
    mask |= eventLoop->events[fd].mask;    /* Merge old events */
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.u64 = 0;    /* avoid valgrind warning */
    ee.data.fd = fd;

    if (epoll_ctl(state->epfd, op, fd, &ee) == -1) return -1;

    return 0;
}

/*
 * 从fd中删除给定事件
 */
static void aeApiDelEvent(aeEventLoop* eventLoop, int fd, int delmask) {
    aeApiState* state = eventLoop->apidata;
    struct epoll_event ee;

    int mask = eventLoop->events[fd].mask & (~delmask);

    ee.events = 0;
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.u64 = 0;    /* avoid valgrind warning */
    ee.data.fd = fd;

    if (mask != AE_NONE) {
        epoll_ctl(state->epfd, EPOLL_CTL_MOD, fd, &ee);
    } else {
        /* Note, Kernel < 2.6.9 requires a non null event pointer even for
         * EPOLL_CTL_DEL. */
        epoll_ctl(state->epfd, EPOLL_CTL_DEL, fd, &ee);
    }
}

/*
 * 获取可执行事件
 */
static int aeApiPoll(aeEventLoop* eventLoop, struct timeval* tvp) {
    aeApiState* state = eventLoop->apidata;
    int retval;
    int numevents = 0;

    retval = epoll_wait(state->epfd, state->events, eventLoop->setsize, tvp ? (tvp->tv_sec * 1000 + tvp->tv_usec / 1000) : -1);

    if (retval > 0) {
        int j;

        numevents = retval;
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            struct epoll_event* e = state->events + j;

            if (e->events & EPOLLIN) mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;
            if (e->events & EPOLLERR) mask |= AE_WRITABLE;
            if (e->events & EPOLLHUP) mask |= AE_WRITABLE;

            eventLoop->fired[j].fd = e->data.fd;
            eventLoop->fired[j].mask = mask;
        }
    }

    return numevents;
}

static char* aeApiName(void) {
    return "epoll";
}
