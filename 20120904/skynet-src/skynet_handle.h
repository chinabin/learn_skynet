#ifndef SKYNET_CONTEXT_HANDLE_H
#define SKYNET_CONTEXT_HANDLE_H

#include <stdint.h>

#include "skynet_harbor.h"

struct skynet_context;

/*
 注册并返回 handle id ，从 ctx 进化为服务。
 handle id 的格式为：最高 8 字节为 harbor id ，其余字节为 handle id
*/
uint32_t skynet_handle_register(struct skynet_context *);
/*
 注销 handle id ，退回 ctx 的角色。如果 ctx 没有额外的引用，则会彻底销毁。
*/
void skynet_handle_retire(uint32_t handle);
/*
 获取 handle 对应的 ctx
*/
struct skynet_context * skynet_handle_grab(uint32_t handle);

/*
 获取 name 对应的 handle 。失败则返回 0 。
*/
uint32_t skynet_handle_findname(const char * name);
/*
 为 handle 添加 name 别名。
 如果 name 已存在则返回 NULL ，否则返回 name 。
 名字是排序存储的。
*/
const char * skynet_handle_namehandle(uint32_t handle, const char *name);

/*
 初始化 handle storage
*/
void skynet_handle_init(int harbor);

#endif
