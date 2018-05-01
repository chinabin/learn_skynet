#include "skynet_module.h"

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_MODULE_TYPE 32

struct modules {
	int count;
	int lock;
	const char * path;
	struct skynet_module m[MAX_MODULE_TYPE];
};

static struct modules * M = NULL;

/*
 从默认的动态链接库目录找到并打开 name 所指定的so库，
 返回动态链接库句柄，失败返回NULL
*/
static void *
_try_open(struct modules *m, const char * name) {
	const char * path = m->path;
	size_t path_size = strlen(path);
	size_t name_size = strlen(name);

	int sz = path_size + name_size;
	char tmp[sz];
	int i;
	for (i=0;path[i]!='?' && path[i]!='\0';i++) {
		tmp[i] = path[i];
	}
	memcpy(tmp+i,name,name_size);
	if (path[i] == '?') {
		strcpy(tmp+i+name_size,path+i+1);
	} else {
		fprintf(stderr,"Invalid C service path\n");
		exit(1);
	}

	/*
	 RTLD_NOW: 在dlopen返回前，解析出每个未定义变量的地址，如果解析不出来，
	 返回NULL，并抛出错误信息。
	 RTLD_GLOBAL: 使得库中的解析的定义变量在随后的其它的链接库中变得可以使用。
	*/
	void * dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);

	if (dl == NULL) {
		fprintf(stderr, "try open %s failed : %s\n",tmp,dlerror());
	}

	return dl;
}

/*
 查询 name 对应的so库是否已经加载。
 如果已经加载则返回对应的 skynet_module 指针
 否则返回 NULL
*/
static struct skynet_module * 
_query(const char * name) {
	int i;
	for (i=0;i<M->count;i++) {
		if (strcmp(M->m[i].name,name)==0) {
			return &M->m[i];
		}
	}
	return NULL;
}

/*
 给三个约定的函数指针赋值
 返回0表示成功
*/
static int
_open_sym(struct skynet_module *mod) {
	size_t name_size = strlen(mod->name);
	char tmp[name_size + 9]; // create/init/release , longest name is release (7)
	memcpy(tmp, mod->name, name_size);
	strcpy(tmp+name_size, "_create");
	mod->create = dlsym(mod->module, tmp);
	strcpy(tmp+name_size, "_init");
	mod->init = dlsym(mod->module, tmp);
	strcpy(tmp+name_size, "_release");
	mod->release = dlsym(mod->module, tmp);

	return mod->init == NULL;
}

/*
 将 name 对应的so库加载进 modules ，并返回 skynet_module 指针
*/
struct skynet_module * 
skynet_module_query(const char * name) {
	struct skynet_module * result = _query(name);
	if (result)
		return result;

	while(__sync_lock_test_and_set(&M->lock,1)) {}

	result = _query(name); // double check

	if (result == NULL && M->count < MAX_MODULE_TYPE) {
		int index = M->count;
		void * dl = _try_open(M,name);
		if (dl) {
			M->m[index].name = name;
			M->m[index].module = dl;

			if (_open_sym(&M->m[index]) == 0) {
				M->m[index].name = strdup(name);
				M->count ++;
				result = &M->m[index];
			}
		}
	}

	__sync_lock_release(&M->lock);

	return result;
}

/*
 将 skynet_module 插入到 modules
*/
void 
skynet_module_insert(struct skynet_module *mod) {
	while(__sync_lock_test_and_set(&M->lock,1)) {}

	struct skynet_module * m = _query(mod->name);
	assert(m == NULL && M->count < MAX_MODULE_TYPE);
	int index = M->count;
	M->m[index] = *mod;
	++M->count;
	__sync_lock_release(&M->lock);
}

/*
 调用 create 接口，返回实例指针。
 后面会传入 init 函数和 release 函数。
*/
void * 
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) {
		return m->create();
	} else {
		return (void *)(intptr_t)(~0);
	}
}

/*
 调用 init 接口。
 1. init 接口实现中需要调用 skynet_callback 设置好回调接口，之后处理消息的时候需要调用
 2. 返回0表示成功
*/
int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
	return m->init(inst, ctx, parm);
}

/*
 调用 release 接口。
*/
void 
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) {
		m->release(inst);
	}
}

/*
 初始化 modules
*/
void 
skynet_module_init(const char *path) {
	struct modules *m = malloc(sizeof(*m));
	m->count = 0;
	m->path = strdup(path);
	m->lock = 0;

	M = m;
}
