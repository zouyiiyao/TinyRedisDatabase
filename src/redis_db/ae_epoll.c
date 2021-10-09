//
// Created by zouyi on 2021/9/26.
//

#include <sys/epoll.h>
#include <sys/time.h>
#include <unistd.h>

#include "ae.h"
#include "zmalloc.h"
#include "config.h"

/*
 * epoll
 *
 * epoll是Linux特有的I/O复用函数，它在实现上与select或poll有很大差异:
 *
 * 1. epoll使用一组函数来完成任务，而不是单个函数；
 * 2. epoll把用户关心的文件描述符上的事件放在内核里的一个事件表中，从而无须像select或poll那样每次调用都要重复传入文件描述符集或事件集；
 * 3. 但epoll需要使用一个额外的文件描述符，来唯一标识内核中的这个事件表，这个文件描述符由epoll_create创建，该函数返回的文件描述符将用作其他所有epoll系统调用的第一个参数，以指定要访问的内核事件表；
 */

/*
 * 事件状态
 */
typedef struct aeApiState {

    // 唯一标识内核中的事件表
    int epfd;

    // 事件槽，用于保存epoll_wait系统调用返回的就绪文件事件
    struct epoll_event* events;

} aeApiState;

/*
 * 创建一个新的epoll实例，并将它赋值给eventLoop
 * eventLoop的apidata字段存放的是一个aeApiState结构体
 */
static int aeApiCreate(aeEventLoop* eventLoop) {

    aeApiState* state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;

    // 初始化事件槽空间，eventLoop的setsize字段指示了事件槽的大小，也就是eventLoop->events & eventLoop->fired数组的大小
    state->events = zmalloc(sizeof(struct epoll_event) * eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }

    // 创建epoll实例，提示内核事件表需要多大

    // epoll_create系统调用: 创建一个epoll文件描述符，唯一标识内核中的事件表
    state->epfd = epoll_create(1024);    /* 1024 is just a hint for the kernel */
    if (state->epfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }

    // 赋值给eventLoop->apidata(多路复用库的特有数据)
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
    // 给定事件，包含文件描述符fd和事件类型信息
    struct epoll_event ee;

    // 如果fd没有关联任何事件，那么这是一个ADD操作；如果已经关联了某个/某些事件，那么这是一个MOD操作；
    /* If the fd was already monitored for some event, we need a MOD
     * operation. Otherwise we need an ADD operation.
     */
    int op = eventLoop->events[fd].mask == AE_NONE ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    // 注册事件到epoll
    // 指定事件类型
    ee.events = 0;
    mask |= eventLoop->events[fd].mask;    /* Merge old events */
    // EPOLLIN: epoll数据可读事件
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    // EPOLLOUT: epoll数据可写事件
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;

    ee.data.u64 = 0;                       /* avoid valgrind warning */
    // 指定事件所从属的目标文件描述符
    ee.data.fd = fd;

    // epoll_ctl系统调用: 用于操作epoll的内核事件表
    /*
     * 函数原型: int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event)
     * 参数epfd : 指定要访问的内核事件表；
     * 参数op   : 指定操作类型(3种: EPOLL_CTL_ADD，往事件表中注册fd上的事件；EPOLL_CTL_MOD，修改fd上的注册事件；EPOLL_CTL_DEL，删除fd上的注册事件；)
     * 参数fd   : 要操作的文件描述符；
     * 参数event: 指定事件类型；
     */
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

    // epoll_wait系统调用: 在一段超时时间内等待一组文件描述符上的事件
    /*
     * 函数原型: int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout)
     * 该函数成功时返回就绪的文件描述符的个数，失败时返回-1并设置errno；
     * 参数epfd     : 指定要访问的内核事件表；
     * 参数events   : epoll_wait函数如果检测到事件，就将所有就绪的事件从内核事件表(由epfd参数指定)中复制到它的第二个参数events指向的数组中，这个数组只用于输出epoll_wait检测到的就绪事件，
     *                而不像select和poll的数组参数那样既用于传入用户注册的事件，又用于输出内核检测到的就绪事件，这就极大地提高了应用程序索引就绪文件描述符的效率；
     * 参数maxevents: 指定最多监听多少个事件，它必须大于0；
     * 参数timeout  : 指定epoll的超时值，单位是毫秒，当timeout为-1时，epoll调用将永远阻塞，直到某个事件发生，当timeout为0时，epoll调用将立即返回；
     */
    retval = epoll_wait(state->epfd, state->events, eventLoop->setsize, tvp ? (tvp->tv_sec * 1000 + tvp->tv_usec / 1000) : -1);

    // 如果就绪的文件事件数目大于0，所有就绪的事件被保存在state->events事件槽中
    if (retval > 0) {
        int j;

        /* 仅遍历就绪的numevents个文件描述符 */
        numevents = retval;
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            struct epoll_event* e = state->events + j;

            if (e->events & EPOLLIN) mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;
            if (e->events & EPOLLERR) mask |= AE_WRITABLE;
            if (e->events & EPOLLHUP) mask |= AE_WRITABLE;

            // 设置已就绪文件事件数组，将就绪的事件从state->events事件槽复制到eventLoop->fired数组(就绪文件事件数组)中
            eventLoop->fired[j].fd = e->data.fd;
            eventLoop->fired[j].mask = mask;
        }
    }

    return numevents;
}

static char* aeApiName(void) {
    return "epoll";
}
