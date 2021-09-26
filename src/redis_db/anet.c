//
// Created by zouyi on 2021/9/23.
//
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#include "anet.h"

/*
 * 网络编程基础API的封装，不依赖其他模块
 */

/*
 * 打印错误信息，不定参数
 */
static void anetSetError(char* err, const char* fmt, ...) {
    va_list ap;

    if (!err) return;

    // 使用不定参数相关的宏
    va_start(ap, fmt);
    vsnprintf(err, ANET_ERR_LEN, fmt, ap);
    va_end(ap);
}

/*
 * 将fd设置为非阻塞模式: O_NONBLOCK
 * fcntl系统调用是控制文件描述符属性的通用POSIX方法，在网络编程中常用来将一个文件描述符设置为非阻塞
 */
int anetNonBlock(char* err, int fd) {
    int flags;

    /* Set the socket non-blocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be interrupted by a signal. */
    if ((flags = fcntl(fd, F_GETFL)) == -1) {
        anetSetError(err, "fcntl(F_GETFL): %s", strerror(errno));
        return ANET_ERR;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        anetSetError(err, "fcntl(F_SETFL, O_NONBLOCK): %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

/*
 * 打开socket的SO_KEEPALIVE选项，发送周期性保活报文以维持连接
 * getsockopt和setsockopt两个系统调用是专门用来读取和设置socket文件描述符属性的方法
 */
/* Set TCP keep alive option to detect dead peers. The interval option
 * is only used for Linux as we are using Linux-specific APIs to set
 * the probe send time, interval, and count.
 */
int anetKeepAlive(char* err, int fd, int interval) {
    int val = 1;

    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1) {
        anetSetError(err, "setsockopt SO_KEEPALIVE: %s", strerror(errno));
        return ANET_ERR;
    }

    // linux
    /* Default settings are more or less garbage, with the keepalive time
    * set to 7200 by default on Linux. Modify settings to make the feature
    * actually useful. */

    /* Send first probe after interval. */
    val = interval;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) {
        anetSetError(err, "setsockopt TCP_KEEPIDLE: %s\n", strerror(errno));
        return ANET_ERR;
    }

    /* Send next probes after the specified interval. Note that we set the
     * delay as interval / 3, as we send three probes before detecting
     * an error (see the next setsockopt call). */
    val = interval / 3;
    if (val == 0) val = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) {
        anetSetError(err, "setsockopt TCP_KEEPINTVL: %s\n", strerror(errno));
        return ANET_ERR;
    }

    /* Consider the socket in error state after three we send three ACK
     * probes without getting a reply. */
    val = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) {
        anetSetError(err, "setsockopt TCP_KEEPCNT: %s\n", strerror(errno));
        return ANET_ERR;
    }

    return ANET_OK;
}

/*
 * Nagle算法，尽可能发送大块数据，避免网络中充斥许多小数据块
 * 默认情况下，发送数据采用Nagle算法，这样虽然提高了网络吞吐量，但是实时性却降低了，
 * 在一些交互性很强的应用程序来说是不允许的，使用TCP_NODELAY选项可以禁止Nagle算法
 */
static int anetSetTcpNoDelay(char* err, int fd, int val) {
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) == -1) {
        anetSetError(err, "setsockopt TCP_NODELAY: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

/*
 * 打开socket的TCP_NODELAY选项，禁止Nagle算法
 */
int anetEnableTcpNoDelay(char* err, int fd) {
    return anetSetTcpNoDelay(err, fd, 1);
}

/*
 * 启用Nagle算法
 */
int anetDisableTcpNoDelay(char* err, int fd) {
    return anetSetTcpNoDelay(err, fd, 0);
}

/*
 * 解析host的地址，保存到ipbuf中
 */
/* anetGenericResolve() is called by anetResolve() and anetResolveIP() to
 * do the actual work. It resolves the hostname "host" and set the string
 * representation of the IP address into the buffer pointed by "ipbuf".
 *
 * If flags is set to ANET_IP_ONLY the function only resolves hostnames
 * that are actually already IPv4 or IPv6 addresses. This turns the function
 * into a validating / normalizing function. */
int anetGenericResolve(char* err, char* host, char* ipbuf, size_t ipbuf_len, int flags) {
    struct addrinfo hints;
    struct addrinfo* info;
    int rv;

    memset(&hints, 0, sizeof(hints));
    // 表示host参数必须是用字符串表示的IP地址，从而避免DNS查询
    if (flags & ANET_IP_ONLY)
        hints.ai_flags = AI_NUMERICHOST;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;    /* specify socktype to avoid dups */

    // getaddrinfo既能通过主机名获得IP地址，内部使用的是gethostbyname；
    // 也能通过服务名获得端口号，内部使用的是getservbyname；
    if ((rv = getaddrinfo(host, NULL, &hints, &info)) != 0) {
        anetSetError(err, "%s", gai_strerror(rv));
        return ANET_ERR;
    }

    // TCP/IPv4协议族
    if (info->ai_family == AF_INET) {
        struct sockaddr_in* sa = (struct sockaddr_in*)info->ai_addr;
        // inet_ntop将网络字节序整数表示的IP地址转化为字符串表示的IP地址
        inet_ntop(AF_INET, &(sa->sin_addr), ipbuf, ipbuf_len);
    // TCP/IPv6协议族
    } else {
        struct sockaddr_in6* sa = (struct sockaddr_in6*)info->ai_addr;
        inet_ntop(AF_INET6, &(sa->sin6_addr), ipbuf, ipbuf_len);
    }

    freeaddrinfo(info);
    return ANET_OK;
}

int anetResolve(char* err, char* host, char* ipbuf, size_t ipbuf_len) {
    return anetGenericResolve(err, host, ipbuf, ipbuf_len, ANET_NONE);
}

int anetResolveIP(char* err, char* host, char* ipbuf, size_T ipbuf_len) {
    return anetGenericResolve(err, host, ipbuf, ipbuf_len, ANET_IP_ONLY);
}

/*
 * 设置socket的SO_REUSEADDR来强制使用被处于TIME_WAIT状态的连接占用的socket地址
 */
static int anetSetReuseAddr(char* err, int fd) {
    int yes = 1;
    /* Make sure connection-intensive things like the redis benckmark
     * will be able to close/open sockets a zillion of times */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt SO_REUSEADDR: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

/*
 * 创建并返回socket，SOCK_STREAM表示传输层使用TCP协议，SOCK_DGRAM表示传输层使用UDP协议
 */
static int anetCreateSocket(char* err, int domain) {
    int s;
    if ((s = socket(domain, SOCK_STREAM, 0)) == -1) {
        anetSetError(err, "creating socket: %s", strerror(errno));
        return ANET_ERR;
    }

    /* Make sure connection-intensive things like the redis benchmark
     * will be able to close/open sockets a zillion of times */
    if (anetSetReuseAddr(err, s) == ANET_ERR) {
        close(s);
        return ANET_ERR;
    }
    return s;
}

/*
 * 底层连接创建函数
 */
#define ANET_CONNECT_NONE 0
#define ANET_CONNECT_NONBLOCK 1
static int anetTcpGenericConnect(char* err, char* addr, int port, char* source_addr, int flags) {

    int s = ANET_ERR;
    int rv;
    char portstr[6];
    struct addrinfo hints;
    struct addrinfo* servinfo;
    struct addrinfo* bservinfo;
    struct addrinfo* p;
    struct addrinfo* b;

    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(addr, portstr, &hints, &servinfo)) != 0) {
        anetSetError(err, "%s", gai_strerror(rv));
        return ANET_ERR;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        /* Try to create the socket and to connect it.
         * If we fail in the socket() call, or on connect(), we retry with
         * the next entry in servinfo. */
        if ((s = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
            continue;
        if (anetSetReuseAddr(err, s) == ANET_ERR) goto error;
        if (flags & ANET_CONNECT_NONBLOCK && anetNonBlock(err, s) != ANET_OK)
            goto error;
        if (source_addr) {
            int bound = 0;
            /* Using getaddrinfo saves us from self-determining IPv4 vs IPv6 */
            if ((rv = getaddrinfo(source_addr, NULL, &hints, &bservinfo)) != 0) {
                anetSetError(err, "%s", gai_strerror(rv));
                goto end;
            }
            for (b = bservinfo; b != NULL; b = b->ai_next) {
                if (bind(s, b->ai_addr, b->ai_addrlen) != -1) {
                    bound = 1;
                    break;
                }
            }
            if (!bound) {
                anetSetError(err, "bind: %s", strerror(errno));
                goto end;
            }
        }
        if (connect(s, p->ai_addr, p->ai_addrlen) == -1) {
            /* If the socket is non-blocking, it is ok for connect() to
             * return an EINPROGRESS error here. */
            if (errno == EINPROGRESS && flags & ANET_CONNECT_NONBLOCK)
                goto end;
            close(s);
            s = ANET_ERR;
            continue;
        }

        /* If we ended an iteration of the for loop without errors, we
         * have a connected socket. Let's return to the caller. */
        goto end;
    }
    if (p == NULL)
        anetSetError(err, "creating socket: %s", strerror(errno));

error:
    if (s != ANET_ERR) {
        close(s);
        s = ANET_ERR;
    }
end:
    freeaddrinfo(servinfo);
    return s;
}

/*
 * 创建阻塞TCP连接
 */
int anetTcpConnect(char* err, char* addr, int port) {
    return anetTcpGenericConnect(err, addr, port, NULL, ANET_CONNECT_NONE);
}

/*
 * 创建非阻塞TCP连接
 */
int anetTcpNonBlockConnect(char* err, char* addr, int port) {
    return anetTcpGenericConnect(err, addr, port, NULL, ANET_CONNECT_NONBLOCK);
}

int anetTcpNonBlockBindConnect(char* err, char* addr, int port, char* source_addr) {
    return anetTcpGenericConnect(err, addr, port, source_addr, ANET_CONNECT_NONBLOCK);
}

/*
 * 底层本地连接创建函数
 */
int anetUnixGenericConnect(char* err, char* path, int flags) {

    int s;
    struct sockaddr_un sa;

    if ((s = anetCreateSocket(err, AF_LOCAL)) == ANET_ERR)
        return ANET_ERR;

    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    if (flags & ANET_CONNECT_NONBLOCK) {
        if (anetNonBlock(err, s) != ANET_OK)
            return ANET_ERR;
    }
    if (connect(s, (struct sockaddr*)&sa, sizeof(sa)) == -1) {
        if (errno == EINPROGRESS && flags & ANET_CONNECT_NONBLOCK)
            return s;

        anetSetError(err, "connect: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return s;
}

/*
 * 创建阻塞本地连接
 */
int anetUnixConnect(char* err, char* path) {
    return anetUnixGenericConnect(err, path, ANET_CONNECT_NONE);
}

/*
 * 创建非阻塞本地连接
 */
int anetUnixNonBlockConnect(char* err, char* path) {
    return anetUnixGenericConnect(err, path, ANET_CONNECT_NONBLOCK);
}

/* Like read(2) but make sure 'count' is read before to return
 * (unless error or EOF condition is encountered) */
int anetRead(int fd, char* buf, int count) {
    int nread;
    int totlen = 0;
    while (totlen != count) {
        nread = read(fd, buf, count - totlen);
        if (nread == 0) return totlen;
        if (nread == -1) return -1;
        totlen += nread;
        buf += nread;
    }
    return totlen;
}

/* Like write(2) but make sure 'count' is read before to return
 * (unless error is encountered) */
int anetWrite(int fd, char* buf, int count) {
    int nwritten;
    int totlen = 0;
    while (totlen != count) {
        nwritten = write(fd, buf, count - totlen);
        if (nwritten == 0) return totlen;
        if (nwritten == -1) return -1;
        totlen += nwritten;
        buf += nwritten;
    }
    return totlen;
}

/*
 * 为socket绑定地址并开始监听
 */
static int anetListen(char* err, int s, struct sockaddr* sa, socklen_t len, int backlog) {
    if (bind(s, sa, len) == -1) {
        anetSetError(err, "bind: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }

    if (listen(s, backlog) == -1) {
        anetSetError(err, "listen: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return ANET_OK;
}

static int anetV6Only(char* err, int s) {
    int yes = 1;
    if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return ANET_OK;
}

static int _anetTcpServer(char* err, int port, char* bindaddr, int af, int backlog) {
    int s;
    int rv;
    char _port[6];
    struct addrinfo hints;
    struct addrinfo* servinfo;
    struct addrinfo* p;

    snprintf(_port, 6, "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = af;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;    /* No effect if bindaddr != NULL */

    if ((rv = getaddrinfo(bindaddr, _port, &hints, &servinfo)) != 0) {
        anetSetError(err, "%s", gai_strerror(rv));
        return ANET_ERR;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((s = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
            continue;

        if (af == AF_INET6 && anetV6Only(err, s) == ANET_ERR) goto error;
        if (anetSetReuseAddr(err, s) == ANET_ERR) goto error;
        if (anetListen(err, s, p->ai_addr, p->ai_addrlen, backlog) == ANET_ERR) goto error;
        goto end;
    }
    if (p == NULL) {
        anetSetError(err, "unable to bind socket");
        goto error;
    }

error:
    s = ANET_ERR;
end:
    freeaddrinfo(servinfo);
    return s;
}

int anetTcpServer(char* err, int port, char* bindaddr, int backlog) {
    return _anetTcpServer(err, port, bindaddr, AF_INET, backlog);
}

int anetTcp6Server(char* err, int port, char* bindaddr, int backlog) {
    return _anetTcpServer(err, port, bindaddr, AF_INET6, backlog);
}

/*
 * 创建一个本地连接使用的服务器监听套接字
 */
int anetUnixServer(char* err, char* path, mode_t perm, int backlog) {
    int s;
    struct sockaddr_un sa;

    if ((s = anetCreateSocket(err, AF_LOCAL)) == ANET_ERR)
        return ANET_ERR;

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    if (anetListen(err, s, (struct sockaddr*)&sa, sizeof(sa), backlog) == ANET_ERR)
        return ANET_ERR;
    if (perm)
        chmod(sa.sun_path, perm);
    return s;
}

static int anetGenericAccept(char* err, int s, struct sockaddr* sa, socklen_t* len) {
    int fd;
    while (1) {
        fd = accept(s, sa, len);
        if (fd == -1) {
            if (errno == EINTR)
                continue;
            else {
                anetSetError(err, "accept: %s", strerror(errno));
                return ANET_ERR;
            }
        }
        break;
    }
    return fd;
}

/*
 * TCP accept
 */
int anetTcpAccept(char* err, int s, char* ip, size_t ip_len, int* port) {
    int fd;
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);
    if ((fd = anetGenericAccept(err, s, (struct sockaddr*)&sa, &salen)) == -1)
        return ANET_ERR;

    if (sa.ss_family == AF_INET) {
        struct sockaddr_in* s = (struct sockaddr_in*)&sa;
        if (ip) inet_ntop(AF_INET, (void*)&(s->sin_addr), ip, ip_len);
        if (port) *port = ntohs(s->sin_port);
    } else {
        struct sockaddr_in6* s = (struct sockaddr_in6*)&sa;
        if (ip) inet_ntop(AF_INET6, (void*)&(s->sin6_addr), ip, ip_len);
        if (port) *port = ntohs(s->sin6_port);
    }
    return fd;
}

/*
 * Local accept
 */
int anetUnixAccept(char* err, int s) {
    int fd;
    struct sockaddr_un sa;
    socklen_t salen = sizeof(sa);
    if ((fd = anetGenericAccept(err, s, (struct sockaddr*)&sa, &salen)) == -1)
        return ANET_ERR;

    return fd;
}

int anetPeerToString(int fd, char* ip, size_t ip_len, int* port) {
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);

    if (getpeername(fd, (struct sockaddr*)&sa, &salen) == -1) {
        if (port) *port = 0;
        ip[0] = '?';
        ip[1] = '\0';
        return -1;
    }
    if (sa.ss_family == AF_INET) {
        struct sockaddr_in* s = (struct sockaddr_in*)&sa;
        if (ip) inet_ntop(AF_INET, (void*)&(s->sin_addr), ip, ip_len);
        if (port) *port = ntohs(s->sin_port);
    } else {
        struct sockaddr_in6* s = (struct sockaddr_in6*)&sa;
        if (ip) inet_ntop(AF_INET6, (void*)&(s->sin6_addr), ip, ip_len);
        if (port) *port = ntohs(s->sin6_port);
    }
    return 0;
}

int anetSockName(int fd, char* ip, size_t ip_len, int* port) {
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);

    if (getsockname(fd, (struct sockaddr*)&sa, &salen) == -1) {
        if (port) *port = 0;
        ip[0] = '?';
        ip[1] = '\0';
        return -1;
    }
    if (sa.ss_family == AF_INET) {
        struct sockaddr_in* s = (struct sockaddr_in*)&sa;
        if (ip) inet_ntop(AF_INET, (void*)&(s->sin_addr), ip, ip_len);
        if (port) *port = ntohs(s->sin_port);
    } else {
        struct sockaddr_in6* s = (struct sockaddr_in6*)&sa;
        if (ip) inet_ntop(AF_INET6, (void*)&(s->sin6_addr), ip, ip_len);
        if (port) *port = ntohs(s->sin6_port);
    }
    return 0;
}
