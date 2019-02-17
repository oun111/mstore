#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "http_utils.h"
#include "connection.h"
#include "kernel.h"
#include "tree_map.h"
#include "myrbtree.h"
#include "log.h"
#include "jsons.h"


size_t get_http_hdr_size(char *inb, size_t sz_in)
{
  char *p = strstr(inb,"\r\n\r\n");

  return p?(p-inb+4):0;
}

int get_http_hdr_field_str(char *inb, size_t sz_in, const char *field, 
                           const char *endstr, char *outb, size_t *sz_out)
{
  const size_t sz_hdr = get_http_hdr_size(inb,sz_in);
  char *pf = 0, *fend = 0;
  size_t vlen = 0L;


  if (!sz_hdr) {
    return -1;
  }

  pf = strstr(inb,field);
  if (!pf) {
    log_error("cant find field '%s'\n",field);
    return -1;
  }

  fend = strstr(pf,endstr);
  if (!fend) {
    log_error("cant find field ending '%s'\n",endstr);
    return -1;
  }

  vlen = fend-pf-strlen(field) ;
  if (vlen>*sz_out) {
    log_error("res size %zu > %zu\n",vlen,*sz_out);
    return -1;
  }

  if (vlen==0L) {
    outb[0] = '\0';
    *sz_out = 0L;
    return 0;
  }

  strncpy(outb,pf+strlen(field),vlen);
  outb[vlen] = '\0';
  *sz_out = vlen ;

  return 0;
}

int get_http_hdr_field_int(char *inb, size_t sz_in, const char *field, const char *endstr)
{
  const size_t sz_hdr = get_http_hdr_size(inb,sz_in);
  char *pf = 0, *fend = 0;
  char buf[32] = "";
  size_t vlen = 0L;


  if (!sz_hdr) {
    return 0;
  }

  pf = strstr(inb,field);
  if (!pf) {
    return 0;
  }

  fend = strstr(pf,endstr);
  vlen = fend-pf-strlen(field) ;
  strncpy(buf,pf+strlen(field),vlen);
  buf[vlen] = '\0';

  return atoi(buf);
}

ssize_t get_http_body_size(char *inb, size_t sz_in)
{
  return get_http_hdr_field_int(inb,sz_in,"Content-Length:","\r\n");
}

char* get_http_body_ptr(char *inb, size_t sz_in)
{
  size_t szhdr = get_http_hdr_size(inb,sz_in);

  if (szhdr==0L) {
    return NULL;
  }

  return inb+szhdr ;
}

dbuffer_t create_json_params(tree_map_t map)
{
  jsonKV_t *pr = 0;
  dbuffer_t strParams = 0;


  pr = jsons_parse_tree_map(map);
  strParams = alloc_default_dbuffer();

  jsons_toString(pr,&strParams);
  jsons_release(pr);

  return strParams;
}

dbuffer_t create_html_params(tree_map_t map)
{
  dbuffer_t strParams = alloc_default_dbuffer();
  tm_item_t pos, n ;
  //size_t ln = 0L;


  MY_RBTREE_SORTORDER_FOR_EACH_ENTRY_SAFE(pos,n,&map->u.root,node) {

    if (dbuffer_data_size(pos->val)==0L)
      continue ;

    // construct 'key=value'
    append_dbuf_str(strParams,pos->key);
    append_dbuf_str(strParams,"=");
    append_dbuf_str(strParams,pos->val);
    append_dbuf_str(strParams,"&");
  }

  return strParams ;
}

int create_http_normal_res(dbuffer_t *inb, int type, const char *strParams)
{
  char *hdr = 0;
  size_t sz_hdr = 0L;
  const char hdrFmt[] = "HTTP/1.1 %d\r\n"
                         "Content-Type: %s\r\n"
                         "Content-Length:%zu\r\n\r\n";


  sz_hdr = sizeof(hdrFmt)+strlen(strParams)+10;
  hdr = alloca(sz_hdr);

  snprintf(hdr,sz_hdr,hdrFmt,200,type==pt_html?"text/html":"text/json",
           strlen(strParams));

  // attach whole body
  write_dbuf_str(*inb,hdr);
  append_dbuf_str(*inb,strParams);

  log_debug("normal res: %s\n",*inb);

  return 0;
}

int create_browser_redirect_req(dbuffer_t *inb, const char *url, 
                             int param_type, tree_map_t map)
{
  char *hdr = 0;
  size_t sz_hdr = 0L;
  dbuffer_t strParams = 0;
  const char *body = "success";
  const char hdrFmt[] = "HTTP/1.1 302 \r\n"
                        "Content-Length: %zu\r\n"
                        "Location: %s?%s \r\n\r\n";


  if (param_type==pt_html) {
    strParams = create_html_params(map);
  }
  else {
    strParams = create_json_params(map);
  }


  sz_hdr = sizeof(hdrFmt)+strlen(strParams)+strlen(url);
  hdr = alloca(sz_hdr);

  snprintf(hdr,sz_hdr,hdrFmt,strlen(body),url,strParams);

  // attach whole body
  write_dbuf_str(*inb,hdr);
  append_dbuf_str(*inb,body);

  log_debug("REDIRECT req: %s\n",*inb);

  drop_dbuffer(strParams);

  return 0;
}

int create_http_get_req(dbuffer_t *inb, const char *url, 
                        int param_type, tree_map_t map)
{
  char *hdr = 0;
  size_t sz_hdr = 0L;
  dbuffer_t strParams = 0;
  char host[128] = "", uri[256]="";
  const char hdrFmt[] = "GET %s?%s HTTP/1.1\r\n"
                        "Host: %s\r\n"
                        "User-Agent: normal-svr/0.1\r\n"
                        "Content-Type: %s\r\n\r\n";


  if (param_type==pt_html) {
    strParams = create_html_params(map);
  }
  else {
    strParams = create_json_params(map);
  }

  // http header
  parse_http_url(url,host,128,NULL,uri,256,NULL);

  sz_hdr = sizeof(hdrFmt)+strlen(strParams)+strlen(uri)+strlen(host);
  hdr = alloca(sz_hdr);

  snprintf(hdr,sz_hdr,hdrFmt,uri,strParams,host,
           param_type==pt_html?"text/html":"text/json");

  // attach whole body
  write_dbuf_str(*inb,hdr);

  log_debug("GET req: %s\n",*inb);

  drop_dbuffer(strParams);

  return 0;
}

int create_http_post_req(dbuffer_t *inb, const char *url, 
                         int param_type, tree_map_t map)
{
  char hdr[1024] = "";
  dbuffer_t strParams = 0;
  char host[128] = "", uri[256]="";
  const char hdrFmt[] = "GET %s HTTP/1.1\r\n"
                        "User-Agent: normal-svr/0.1\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Host: %s\r\n\r\n";


  if (param_type==pt_html) {
    strParams = create_html_params(map);
  }
  else {
    strParams = create_json_params(map);
  }

  // http header
  parse_http_url(url,host,128,NULL,uri,256,NULL);
  snprintf(hdr,sizeof(hdr),hdrFmt,uri,param_type==pt_html?"text/html":"text/json",
           dbuffer_data_size(strParams),host);

  // attach whole body
  write_dbuf_str(*inb,hdr);
  append_dbuf_str(*inb,strParams);

  log_debug("POST req: %s\n",*inb);

  drop_dbuffer(strParams);

  return 0;
}

int parse_http_url(const char *url, char *host, size_t szhost,
                   int *port, char *uri, size_t szuri, bool *is_ssl)
{
  char *phost = strstr(url,"://"), *hend = 0, *pe = 0;
  char *urlend = (char*)url + strlen(url);
  size_t ln = 0L, lp=0L;
  char chport[10] = "";


  if (is_ssl && phost) {
    *is_ssl = !strncmp(url,"https",5)?true:false ;
  }

  phost = phost?(phost+3):(char*)url ;
  pe = strchr(phost,':');
  hend = pe?pe:strchr(phost,'/');

  // host
  ln = hend?(hend-phost):(urlend-phost);
  if (host) {
    strncpy(host,phost,ln<szhost?ln:(szhost-1));
    host[ln] = '\0';
  }

  if (pe) {
    char *s = pe ;
    for (++pe;isspace(*pe);pe++);
    for (int i=0;pe&&*pe!='\0'&&isdigit(*pe);i++,pe++)
      chport[i] = *pe ;
    if (port) 
      *port = atoi(chport);
    lp = pe-s ;
  }

  // uri
  phost += ln + lp ;
  ln = strlen(url)-ln;
  if (uri) {
    if (ln>0) {
      strncpy(uri,phost,ln<szuri?ln:(szuri-1));
      uri[ln] = '\0';
    }
    else {
      uri[0] = '/' ;
      uri[1] = '\0';
    }
  }

  return 0;
}

#if TEST_CASES==1
void test_http_utils()
{
  char url[]=  "www.163.com///";
  char host[32], uri[64];

  parse_http_url(url,host,32,NULL,NULL,0L,NULL);

  printf("host111: %s, uri: %s\n",host,uri);
}
#endif
