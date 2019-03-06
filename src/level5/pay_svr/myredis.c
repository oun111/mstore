#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <hiredis/hiredis.h>
#include "dbuffer.h"
#include "jsons.h"
#include "tree_map.h"
#include "myredis.h"
#include "log.h"

/**
 * Redis - Mysql 数据同步策略：
 *
 * 写入：
 *
 *  1. 应用写数据到redis，置status=need-sync（如有记录则覆盖），同时push消息
 *
 *  2. 同步程序接收push消息，拆分该记录写入Mysql，置status=ok
 *
 * 读取：
 *
 *  1. 有记录且status=ok/need-sync，直接返回记录
 *
 *  2. 无记录，加一条Redis空记录并置status=need-sync-back，同时upsh消息
 *     同步程序接收push消息并尝试从Mysql读取：
 *
 *      若有，把Mysql记录合并Redis格式覆盖Redis空记录并置status=ok;
 *      若无，更新Redis空记录置status=n-a
 *
 *  3. 应用根据status=need-sync-back/n-a 返回相应内容
 *
 */

#define REDIS_CTX  ((redisContext*)(mr->ctx))


bool is_myredis_ok(myredis_t mr)
{
  return REDIS_CTX && REDIS_CTX->err==REDIS_OK ;
}

void myredis_release(myredis_t mr)
{
  if (REDIS_CTX) {
    redisFree(REDIS_CTX);
    mr->ctx = NULL;
  }
}

int myredis_init(myredis_t mr, const char *host, int port, char *name)
{
  myredis_release(mr);

  mr->ctx = redisConnect(host,port);

  if (!REDIS_CTX || REDIS_CTX->err!=REDIS_OK) {
    log_error("redis connect fail: %s\n",REDIS_CTX->errstr);
    myredis_release(mr);
    return -1;
  }

  snprintf(mr->cache_name,sizeof(mr->cache_name),"%s_cache",name);
  snprintf(mr->mq_name,sizeof(mr->mq_name),"%s_mq",name);

  return 0;
}

static
int myredis_write_cache(myredis_t mr, char *k, char *v)
{
  int ret = 0;
  redisReply *rc = (redisReply*)redisCommand(REDIS_CTX,"hset %s %s %s",
                   mr->cache_name,k,v);


  if (rc->type==REDIS_REPLY_ERROR) {
    log_error("write to cache '%s' fail: %s\n",mr->cache_name,rc->str) ;
    ret = -1;
  }

  freeReplyObject(rc);

  return ret;
}

static
int myredis_read_cache(myredis_t mr, char *k, redisReply **rc)
{
  *rc = (redisReply*)redisCommand(REDIS_CTX,"hget %s %s",mr->cache_name,k);

  if ((*rc)->type==REDIS_REPLY_ERROR) {
    log_error("read from redis fail: %s\n",(*rc)->str) ;
    return -1 ;
  }

  return 0;
}

static
int myredis_read_cache_all(myredis_t mr, redisReply **rc)
{
  *rc = (redisReply*)redisCommand(REDIS_CTX,"hgetall %s",mr->cache_name);

  if ((*rc)->type==REDIS_REPLY_ERROR) {
    log_error("read from redis fail: %s\n",(*rc)->str) ;
    return -1 ;
  }

  return 0;
}

static
int myredis_mq_tx(myredis_t mr, char *m)
{
  int ret = 0;
  redisReply *rc = (redisReply*)redisCommand(REDIS_CTX,"rpush %s %s ",
                   mr->mq_name,m);


  if (rc->type==REDIS_REPLY_ERROR) {
    log_error("write to mq '%s' fail: %s\n", mr->mq_name,rc->str) ;
    ret = -1;
  }

  freeReplyObject(rc);

  return ret;
}

static
int myredis_mq_rx(myredis_t mr, redisReply **rc)
{
  *rc = (redisReply*)redisCommand(REDIS_CTX,"lpop %s ",mr->mq_name);

  if ((*rc)->type==REDIS_REPLY_ERROR) {
    log_error("read from redis fail: %s\n",(*rc)->str) ;
    return -1 ;
  }

  return 0;
}

int 
myredis_write(myredis_t mr, const char *table, char *key, char *value, int st)
{
  size_t kl = strlen(table)+strlen(key)+6;
  size_t vl = kl+strlen(value)+96;
  dbuffer_t k = alloc_dbuffer(kl);
  dbuffer_t v = alloc_dbuffer(vl);
  int status = st>=mr__na&&st<=mr__ok?st:mr__need_sync;
  int ret = 0;


  snprintf(k,kl,"%s_%s",table,key);
  snprintf(v,vl,"{\"status\" : %d, \"table\" : \"%s\", \"value\" : \"%s\" }",
           status, table, value);


  if (myredis_write_cache(mr,k,v)==-1) {
    ret = -1;
  }

  // push message
  if (status==mr__need_sync || status==mr__need_sync_back) {
    if (myredis_mq_tx(mr,k)==-1) {
      ret = -1;
    }
  }

  drop_dbuffer(k);
  drop_dbuffer(v);

  return ret;
}

int myredis_read(myredis_t mr, const char *table, const char *key, 
                 dbuffer_t *value)
{
  size_t kl = strlen(table)+strlen(key)+10;
  dbuffer_t k = alloc_dbuffer(kl);
  redisReply *rc = 0;
  int ret = 0, status = 0;
  jsonKV_t *pr = 0;
  tree_map_t map = 0, submap = 0;
  dbuffer_t pd = 0;


  // key in redis table
  snprintf(k,kl,"%s_%s",table,key);

  ret = myredis_read_cache(mr,k,&rc);
  if (unlikely(ret==-1)) {
    freeReplyObject(rc);
    drop_dbuffer(k);
    return -1;
  }

  if (rc->type!=REDIS_REPLY_STRING) {
    log_debug("not redis string, type=%d\n",rc->type);
    ret = -1;

    if (rc->type==REDIS_REPLY_NIL) {
      log_debug("begin to sync, try read later\n");
      // write dummy record
      myredis_write(mr,table,(char*)key,"",mr__need_sync_back);
      ret = 1;
    }

    drop_dbuffer(k);
    freeReplyObject(rc);
    return ret ;
  }


  dbuffer_t v = alloc_default_dbuffer();
  v = write_dbuffer_string(v,rc->str,rc->len);
  //printf("ret string: %s\n",v);

  // parse the result
  pr  = jsons_parse(v);
  map = jsons_to_treemap(pr);
  submap = get_tree_map_nest(map,"root");
  pd  = get_tree_map_value(submap,"status");

  if (unlikely(!pd)) {
    log_error("fatal: no status found!\n");
    ret = -1;
    goto __end ;
  }

  status = atoi(pd);
  // get value ok!
  if (status==mr__ok || status==mr__need_sync) {
    pd = get_tree_map_value(submap,"value");
    *value = write_dbuffer_string(*value,pd,strlen(pd));
    ret = 0;
    goto __end ;
  }

  // the record is NOT found
  if (status==mr__na) {
    log_error("record not found!\n");
    ret = -1;
    goto __end ;
  }

  if (status==mr__need_sync_back) {
    log_debug("try read later!\n");
    ret = 1;
    goto __end ;
  }

__end:
  drop_dbuffer(k);
  drop_dbuffer(v);
  jsons_release(pr);
  delete_tree_map(map);
  freeReplyObject(rc);

  return ret;
}

#if 0
int myredis_read_all(myredis_t mr, char ***rec, int *cnt)
{
  redisReply *rc = 0;
  int ret = myredis_read_cache_all(mr,&rc);


  if (ret)
    return -1;

  if (rc->type!=REDIS_REPLY_ARRAY) {
    log_error("not redis array type!\n");
    freeReplyObject(rc);
    return -1;
  }

  *cnt = rc->elements>>1 ;
  *rec = kmalloc(sizeof(char*)*(*cnt),0L);

  //printf("type: %d, cnt: %d\n",rc->type,*cnt);
  for (int i=0,n=0;i<rc->elements;i++) {
    // dont read keys
    if (rc->element[i]->type==REDIS_REPLY_STRING && (i&0x1)) {
      (*rec)[n] = kmalloc(rc->element[i]->len+1,0L);
      strcpy((*rec)[n],rc->element[i]->str);

      //printf("%s\n",(*rec)[n]);
      n++ ;
    }
  }

  freeReplyObject(rc);

  return 0;
}

void myredis_read_all_done(char ***rec, int cnt)
{
  if (!rec || !*rec)
    return ;

  for (int i=0;i<cnt;i++)
    kfree((*rec)[i]);

  kfree(*rec);
}
#endif

#if TEST_CASES==1
void test_myredis()
{
  struct myredis_s mr ;
  dbuffer_t res = alloc_default_dbuffer(); 
  //char **ary = 0;
  //char *k = 0;
  //int cnt = 0;

  if (myredis_init(&mr,"127.0.0.1",6379,"rds_example")) {
    return ;
  }

  myredis_write(&mr,"merchant_info","100","asdfjkasdlfj",-1);

#if 0
  k = "101";
  printf("trying key %s\n",k);
  if (!myredis_read(&mr,"merchant_info",k,&res)) {
    printf("ret string: %s\n",res);
  }

  k = "100";
  printf("trying key %s\n",k);
  if (!myredis_read(&mr,"merchant_info",k,&res)) {
    printf("ret string: %s\n",res);
  }
#endif

  drop_dbuffer(res);

#if 0
  if (!myredis_read_all(&mr,&ary,&cnt)) {
  }

  myredis_read_all_done(&ary,cnt);
#endif
}
#endif