#include "skynet_module.h"

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_MODULE_TYPE 32
#define SO ".so"

//用来管理skynet_module
struct modules {
	int count;		//当前已经存在的so库个数
	int lock;		//锁，防止同时添加
	const char * path;	//所有so库默认路径
	struct skynet_module m[MAX_MODULE_TYPE];
};

static struct modules * M = NULL;
 
//打开 name 所指定的so库并返回动态链接库句柄，失败返回NULL
static void *
_try_open(struct modules *m, const char * name) {
	size_t path_size = strlen(m->path);
	size_t name_size = strlen(name);
	char tmp[path_size + name_size + sizeof(SO)];
	memcpy(tmp, m->path, path_size);
	memcpy(tmp + path_size , name, name_size);
	memcpy(tmp + path_size + name_size , SO, sizeof(SO));

	void * dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);

	if (dl == NULL) {
		printf("try open %s failed : %s\n",tmp,dlerror());
	}

	return dl;
}

//查找 name 对应的so库是否已经加载
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

//给三个约定的函数指针赋值
//返回0表示成功
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

//将 name 对应的so库加载进 modules ，如果已存在，则返回
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

void * 
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) {
		return m->create();
	} else {
		return (void *)(intptr_t)(~0);
	}
}

int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
	return m->init(inst, ctx, parm);
}

void 
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) {
		m->release(inst);
	}
}

//传入默认so库的路径来初始化modules
void 
skynet_module_init(const char *path) {
	struct modules *m = malloc(sizeof(*m));
	m->count = 0;
	m->path = strdup(path);
	m->lock = 0;

	M = m;
}
