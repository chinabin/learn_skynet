#ifndef SKYNET_SERVER_H
#define SKYNET_SERVER_H

#include <stdint.h>

struct skynet_context;
struct skynet_message;

/*
 创建新的 ctx 并返回
 name: 模块名
 parm: 模块的 init 接口使用的参数之一
*/
struct skynet_context * skynet_context_new(const char * name, const char * parm);
// 增加 ctx 的引用计数
void skynet_context_grab(struct skynet_context *);
// ctx 释放
// 引用计数 为 0 则销毁
struct skynet_context * skynet_context_release(struct skynet_context *);
// 返回 ctx 的 handle
uint32_t skynet_context_handle(struct skynet_context *);
// 设置 ctx 的 handle
void skynet_context_init(struct skynet_context *, uint32_t handle);
// 未实现
int skynet_context_push(uint32_t handle, struct skynet_message *message);
/*
 从全局消息队列中取出消息分发，返回 1 表示阻塞，当前无消息
 这个时候的消息队列比较简单，所有消息都是放进全局消息队列中，服务的私有消息队列只是
 存放当服务被占用导致无法处理消息的时候的消息
*/
int skynet_context_message_dispatch(void);	// return 1 when block

#endif
