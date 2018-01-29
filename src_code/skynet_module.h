#ifndef SKYNET_MODULE_H
#define SKYNET_MODULE_H

struct skynet_context;

typedef void * (*skynet_dl_create)(void);
typedef int (*skynet_dl_init)(void * inst, struct skynet_context *, const char * parm);
typedef void (*skynet_dl_release)(void * inst);

//skynet_module是skynet中对SO模块的抽象
struct skynet_module {
	const char * name;
	void * module;
	skynet_dl_create create;
	skynet_dl_init init;
	skynet_dl_release release;
};

//将skynet_module插入modules，暂时没有使用
void skynet_module_insert(struct skynet_module *mod);
//传入模块名，返回对应的skynet_module。
//如果此模块没有预先创建则创建并添加到modules中，如果此模块对应的SO文件不在指定的路径则返回NULL。
struct skynet_module * skynet_module_query(const char * name);
//调用skynet_module的create函数创建实例
void * skynet_module_instance_create(struct skynet_module *);
//调用skynet_module的init函数初始化实例，返回0表示成功
int skynet_module_instance_init(struct skynet_module *, void * inst, struct skynet_context *ctx, const char * parm);
//调用skynet_module的release函数释放实例
void skynet_module_instance_release(struct skynet_module *, void *inst);

//传入默认路径来初始化modules
void skynet_module_init(const char *path);

#endif
