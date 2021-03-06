#ifndef __CONFIG_H__
#define __CONFIG_H__

#include "jsons.h"


struct monitorPathInfo {
  char path[PATH_MAX];
  int fmt_id ;
  struct list_head upper;
} ;
typedef struct monitorPathInfo* monitorPathInfo_t ;

struct global_setting_s {
  tree_map_t monitor_paths ;

  char hbaseAddr[32];
  int hbasePort ;

  int workerCount;

  bool preload ;
} ;
typedef struct global_setting_s* global_setting_t ;

struct hsyncd_config_s {

  jsonKV_t *m_root ;

  struct global_setting_s m_globSettings;

} __attribute__((__aligned__(64))) ;

typedef struct hsyncd_config_s* hsyncd_config_t ;


extern const char* get_monitor_path(hsyncd_config_t conf);

extern int init_config(hsyncd_config_t conf, const char *infile);

extern int free_config(hsyncd_config_t conf);

extern void get_hbase_client_settings(hsyncd_config_t conf, char *a, 
                                      size_t la, int *p);

extern int get_worker_count(hsyncd_config_t conf);

extern bool need_preload(hsyncd_config_t conf);

#endif /* __CONFIG_H__*/

