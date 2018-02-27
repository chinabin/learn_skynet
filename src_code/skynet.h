#ifndef SKYNET_H
#define SKYNET_H

#include <stddef.h>

struct skynet_context;

// 从 context->handle 中给 logger 发消息
void skynet_error(struct skynet_context * context, const char *msg, ...);
const char * skynet_command(struct skynet_context * context, const char * cmd , const char * parm);
/*
 服务 context->handle 给服务 addr 发消息
 addr: 如果以':'开头则后面跟的是 handle ，如果以'.'开头则后面跟的是 handle name
*/
void skynet_send(struct skynet_context * context, const char * addr , void * msg, size_t sz_session);

typedef void (*skynet_cb)(struct skynet_context * context, void *ud, const char * uid , const void * msg, size_t sz_session);
// 设置 ctx 的 回调函数接口以及传入回调函数的第二个参数
void skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb);

#endif
