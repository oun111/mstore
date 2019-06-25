#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "pay_data.h"
#include "log.h"
#include "kernel.h"
#include "mm_porting.h"
#include "myrbtree.h"
#include "config.h"
#include "bitmap.h"


static int compare(const char *s0, const char *s1)
{
  return strcmp(s0,s1);
}

pay_channel_t get_pay_channel(pay_channels_entry_t entry, const char *chan)
{
  pay_channel_t p = 0;


  if (MY_RB_TREE_FIND(&entry->u.root,chan,p,channel,node,compare)) 
    return NULL ;

  return p;
}

pay_data_t get_pay_data(pay_channel_t pc, const char *subname)
{
  pay_data_t pd = 0;


  list_for_each_entry(pd,&pc->pay_data_list,upper) {
    if (!strcmp(pd->subname,subname))
      return pd;
  }

  return NULL ;
}

pay_data_t 
get_paydata_by_ali_appid(pay_channels_entry_t entry, const char *chan, 
                         const char *appid)
{
  pay_data_t pd = 0;
  pay_channel_t pc = get_pay_channel(entry,chan);


  if (!pc) {
    log_error("found no pay channel '%s'\n",chan);
    return NULL;
  }

  list_for_each_entry(pd,&pc->pay_data_list,upper) {
    const char *ch_appid = get_tree_map_value(pd->pay_params,"app_id");

    if (appid && !strcmp(ch_appid,appid))
      return pd;
  }

  return NULL ;
}

pay_data_t 
get_paydata_by_id(pay_channels_entry_t entry, const char *chan, int id)
{
  pay_data_t pd = 0;
  pay_channel_t pc = get_pay_channel(entry,chan);


  if (!pc) {
    log_error("found no pay channel '%s'\n",chan);
    return NULL;
  }

  list_for_each_entry(pd,&pc->pay_data_list,upper) {
    const char *p_id = get_tree_map_value(pd->pay_params,"id");

    if (p_id && atoi(p_id)==id)
      return pd;
  }

  return NULL ;
}


pay_channels_entry_t new_pay_channels_entry()
{
  pay_channels_entry_t entry = kmalloc(sizeof(struct pay_channels_entry_s),0L);


  entry->u.root = RB_ROOT ;

  return entry;
}

pay_channel_t new_pay_channel(const char *chan)
{
  char *pv  = NULL;
  pay_channel_t pc = kmalloc(sizeof(struct pay_channel_item_s),0L);


  pv = (char*)chan ;
  pc->channel = alloc_default_dbuffer();
  write_dbuf_str(pc->channel,pv);
  INIT_LIST_HEAD(&pc->pay_data_list);

  return pc ;
}

static int get_rc_paras(tree_map_t rc_cfg, struct risk_control_s *rcp, dbuffer_t *reason)
{
  char *tmp = 0, *rcid = 0;
#define FETCH_RC_KEY(rck,__rc_cfg,tmp,r) do{ \
  char msg[256] = ""; \
  tmp = get_tree_map_value(__rc_cfg,rck) ;\
  if (!tmp) { \
    snprintf(msg,sizeof(msg),"risk control keyword '%s' not found",rck); \
    log_error("%s\n",msg);  \
    if (r) write_dbuf_str(r,msg); \
    return -1 ; \
  } \
}while(0)

  FETCH_RC_KEY("rcid",rc_cfg,rcid,*reason);

  FETCH_RC_KEY("max_orders",rc_cfg,tmp,*reason);
  rcp->max_orders = atoi(tmp);

  // max amount 
  FETCH_RC_KEY("max_amount",rc_cfg,tmp,*reason) ;
  rcp->max_amount = atof(tmp);

  // period
  FETCH_RC_KEY("period",rc_cfg,tmp,*reason) ;
  rcp->period = atol(tmp);

  log_info("rcid: %s, max_orders: %d, max_amount: %f, period: %lld\n",
      rcid, rcp->max_orders, rcp->max_amount, rcp->period);

  return 0;
}

pay_data_t 
add_pay_data(pay_channels_entry_t entry, const char *chan, 
             const char *subname, tree_map_t params)
{
  char *pv = 0;
  pay_channel_t pc = get_pay_channel(entry,chan);
  pay_data_t p = NULL;
  char *rcid = 0;
  char *ppriv = 0, *ppub = 0;


  if (!pc) {
    pc = new_pay_channel(chan);
    if (MY_RB_TREE_INSERT(&entry->u.root,pc,channel,node,compare)) {
      log_error("insert pay channel item by name '%s' fail\n",chan);
      kfree(pc);
      return NULL;
    }
  }

  p = get_pay_data(pc,subname);

  if (!p) {
    p = kmalloc(sizeof(struct pay_data_item_s),0L);
    if (!p) {
      log_error("allocate new tree map item fail\n");
      return NULL ;
    }

    p->subname = alloc_default_dbuffer(chan);
    p->pay_params = params;
    p->rc.max_amount = 0.0;
    p->rc.max_orders = 0;
    p->rc.time = 0L;
    p->cfg_rc.max_amount = 0.0;
    p->cfg_rc.max_orders = 0;
    p->cfg_rc.time = 0L;

    INIT_LIST_HEAD(&p->upper); 
    list_add(&p->upper,&pc->pay_data_list);
  }

  // update risk control configs
  rcid = get_tree_map_value(p->pay_params,"rcid");
  if (rcid) {
    extern paySvr_config_t get_running_configs();
    paySvr_config_t conf = get_running_configs();
    tree_map_t rc_cfg = get_rc_conf_by_rcid(conf,rcid);

    if (rc_cfg) {
      get_rc_paras(rc_cfg,&p->cfg_rc,NULL);
    }
    else {
      log_error("found no risk control configs for rcid: %s\n", rcid);
    }
  }

  // pre init the rsa entry
  ppub  = get_tree_map_value(p->pay_params,"public_key_path");
  ppriv = get_tree_map_value(p->pay_params,"private_key_path");
  if (ppub && ppriv) {
    init_rsa_entry(&p->rsa_cache,ppub,ppriv);
  }

  pv = (char*)subname ;
  write_dbuf_str(p->subname,pv);

  return p;
}

static
int drop_pay_data_internal(pay_channel_t pc)
{
  pay_data_t pos,n ;


  list_for_each_entry_safe(pos,n,&pc->pay_data_list,upper) {
    drop_dbuffer(pos->subname);
    release_rsa_entry(&pos->rsa_cache);
    kfree(pos);
  }

  return 0;
}

int drop_pay_channel(pay_channels_entry_t entry, const char *chan)
{
  pay_channel_t pc = get_pay_channel(entry,chan);


  if (!pc) {
    return -1;
  }

  drop_pay_data_internal(pc);

  rb_erase(&pc->node,&entry->u.root);

  drop_dbuffer(pc->channel);
  kfree(pc);

  return 0;
}

static
int release_all_pay_datas(pay_channels_entry_t entry)
{
  pay_channel_t pos,n;


  rbtree_postorder_for_each_entry_safe(pos,n,&entry->u.root,node) {
    drop_pay_channel(entry,pos->channel);
  }

  return 0;
}

void delete_pay_channels_entry(pay_channels_entry_t entry)
{
  release_all_pay_datas(entry);

  kfree(entry);
}

void update_paydata_rc_arguments(pay_data_t pd, double amount)
{
  pd->rc.max_amount += amount ;
  pd->rc.max_orders ++ ;
}

pay_data_t get_pay_route2(struct list_head *pr_list, dbuffer_t *reason) 
{
  pay_route_item_t pr ;
  char msg[256] = "";


  // risk control timestamp
  struct timespec ts ;
  clock_gettime(CLOCK_REALTIME,&ts);

  // get best pay route
  list_for_each_entry(pr,pr_list,upper) {

    pay_data_t pos = pr->pdr ;

    if (pos->rc.time==0L || (ts.tv_sec-pos->rc.time)>pos->cfg_rc.period) {
      pos->rc.time = ts.tv_sec;
      pos->rc.max_orders = 0;
      pos->rc.max_amount = 0.0;
    }
    log_debug("config rc: %lld, %d, %f\n",pos->cfg_rc.period,pos->cfg_rc.max_orders,pos->cfg_rc.max_amount);
    log_debug("current rc: %ld, %d, %f\n",pos->rc.time,pos->rc.max_orders,pos->rc.max_amount);

    if ((ts.tv_sec-pos->rc.time)<pos->cfg_rc.period) {
      // 
      if (pos->rc.max_orders>=pos->cfg_rc.max_orders)
        continue ;

      if (pos->rc.max_amount>=pos->cfg_rc.max_amount)
        continue ;
    }

    // move pay data item to list tail
    {
      list_del(&pr->upper);
      list_add_tail(&pr->upper,pr_list);
    }

    return pos ;
  }

  snprintf(msg,sizeof(msg),"no suitable pay route");
  log_error("%s\n",msg);
  write_dbuf_str(*reason,msg);

  return NULL ;
}

#if 0
pay_data_t get_pay_route(pay_channels_entry_t entry, const char *chan, dbuffer_t *reason)
{
  pay_channel_t pc  = get_pay_channel(entry,chan);
  pay_data_t pos ;
  char msg[256] = "";


  if (!pc) {
    snprintf(msg,sizeof(msg),"no pay channel '%s'",chan);
    log_error("%s\n",msg);
    write_dbuf_str(*reason,msg);
    return NULL ;
  }

  // risk control timestamp
  struct timespec ts ;
  clock_gettime(CLOCK_REALTIME,&ts);

  // get best pay route
  list_for_each_entry(pos,&pc->pay_data_list,upper) {
#if 0
    char *ol = get_tree_map_value(pos->pay_params,"online");

    //dump_tree_map(pos->pay_params);
    if (!ol || *ol=='0')
      continue ;
#endif

#if 0
    // don't use trans fund channel
    if (pos == pc->pd_transFund)
      continue ;
#endif

    if (pos->rc.time==0L || (ts.tv_sec-pos->rc.time)>pos->cfg_rc.period) {
      pos->rc.time = ts.tv_sec;
      pos->rc.max_orders = 0;
      pos->rc.max_amount = 0.0;
    }
    log_debug("config rc: %lld, %d, %f\n",pos->cfg_rc.period,pos->cfg_rc.max_orders,pos->cfg_rc.max_amount);
    log_debug("current rc: %ld, %d, %f\n",pos->rc.time,pos->rc.max_orders,pos->rc.max_amount);

    if ((ts.tv_sec-pos->rc.time)<pos->cfg_rc.period) {
      // 
      if (pos->rc.max_orders>=pos->cfg_rc.max_orders)
        continue ;

      if (pos->rc.max_amount>=pos->cfg_rc.max_amount)
        continue ;
    }

    // move pay data item to list tail
    {
      list_del(&pos->upper);
      list_add_tail(&pos->upper,&pc->pay_data_list);
    }

    return pos ;
  }

  snprintf(msg,sizeof(msg),"no suitable pay route for channel '%s'",chan);
  log_error("%s\n",msg);
  write_dbuf_str(*reason,msg);

  return NULL ;
}
#endif

int init_pay_data(pay_channels_entry_t *paych)
{
  tm_item_t pos,n;
  tm_item_t pos1,n1;
  extern paySvr_config_t get_running_configs();
  tree_map_t pr = get_running_configs()->chan_conf;
  tree_map_t entry = get_tree_map_nest(pr,"channels");
  
  
  *paych = new_pay_channels_entry();

  rbtree_postorder_for_each_entry_safe(pos,n,&entry->u.root,node) {
    tree_map_t chansub = pos->nest_map ;

    if (!chansub)
      continue ;

    rbtree_postorder_for_each_entry_safe(pos1,n1,&chansub->u.root,node) {
      char *ol = get_tree_map_value(pos1->nest_map,"online");

      if (!ol || *ol!='1') 
        continue ;

      add_pay_data(*paych,pos->key,pos1->key,pos1->nest_map);
      log_info("adding channel '%s - %s'\n",pos->key,pos1->key);
    }
  }

  return 0;
}

static
int idStr_to_bits(const char *s, unsigned long idbits[], size_t szb)
{
  for (char *p=(char*)s,*pch = NULL;p && *p!='\0';) {
    int v0 = 0;

    pch = strchr(p,',');
    if (pch) {
        *pch= '\0';
        v0  = atoi(p);
        *pch= ',';
        p   = pch + 1;
    }
    else {
        v0 = atoi(p);
        p++;
    }

    if (v0>=0 && v0<szb*sizeof(unsigned long)*8)
        __set_bit(v0,idbits);

    if (!pch) break ;
  }

  return 0;
}

int init_pay_route_references(pay_channels_entry_t pe, struct list_head *pr_list,
                              const char *ch_ids, bool istransfund)
{
  unsigned long idbits[128];
  const size_t szb = sizeof(idbits);
  pay_route_item_t pr = NULL;
  pay_data_t pd = NULL;
  const char *chan = "alipay";


  bzero(idbits,szb);
  idStr_to_bits(ch_ids,idbits,szb);

  for (int id=0;id<szb;id++) {
    if (test_bit(id,idbits)==1 && (pd=get_paydata_by_id(pe,chan,id))) {

      char *tf = get_tree_map_value(pd->pay_params,"istransfund");

      // test for alipay transfund channel
      if ((istransfund && (!tf || *tf!='1')) || (!istransfund && (tf && *tf=='1')))
        continue;

      pr = kmalloc(sizeof(struct pay_route_item_s),0L);
      pr->pdr = pd ;
      list_add(&pr->upper,pr_list);
    }
  }

  return 0;
}

int release_all_pay_route_references(struct list_head *pr_list)
{
  pay_route_item_t pos, n;


  list_for_each_entry_safe(pos,n,pr_list,upper) {
    kfree(pos);
  }

  return 0;
}

