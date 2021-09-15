CC = gcc
CCFLAGS = -Wall -std=c99

RM = rm
RMFLAGS = -rf

T_LIST_TEST = t_list_test
T_LIST_TEST_OBJ = sds.o adlist.o intset.o dict.o zskiplist.o ziplist.o utils.o zmalloc.o object.o redis.o t_list.o t_list_test.o

T_SET_TEST = t_set_test
T_SET_TEST_OBJ = sds.o adlist.o intset.o dict.o zskiplist.o ziplist.o utils.o zmalloc.o object.o redis.o t_set.o t_set_test.o

T_HASH_TEST = t_hash_test
T_HASH_TEST_OBJ = sds.o adlist.o intset.o dict.o zskiplist.o ziplist.o utils.o zmalloc.o object.o redis.o t_hash.o t_hash_test.o

T_ZSET_TEST = t_zset_test
T_ZSET_TEST_OBJ = sds.o adlist.o intset.o dict.o zskiplist.o ziplist.o utils.o zmalloc.o object.o redis.o t_zset.o t_zset_test.o

t_list_test: $(T_LIST_TEST_OBJ)
	$(CC) $(CCFLAGS) -o $(T_LIST_TEST) $(T_LIST_TEST_OBJ)

t_set_test: $(T_SET_TEST_OBJ)
	$(CC) $(CCFLAGS) -o $(T_SET_TEST) $(T_SET_TEST_OBJ)

t_hash_test: $(T_HASH_TEST_OBJ)
	$(CC) $(CCFLAGS) -o $(T_HASH_TEST) $(T_HASH_TEST_OBJ)

t_zset_test: $(T_ZSET_TEST_OBJ)
	$(CC) $(CCFLAGS) -o $(T_ZSET_TEST) $(T_ZSET_TEST_OBJ)

t_list_test.o: t_list_test.c redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h
	$(CC) $(CCFLAGS) -c t_list_test.c

t_set_test.o: t_set_test.c redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h
	$(CC) $(CCFLAGS) -c t_set_test.c

t_hash_test.o: t_hash_test.c redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h
	$(CC) $(CCFLAGS) -c t_hash_test.c

t_zset_test.o: t_zset_test.c redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h
	$(CC) $(CCFLAGS) -c t_zset_test.c

redis.o: redis.c redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h redis_obj.h
	$(CC) $(CCFLAGS) -c redis.c

sds.o: sds.c sds.h zmalloc.h
	$(CC) $(CCFLAGS) -c sds.c

adlist.o: adlist.c adlist.h zmalloc.h
	$(CC) $(CCFLAGS) -c adlist.c

intset.o: intset.c intset.h zmalloc.h config.h
	$(CC) $(CCFLAGS) -c intset.c

dict.o: dict.c dict.h zmalloc.h
	$(CC) $(CCFLAGS) -c dict.c

zskiplist.o: zskiplist.c zskiplist.h zmalloc.h redis_obj.h
	$(CC) $(CCFLAGS) -c zskiplist.c

ziplist.o: ziplist.c ziplist.h zmalloc.h utils.h
	$(CC) $(CCFLAGS) -c ziplist.c

utils.o: utils.c utils.h sds.h
	$(CC) $(CCFLAGS) -c utils.c

zmalloc.o: zmalloc.c config.h zmalloc.h
	$(CC) $(CCFLAGS) -c zmalloc.c

object.o: object.c redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h
	$(CC) $(CCFLAGS) -c object.c

t_list.o: t_list.c redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h
	$(CC) $(CCFLAGS) -c t_list.c

t_set.o: t_set.c redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h
	$(CC) $(CCFLAGS) -c t_set.c

t_hash.o: t_hash.c redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h
	$(CC) $(CCFLAGS) -c t_hash.c

t_zset.o: t_zset.c redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h
	$(CC) $(CCFLAGS) -c t_zset.c

clean:
	$(RM) $(RMFLAGS) *.o *test
