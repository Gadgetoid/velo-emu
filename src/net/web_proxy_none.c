#include "net/web_proxy.h"

#include <stddef.h>

web_proxy_t *web_proxy_start(net_gateway_log_fn log, const char *user_agent) {
    (void)log; (void)user_agent; return NULL;
}
void        web_proxy_stop(web_proxy_t *proxy) {
    (void)proxy;
}
void        web_proxy_poll(web_proxy_t *proxy) {
    (void)proxy;
}
const char *web_proxy_socket_path(const web_proxy_t *proxy) {
    (void)proxy; return NULL;
}
