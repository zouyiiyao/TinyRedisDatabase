//
// Created by zouyi on 2021/9/2.
//

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "redis.h"

int main(int argc, char* argv[]) {
    // 创建一个列表类型对象，使用REDIS_ENCODING_ZIPLIST编码，引用计数为1
    robj* l = createZiplistObject();
    printf("%d\n", l->refcount);

    // 创建一个字符串类型对象s1，长度为5，使用REDIS_ENCODING_EMBSTR编码，引用计数为1
    robj* s1 = createStringObject("Hello", 5);
    // REDIS_ENCODING_EMBSTR
    printf("s1 encoding: %s\n", strEncoding(s1->encoding));
    // 将s1添加到列表l的尾部，此时不存在对象嵌套，s1引用计数仍然为1
    listTypePush(l, s1, REDIS_TAIL);
    // REDIS_ENCODING_ZIPLIST
    printf("l encoding: %s\n", strEncoding(l->encoding));
    printf("s1: %p %d\n", s1, s1->refcount);

    // 估计对象的空转时间
    sleep(2);
    printf("%llu\n", estimateObjectIdleTime(s1));

    // 创建一个字符串类型对象s2，长度为65，使用REDIS_ENCODING_RAW编码，引用计数为1
    char tmp1[65];
    memset(tmp1, 'a', 65);
    robj* s2 = createStringObject(tmp1, 65);
    // REDIS_ENCODING_RAW
    printf("s2 encoding: %s\n", strEncoding(s2->encoding));
    // 将s2添加到列表l的尾部，此时列表将转为REDIS_ENCODING_LINKEDLIST编码，为底层存储创建一个双向链表，
    // 将为原来的节点创建一个新对象，并依次放入双向链表，原压缩列表被释放
    listTypePush(l, s2, REDIS_TAIL);
    // REDIS_ENCODING_LINKEDLIST
    printf("l encoding: %s\n", strEncoding(l->encoding));
    // 列表类型对象l中的s2和字符串类型对象s2共享内存，s2引用计数为2
    printf("s2: %p %d\n", s2, s2->refcount);
    // 注意，此时新创建的包含原s1内容的新对象引用计数为1，只有列表l对其引用

    // 迭代列表l
    listTypeIterator* li = listTypeInitIterator(l, 0, REDIS_TAIL);
    listTypeEntry entry;

    while (listTypeNext(li, &entry)) {
        robj* tmp = listTypeGet(&entry);
        printf("%p %lu %d\n", tmp, sdslen(tmp->ptr), tmp->refcount);
        decrRefCount(tmp);
    }

    // 释放列表类型对象l，同时其内嵌的对象引用计数减1(新创建包含原s1内容的对象 & s2)，
    // 执行完毕后，新创建包含原s1内容的对象被释放，s2引用计数为1
    decrRefCount(l);
    // 释放s1
    printf("s1: %p %d\n", s1, s1->refcount);
    // 释放s2
    printf("s2: %p %d\n", s2, s2->refcount);
    decrRefCount(s1);
    decrRefCount(s2);
    return 0;
}
