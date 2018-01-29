#ifndef SKYNET_SERVER_H
#define SKYNET_SERVER_H

struct skynet_context;
struct skynet_message;

//传入模块名和参数，创建新的ctx并返回
struct skynet_context * skynet_context_new(const char * name, char * parm);
//ctx->ref加1并返回更新后的值
void skynet_context_grab(struct skynet_context *);
//ctx的引用计数减1，如果ctx的引用计数为0则删除ctx
struct skynet_context * skynet_context_release(struct skynet_context *);
int skynet_context_handle(struct skynet_context *);
void skynet_context_init(struct skynet_context *, int handle);
void skynet_context_push(struct skynet_context *, struct skynet_message *message);
int skynet_context_pop(struct skynet_context *, struct skynet_message *message);
int skynet_context_message_dispatch(void);	// return 1 when block

#endif
