#ifndef SKYNET_H
#define SKYNET_H

/*
对第三方提供的接口，让第三方可以编写自己的服务
*/
#include <stddef.h>
#include <stdint.h>

struct skynet_context;

// 从 context->handle 中给 logger 发消息
void skynet_error(struct skynet_context * context, const char *msg, ...);
const char * skynet_command(struct skynet_context * context, const char * cmd , int session, const char * parm);
/*
 服务 context->handle 给服务 addr 发消息
 addr: 如果以':'开头则后面跟的是 handle ，如果以'.'开头则后面跟的是 handle name
*/
int skynet_send(struct skynet_context * context, const char * addr , int session, void * msg, size_t sz);

/*
 context 是服务指针
 ud 是 skynet_callback 设置的第二个参数
 addr 是源服务地址
 msg 是消息数据
 sz 是一个约定号
*/
typedef void (*skynet_cb)(struct skynet_context * context, void *ud, int session, const char * addr , const void * msg, size_t sz);
// 设置 ctx 的 回调函数接口以及传入回调函数的第二个参数
void skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb);

#endif
