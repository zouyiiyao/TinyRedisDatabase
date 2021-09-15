//
// Created by zouyi on 2021/9/15.
//

#ifndef TINYREDISDATABASE_ANET_H
#define TINYREDISDATABASE_ANET_H

#include <sys/types.h>

#define ANET_OK 0
#define ANET_ERR -1
#define ANET_ERR_LEN 256

#define ANET_NONE 0
#define ANET_IP_ONLY (1<<0)

int anetTcpNonBlockConnect(char* err, char* addr, int port);
int anetTcpNonBlockBindConnect(char* err, char* addr, int port, char* source_addr);
int anetRead(int fd, char* buf, int count);
int anetResolve(char* err, char* host, char* ipbuf, size_t ipbuf_len);
int anetResolveIP(char* err, char* host, char* ipbuf, size_t ipbuf_len);
int anetTcpServer(char* err, int port, char* bindaddr, int backlog);
int anetTcp6Server(char* err, int port, char* bindaddr, int backlog);
int anetUnixServer(char* err, char* path, mode_t perm, int backlog);
int anetTcpAccept(char* err, int serversock, char* ip, size_t ip_len, int* port);
int anetUnixAccept(char* err, int serversock);
int anetNonBlock(char* err, int fd);
int anetEnableTcpNoDelay(char* err, int fd);
int anetDisableTcpNoDelay(char* err, int fd);
int anetPeerToString(int fd, char* ip, size_t ip_len, int* port);
int anetKeepAlive(char* err, int fd, int interval);
int anetSockName(int fd, char* ip, size_t ip_len, int* port);

#endif //TINYREDISDATABASE_ANET_H
