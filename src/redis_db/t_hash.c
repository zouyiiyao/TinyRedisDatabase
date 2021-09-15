//
// Created by zouyi on 2021/9/7.
//

#include "redis.h"

/*
 * Hash API
 */

/*
 * 检查argv数组中的多个对象，是否需要将哈希类型对象底层编码由REDIS_ENCODING_ZIPLIST转为REDIS_ENCODING_HT
 */
void hashTypeTryConversion(robj* o, robj** argv, int start, int end) {
    int i;

    if (o->encoding != REDIS_ENCODING_ZIPLIST) return;

    for (i = start; i <= end; i++) {
        if (sdsEncodesObject(argv[i]) && sdslen(argv[i]->ptr) > HASH_MAX_ZIPLIST_VALUE) {
            // 将哈希类型对象底层编码转化为REDIS_ENCODING_HT
            hashTypeConvert(o, REDIS_ENCODING_HT);
            break;
        }
    }
}

/*
 * 从底层编码为REDIS_ENCODING_ZIPLIST的哈希类型对象中取出field键对应的值
 */
int hashTypeGetFromZiplist(robj* o, robj* field, unsigned char** vstr, unsigned int* vlen, long long* vll) {
    unsigned char* zl;
    unsigned char* fptr = NULL;
    unsigned char* vptr = NULL;
    int ret;

    assert(o->encoding == REDIS_ENCODING_ZIPLIST);

    field = getDecodedObject(field);

    zl = o->ptr;
    fptr = ziplistIndex(zl, ZIPLIST_HEAD);
    if (fptr != NULL) {
        // 查询键，skip = 1
        fptr = ziplistFind(fptr, field->ptr, sdslen(field->ptr), 1);
        if (fptr != NULL) {
            // 在EDIS_ENCODING_ZIPLIST编码方式中，键值由压缩列表中连续的两个节点表示
            vptr = ziplistNext(zl, fptr);
            assert(vptr != NULL);
        }
    }

    decrRefCount(field);

    if (vptr != NULL) {
        ret = ziplistGet(vptr, vstr, vlen, vll);
        assert(ret);
        return 0;
    }

    return -1;
}

/*
 * 从底层编码为REDIS_ENCODING_HT的哈希类型对象中取出field键对应的值
 */
int hashTypeGetFromHashTable(robj* o, robj* field, robj** value) {
    dictEntry* de;

    assert(o->encoding == REDIS_ENCODING_HT);

    de = dictFind(o->ptr, field);

    if (de == NULL) return -1;

    *value = dictGetVal(de);

    return 0;
}

/*
 * 从哈希类型对象中取出键为field的值，返回一个值对象
 */
robj* hashTypeGetObject(robj* o, robj* field) {
    robj* value = NULL;

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char* vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        if (hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll) == 0) {
            if (vstr) {
                value = createStringObject((char*)vstr, vlen);
            } else {
                value = createStringObjectFromLongLong(vll);
            }
        }
    } else if (o->encoding == REDIS_ENCODING_HT) {
        robj* aux;

        if (hashTypeGetFromHashTable(o, field, &aux) == 0) {
            incrRefCount(aux);
            value = aux;
        }
    } else {
        exit(1);
    }

    return value;
}

/*
 * 检查给定键field是否在哈希类型对象中
 */
int hashTypeExists(robj* o, robj* field) {

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char* vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        if (hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll) == 0) return 1;

    } else if (o->encoding == REDIS_ENCODING_HT) {
        robj* aux;

        if (hashTypeGetFromHashTable(o, field, &aux) == 0) return 1;
    } else {
        exit(1);
    }

    return 0;
}

/*
 * 将给定的键值对field-value添加到哈希类型对象中，
 * 如果field已经存在，则删除旧的值，替换为新的值
 */
int hashTypeSet(robj* o, robj* field, robj* value) {
    int update = 0;

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char* zl;
        unsigned char* fptr;
        unsigned char* vptr;

        field = getDecodedObject(field);
        value = getDecodedObject(value);

        zl = o->ptr;
        fptr = ziplistIndex(zl, ZIPLIST_HEAD);
        if (fptr != NULL) {
            fptr = ziplistFind(fptr, field->ptr, sdslen(field->ptr), 1);
            if (fptr != NULL) {
                vptr = ziplistNext(zl, fptr);
                assert(vptr != NULL);

                update = 1;

                zl = ziplistDelete(zl, &vptr);

                zl = ziplistInsert(zl, vptr, value->ptr, sdslen(value->ptr));
            }
        }

        if (!update) {
            zl = ziplistPush(zl, field->ptr, sdslen(field->ptr), ZIPLIST_TAIL);
            zl = ziplistPush(zl, value->ptr, sdslen(field->ptr), ZIPLIST_TAIL);
        }

        o->ptr = zl;

        decrRefCount(field);
        decrRefCount(value);

        if (hashTypeLength(o) > HASH_MAX_ZIPLIST_ENTRIES)
            hashTypeConvert(o, REDIS_ENCODING_HT);

    } else if (o->encoding == REDIS_ENCODING_HT) {

        /* Insert */
        if (dictReplace(o->ptr, field, value)) {
            incrRefCount(field);
        /* Update */
        } else {
            update = 1;
        }

        incrRefCount(value);
    } else {
        exit(1);
    }

    return update;
}

/*
 * 将给定键值对field-value从哈希类型对象中删除
 */
int hashTypeDelete(robj* o, robj* field) {
    int deleted = 0;

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char* zl;
        unsigned char* fptr;

        field = getDecodedObject(field);

        zl = o->ptr;
        fptr = ziplistIndex(zl, ZIPLIST_HEAD);
        if (fptr != NULL) {
            fptr = ziplistFind(fptr, field->ptr, sdslen(field->ptr), 1);
            if (fptr != NULL) {
                zl = ziplistDelete(zl, &fptr);
                zl = ziplistDelete(zl, &fptr);
                o->ptr = zl;
                deleted = 1;
            }
        }

        decrRefCount(field);
    } else if (o->encoding == REDIS_ENCODING_HT) {
        if (dictDelete((dict*)o->ptr, field) == REDIS_OK) {
            deleted = 1;

            // 删除成功时，判断字典是否需要收缩
            if (htNeedsResize(o->ptr)) dictResize(o->ptr);
        }
    } else {
        exit(1);
    }

    return deleted;
}

/*
 * 获取哈希类型对象包含键值对数目
 */
unsigned long hashTypeLength(robj* o) {
    unsigned long length = ULONG_MAX;

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        length = ziplistLen(o->ptr) / 2;
    } else if (o->encoding == REDIS_ENCODING_HT) {
        length = dictSize((dict*)o->ptr);
    } else {
        exit(1);
    }

    return length;
}

/*
 * 创建一个哈希类型对象的迭代器
 */
hashTypeIterator* hashTypeInitIterator(robj* subject) {

    hashTypeIterator* hi = zmalloc(sizeof(hashTypeIterator));

    hi->subject = subject;

    hi->encoding = subject->encoding;

    if (hi->encoding == REDIS_ENCODING_ZIPLIST) {
        hi->fptr = NULL;
        hi->vptr = NULL;
    } else if (hi->encoding == REDIS_ENCODING_HT) {
        hi->di = dictGetIterator(subject->ptr);
    } else {
        exit(1);
    }

    return hi;
}

/*
 * 释放一个哈希类型对象的迭代器
 */
void hashTypeReleaseIterator(hashTypeIterator* hi) {

    if (hi->encoding == REDIS_ENCODING_HT) {
        dictReleaseIterator(hi->di);
    }

    zfree(hi);
}

/*
 * 获取哈希类型对象的下一个节点，并将其保存到迭代器中
 */
int hashTypeNext(hashTypeIterator* hi) {

    if (hi->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char* zl;
        unsigned char* fptr;
        unsigned char* vptr;

        zl = hi->subject->ptr;
        fptr = hi->fptr;
        vptr = hi->vptr;

        if (fptr == NULL) {
            assert(vptr == NULL);
            fptr = ziplistIndex(zl, 0);
        } else {
            assert(vptr != NULL);
            fptr = ziplistNext(zl, vptr);
        }

        if (fptr == NULL) return REDIS_ERR;

        vptr = ziplistNext(zl, fptr);
        assert(vptr != NULL);

        hi->fptr = fptr;
        hi->vptr = vptr;

    } else if (hi->encoding == REDIS_ENCODING_HT) {
        if ((hi->de = dictNext(hi->di)) == NULL) return REDIS_ERR;

    } else {
        exit(1);
    }

    return REDIS_OK;
}

/*
 * 从底层编码为REDIS_ENCODING_ZIPLIST的哈希类型对象中，取出迭代器当前指向键值对的键或值
 */
void hashTypeCurrentFromZiplist(hashTypeIterator* hi, int what, unsigned char** vstr, unsigned int* vlen, long long* vll) {

    int ret;

    assert(hi->encoding == REDIS_ENCODING_ZIPLIST);

    if (what & REDIS_HASH_KEY) {
        ret = ziplistGet(hi->fptr, vstr, vlen, vll);
        assert(ret);
    } else {
        ret = ziplistGet(hi->vptr, vstr, vlen, vll);
        assert(ret);
    }
}

/*
 * 从底层编码为REDIS_ENCODING_HT的哈希类型对象中，取出迭代器当前指向键值对的键或值
 */
void hashTypeCurrentFromHashTable(hashTypeIterator* hi, int what, robj** dst) {
    assert(hi->encoding == REDIS_ENCODING_HT);

    if (what & REDIS_HASH_KEY) {
        *dst = dictGetKey(hi->de);
    } else {
        *dst = dictGetVal(hi->de);
    }
}

/*
 * 一个非copy-on-write友好，但层次更高的hashTypeCurrent函数，
 * 这个函数返回一个增加了引用计数的对象，或者一个新的对象，
 * 当使用完返回对象后，调用者需要对对象执行decrRefCount
 */
robj* hashTypeCurrentObject(hashTypeIterator* hi, int what) {
    robj* dst;

    if (hi->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char* vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);

        if (vstr) {
            dst = createStringObject((char*)vstr, vlen);
        } else {
            dst = createStringObjectFromLongLong(vll);
        }

    } else if (hi->encoding == REDIS_ENCODING_HT) {
        hashTypeCurrentFromHashTable(hi, what, &dst);
        incrRefCount(dst);
    } else {
        exit(1);
    }

    return dst;
}

/*
 * 将底层编码为REDIS_ENCODING_ZIPLIST的哈希类型对象转为enc编码
 */
void hashTypeConvertZiplist(robj* o, int enc) {
    assert(o->encoding == REDIS_ENCODING_ZIPLIST);

    if (enc == REDIS_ENCODING_ZIPLIST) {
        /* Nothing to do... */

    } else if (enc == REDIS_ENCODING_HT) {

        hashTypeIterator* hi;
        dict* dict;
        int ret;

        hi = hashTypeInitIterator(o);

        dict = dictCreate(&hashDictType, NULL);

        while (hashTypeNext(hi) != REDIS_ERR) {
            robj* field;
            robj* value;

            field = hashTypeCurrentObject(hi, REDIS_HASH_KEY);
            /* field = tryObjectEncoding(field); */

            value = hashTypeCurrentObject(hi, REDIS_HASH_VALUE);
            /* value = tryObjectEncoding(value); */

            ret = dictAdd(dict, field, value);
            assert(ret == DICT_OK);
        }

        hashTypeReleaseIterator(hi);

        zfree(o->ptr);

        o->encoding = REDIS_ENCODING_HT;
        o->ptr = dict;

    } else {
        exit(1);
    }
}

/*
 * 将底层编码为REDIS_ENCODING_ZIPLIST的哈希类型对象转为enc编码，只允许转为REDIS_ENCODING_HT编码
 */
void hashTypeConvert(robj* o, int enc) {

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        hashTypeConvertZiplist(o, enc);
    } else if (o->encoding == REDIS_ENCODING_HT) {
        exit(1);
    } else {
        exit(1);
    }
}
