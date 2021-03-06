#ifndef __SSL_H__
#define __SSL_H__

#include <openssl/ssl.h>


enum sslst {
  s_none,
  s_connecting,
  s_ok,
  s_error
};

struct ssl_item_s {
  SSL_CTX *ctx ;
  SSL *ssl ;
  int state;
  //void *peer ;
} ;
typedef struct ssl_item_s* ssl_item_t ;



#include "proto.h"

extern ssl_item_t ssl_init();

extern void ssl_close(Network_t net, connection_t pconn);

extern int ssl_rx(Network_t net, connection_t pconn);

extern int ssl_tx(Network_t net, connection_t pconn);

extern int ssl_connect(ssl_item_t ps, int fd);

#endif /* __SSL_H__*/
