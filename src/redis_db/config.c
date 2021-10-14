//
// Created by zouyi on 2021/9/29.
//

#include "redis.h"

clientBufferLimitsConfig clientBufferLimitsDefaults[REDIS_CLIENT_LIMIT_NUM_CLASSES] = {
        {0, 0, 0}, /* normal */
        {1024*1024*256, 1024*1024*64, 60}, /* slave */
        {1024*1024*32, 1024*1024*8, 60}  /* pubsub */
};

void appendServerSaveParams(time_t seconds, int changes) {
    server.saveparams = zrealloc(server.saveparams,sizeof(struct saveparam)*(server.saveparamslen+1));
    server.saveparams[server.saveparamslen].seconds = seconds;
    server.saveparams[server.saveparamslen].changes = changes;
    server.saveparamslen++;
}

void resetServerSaveParams() {
    zfree(server.saveparams);
    server.saveparams = NULL;
    server.saveparamslen = 0;
}