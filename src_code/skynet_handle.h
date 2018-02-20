#ifndef SKYNET_CONTEXT_HANDLE_H
#define SKYNET_CONTEXT_HANDLE_H

struct skynet_context;

//ctx只有被handle_storage管制的时候才有handle的概念

//将 ctx 注册成为服务( handle ) ， 存储在 handle_storage 中
//返回唯一服务( handle )标识
int skynet_handle_register(struct skynet_context *);
//注销服务
void skynet_handle_retire(int handle);
//从handle_storage中获取此handle对应的ctx，并将引用计数加1
struct skynet_context * skynet_handle_grab(int handle);

//通过名字获取对应的handle，-1表示没有
int skynet_handle_findname(const char * name);
//为 handle 定义名字，返回 name 表示成功，返回 NULL 表示插入失败(名字已存在)
const char * skynet_handle_namehandle(int handle, const char *name);

//初始化 handle_storage
void skynet_handle_init(void);

#endif
