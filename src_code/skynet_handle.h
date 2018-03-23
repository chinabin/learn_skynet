#ifndef SKYNET_CONTEXT_HANDLE_H
#define SKYNET_CONTEXT_HANDLE_H

#include <stdint.h>

// reserve high 8 bits for remote id
// see skynet_harbor.c REMOTE_MAX
#define HANDLE_MASK 0xffffff
#define HANDLE_REMOTE_SHIFT 24

struct skynet_context;

// 注册服务，返回服务编号
uint32_t skynet_handle_register(struct skynet_context *);
//注销服务，并尝试(如果 ctx 的引用计数为0)释放对应 ctx 的资源
void skynet_handle_retire(uint32_t handle);
//从 handle_storage 中获取此 handle 对应的 ctx ，并将引用计数加1
struct skynet_context * skynet_handle_grab(int handle);

//通过名字获取对应的 handle ，-1表示没有
uint32_t skynet_handle_findname(const char * name);
//为 handle 定义名字，返回 name 表示成功，返回 NULL 表示插入失败(名字已存在)
const char * skynet_handle_namehandle(uint32_t handle, const char *name);

//初始化 handle_storage
void skynet_handle_init(int harbor);

#endif
