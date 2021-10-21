CC = gcc
CCFLAGS = -Wall -std=c99

RM = rm
RMFLAGS = -rf

REDIS_SERVER = redis_server
REDIS_SERVER_OBJ = sds.o adlist.o intset.o dict.o zskiplist.o ziplist.o utils.o zmalloc.o object.o t_list.o t_set.o \
t_hash.o t_zset.o t_string.o db.o ae.o anet.o bio.o networking.o config.o aof.o redis.o

redis_server: $(REDIS_SERVER_OBJ)
	$(CC) -o $(REDIS_SERVER) $(REDIS_SERVER_OBJ) -lpthread

redis.o: redis.c redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h redis_obj.h
	$(CC) -c redis.c

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

t_string.o: t_string.c redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h
	$(CC) $(CCFLAGS) -c t_string.c

db.o: db.c redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h
	$(CC) $(CCFLAGS) -c db.c

ae.o: ae_epoll.c ae.c ae.h zmalloc.h config.h
	$(CC) $(CCFLAGS) -c ae_epoll.c ae.c

anet.o: anet.c anet.h
	$(CC) -Wall -c anet.c

bio.o: bio.c bio.h redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h
	$(CC) -Wall -c bio.c

networking.o: networking.c redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h
	$(CC) -Wall -c networking.c

config.o: config.c redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h
	$(CC) -Wall -c config.c

aof.o: aof.c redis.h zmalloc.h config.h utils.h sds.h dict.h adlist.h ziplist.h \
 intset.h zskiplist.h
	$(CC) -Wall -c aof.c

clean:
	$(RM) $(RMFLAGS) *.o *test
