#ifndef SKYNET_MODULE_H
#define SKYNET_MODULE_H

struct skynet_context;

typedef void * (*skynet_dl_create)(void);
typedef int (*skynet_dl_init)(void * inst, struct skynet_context *, const char * parm);
typedef void (*skynet_dl_release)(void * inst);

struct skynet_module {
	const char * name;
	void * module;
	skynet_dl_create create;
	skynet_dl_init init;
	skynet_dl_release release;
};

/*
 将 skynet_module 插入到 modules
*/
void skynet_module_insert(struct skynet_module *mod);
/*
 将 name 对应的so库加载进 modules ，并返回 skynet_module 指针
*/
struct skynet_module * skynet_module_query(const char * name);
/*
 调用 create 接口，返回实例指针。
 后面会传入 init 函数和 release 函数。
*/
void * skynet_module_instance_create(struct skynet_module *);
/*
 调用 init 接口。
 1. init 接口实现中需要调用 skynet_callback 设置好回调接口，之后处理消息的时候需要调用
 2. 返回0表示成功
*/
int skynet_module_instance_init(struct skynet_module *, void * inst, struct skynet_context *ctx, const char * parm);
/*
 调用 release 接口。
*/
void skynet_module_instance_release(struct skynet_module *, void *inst);

/*
 初始化 modules
*/
void skynet_module_init(const char *path);

#endif
