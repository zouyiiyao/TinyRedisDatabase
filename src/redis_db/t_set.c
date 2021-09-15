//
// Created by zouyi on 2021/9/5.
//

#include "redis.h"

/*
 * Set API
 */

/*
 * 以能够保存对象value的值的编码方式创建一个集合
 * 如果对象value的值能表示为long long，则使用REDIS_ENCODING_INTSET编码；
 * 否则使用REDIS_ENCODING_HT编码；
 */
robj* setTypeCreate(robj* value) {

    if (isObjectRepresentableAsLongLong(value, NULL) == REDIS_OK)
        return createIntsetObject();

    return createSetObject();
}

/*
 * 向指定集合中添加一个元素
 */
int setTypeAdd(robj* subject, robj* value) {
    long long llval;

    // 如果集合类型对象底层编码为REDIS_ENCODING_HT，则调用字典API: dictAdd
    if (subject->encoding == REDIS_ENCODING_HT) {
        if (dictAdd(subject->ptr, value, NULL) == DICT_OK) {
            incrRefCount(value);
            return 1;
        }
    // 如果集合类型对象底层编码为REDIS_ENCODING_INTSET，且value的值能表示成long long(intset能保存)，
    // 则调用整数集合API: intsetAdd，如果添加后元素数目超过SET_MAX_INTSET_ENTRIES，则转为REDIS_ENCODING_HT编码；
    // 若value的值不能表示成long long，先转为REDIS_ENCODING_HT编码，再调用字典API: dictAdd；
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {

        if (isObjectRepresentableAsLongLong(value, &llval) == REDIS_OK) {
            uint8_t success = 0;
            subject->ptr = intsetAdd(subject->ptr, llval, &success);
            if (success) {
                if (intsetLen(subject->ptr) > SET_MAX_INTSET_ENTRIES)
                    setTypeConvert(subject, REDIS_ENCODING_HT);
                return 1;
            }
        } else {
            setTypeConvert(subject, REDIS_ENCODING_HT);

            assert(dictAdd(subject->ptr, value, NULL) == DICT_OK);
            incrRefCount(value);
            return 1;
        }
    } else {
        exit(1);
    }

    return 0;
}

/*
 * 在指定集合中删除元素value
 */
int setTypeRemove(robj* setobj, robj* value) {
    long long llval;

    if (setobj->encoding == REDIS_ENCODING_HT) {
        if (dictDelete(setobj->ptr, value) == DICT_OK) {
            if (htNeedsResize(setobj->ptr)) dictResize(setobj->ptr);
            return 1;
        }
    } else if (setobj->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value, &llval) == REDIS_OK) {
            int success;
            setobj->ptr = intsetRemove(setobj->ptr, llval, &success);
            if (success) return 1;
        }
    } else {
        exit(1);
    }

    return 0;
}

/*
 * 检查元素value是否在集合中
 */
int setTypeIsMember(robj* subject, robj* value) {
    long long llval;

    if (subject->encoding == REDIS_ENCODING_HT) {
        return dictFind((dict*)subject->ptr, value) != NULL;
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value, &llval) == REDIS_OK) {
            return intsetFind((intset*)subject->ptr, llval);
        }
    } else {
        exit(1);
    }

    return 0;
}

/*
 * 初始化一个集合类型对象迭代器
 */
setTypeIterator* setTypeInitIterator(robj* subject) {

    setTypeIterator* si = zmalloc(sizeof(setTypeIterator));

    si->subject = subject;

    si->encoding = subject->encoding;

    if (si->encoding == REDIS_ENCODING_HT) {
        si->di = dictGetIterator(subject->ptr);
    } else if (si->encoding == REDIS_ENCODING_INTSET) {
        si->ii = 0;
    } else {
        exit(1);
    }

    return si;
}

/*
 * 释放一个集合类型对象迭代器
 */
void setTypeReleaseIterator(setTypeIterator* si) {

    if (si->encoding == REDIS_ENCODING_HT)
        dictReleaseIterator(si->di);

    zfree(si);
}

/*
 * 取迭代器当前指向的对象，返回集合类型对象的编码方式
 * 这个函数不增加引用计数，所以是copy-on-write友好的
 */
int setTypeNext(setTypeIterator* si, robj** objele, int64_t* llele) {

    if (si->encoding == REDIS_ENCODING_HT) {

        dictEntry* de = dictNext(si->di);

        if (de == NULL) return -1;

        *objele = dictGetKey(de);
    } else if (si->encoding == REDIS_ENCODING_INTSET) {
        if (!intsetGet(si->subject->ptr, si->ii++, llele))
            return -1;
    }

    return si->encoding;
}

/*
 * 取迭代器当前指向的对象，会返回一个新的或者增加引用计数的对象(非copy-on-write友好)
 * 调用者在使用完对象之后，应该对对象调用decrRefCount
 */
robj* setTypeNextObject(setTypeIterator* si) {
    int64_t intele;
    robj* objele;
    int encoding;

    encoding = setTypeNext(si, &objele, &intele);

    switch (encoding) {
        case -1:
            return NULL;
        case REDIS_ENCODING_INTSET:
            return createStringObjectFromLongLong(intele);
        case REDIS_ENCODING_HT:
            incrRefCount(objele);
            return objele;
        default:
            exit(1);
    }
}

/*
 * 从集合类型对象中随机取出一个元素
 */
int setTypeRandomElement(robj* setobj, robj** objele, int64_t* llele) {

    if (setobj->encoding == REDIS_ENCODING_HT) {
        dictEntry* de = dictGetRandomKey(setobj->ptr);
        *objele = dictGetKey(de);
    } else if (setobj->encoding == REDIS_ENCODING_INTSET) {
        *llele = intsetRandom(setobj->ptr);
    } else {
        exit(1);
    }

    return setobj->encoding;
}

/*
 * 返回集合类型对象的元素个数
 */
unsigned long setTypeSize(robj* subject) {

    if (subject->encoding == REDIS_ENCODING_HT) {
        return dictSize((dict*)subject->ptr);
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        return intsetLen((intset*)subject->ptr);
    } else {
        exit(1);
    }
}

/*
 * 将集合类型对象底层编码从REDIS_ENCODING_INTSET转REDIS_ENCODING_HT
 */
void setTypeConvert(robj* setobj, int enc) {

    setTypeIterator* si;

    assert(setobj->type == REDIS_SET && setobj->encoding == REDIS_ENCODING_INTSET);

    if (enc == REDIS_ENCODING_HT) {
        int64_t intele;
        dict* d = dictCreate(&setDictType, NULL);
        robj* element;

        dictExpand(d, intsetLen(setobj->ptr));

        si = setTypeInitIterator(setobj);
        while (setTypeNext(si, NULL, &intele) != -1) {
            element = createStringObjectFromLongLong(intele);
            assert(dictAdd(d, element, NULL) == DICT_OK);
        }
        setTypeReleaseIterator(si);

        setobj->encoding = REDIS_ENCODING_HT;
        zfree(setobj->ptr);
        setobj->ptr = d;
    } else {
        exit(1);
    }
}
