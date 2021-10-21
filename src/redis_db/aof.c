//
// Created by zouyi on 2021/10/21.
//

#include "redis.h"
#include "bio.h"

void flushAppendOnlyFile(int force) {
    // TODO
}

void feedAppendOnlyFile(struct redisCommand* cmd, int dictid, robj** argv, int argc) {
    // TODO
}

void aofRemoveTempFile(pid_t childpid) {
    // TODO
}

int rewriteAppendOnlyFileBackground(void) {
    // TODO
    return REDIS_OK;
}

int loadAppendOnlyFile(char* filename) {
    // TODO
    return REDIS_OK;
}

void stopAppendOnly(void) {
    // TODO
}

void startAppendOnly(void) {
    // TODO
}

void backgroundRewriteDoneHandler(int exitcode, int bysignal) {
    // TODO
}

void aofRewriteBufferReset(void) {
    // TODO
}

unsigned long aofRewriteBufferSize(void) {
    // TODO
    return 0;
}
