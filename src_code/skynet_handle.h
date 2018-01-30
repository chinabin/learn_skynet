#ifndef SKYNET_CONTEXT_HANDLE_H
#define SKYNET_CONTEXT_HANDLE_H

struct skynet_context;

//ctx只有被handle_storage管制的时候才有handle的概念

//将ctx添加到handle_storage中，并赋予此服务（ctx）唯一标识（handle），返回handle
int skynet_handle_register(struct skynet_context *);
//将handle对应的ctx从handle_storage中去除，并将此ctx的引用计数减一
void skynet_handle_retire(int handle);
//从handle_storage中获取此handle对应的ctx，并将引用计数加1
struct skynet_context * skynet_handle_grab(int handle);

//通过名字获取对应的handle，成功则返回值大于等于0
int skynet_handle_findname(const char * name);
//将新的handle name插入handle_storage，返回handle name
const char * skynet_handle_namehandle(int handle, const char *name);

//初始化handle_storage
void skynet_handle_init(void);

#endif
