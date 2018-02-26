#ifndef SKYNET_MODULE_H
#define SKYNET_MODULE_H

struct skynet_context;

typedef void * (*skynet_dl_create)(void);
typedef int (*skynet_dl_init)(void * inst, struct skynet_context *, const char * parm);
typedef void (*skynet_dl_release)(void * inst);

// skynet 中对so库的抽象
struct skynet_module {
	const char * name;			//so库名
	void * module;				//动态链接库句柄
	skynet_dl_create create;	// 调用创建函数，返回实例指针，之后作为 init 函数的参数传入
	skynet_dl_init init;		// 返回 0 为成功
	skynet_dl_release release;
};

/*
 将skynet_module插入modules，暂时没有使用
*/
void skynet_module_insert(struct skynet_module *mod);
/*
 将 name 对应的so库加载进 modules ，如果已存在，则返回
*/
struct skynet_module * skynet_module_query(const char * name);

/*
 调用 create 接口，返回实例指针，后面会传入 init 函数和 release 函数
*/
void * skynet_module_instance_create(struct skynet_module *);
/*
 init 接口实现中需要设置好回调接口
 返回0表示成功
*/
int skynet_module_instance_init(struct skynet_module *, void * inst, struct skynet_context *ctx, const char * parm);
void skynet_module_instance_release(struct skynet_module *, void *inst);

/*
 传入默认so库的路径来初始化 modules 结构体
*/
void skynet_module_init(const char *path);

#endif
