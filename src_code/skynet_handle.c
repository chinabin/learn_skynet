/*
文件描述：句柄，每个服务的编号
*/

#include "skynet_handle.h"
#include "skynet_server.h"
#include "rwlock.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define DEFAULT_SLOT_SIZE 4

struct handle_name {
	char * name;		//名字
	int handle;			//句柄
};

//用来管理skynet_conetxt，ctx只有被handle_storage管制的时候才有handle的概念
//例如skynet_context_grab和skynet_handle_grab两个函数，从名字可以看出都是获取同一样东西，但是偏重点不同，前者重点是ctx本身，后者重点是handle_storage中的ctx，所以导致两个函数的实现差异
struct handle_storage {
	struct rwlock lock;

	int handle_index;
	int slot_size;
	struct skynet_context ** slot;
	
	//每个服务都可以设置一个名字，名字与服务是通过handle来关联的
	int name_cap;
	int name_count;
	struct handle_name *name;
};

static struct handle_storage *H = NULL;

//将ctx插入到handle_storage中，并赋予ctx唯一标识句柄
int 
skynet_handle_register(struct skynet_context *ctx) {
	struct handle_storage *s = H;

	rwlock_wlock(&s->lock);
	
	for (;;) {
		int i;
		for (i=0;i<s->slot_size;i++) {
			int hash = (i+s->handle_index) & (s->slot_size-1);
			if (s->slot[hash] == NULL) {
				s->slot[hash] = ctx;
				int handle = s->handle_index + i;
				skynet_context_init(ctx, handle);

				rwlock_wunlock(&s->lock);

				s->handle_index = handle + 1;

				return handle;
			}
		}
		struct skynet_context ** new_slot = malloc(s->slot_size * 2 * sizeof(struct skynet_context *));
		memset(new_slot, 0, s->slot_size * 2 * sizeof(struct skynet_context *));
		for (i=0;i<s->slot_size;i++) {
			int hash = skynet_context_handle(s->slot[i]) & (s->slot_size * 2 - 1);
			assert(new_slot[hash] == NULL);
			new_slot[hash] = s->slot[i];
		}
		free(s->slot);
		s->slot = new_slot;
		s->slot_size *= 2;
	}
}

//将handle对应的ctx从handle_storage中去除
void
skynet_handle_retire(int handle) {
	struct handle_storage *s = H;

	rwlock_wlock(&s->lock);

	int hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];
	if (skynet_context_handle(ctx) == handle) {
		skynet_context_release(ctx);	//释放ctx
		s->slot[hash] = NULL;			//将此ctx从handle_storage中去除。这里只是简单的置空，并没有将后面的位置前移。

		//每个服务都可以设置一个名字，当此服务被释放的时候，handle_storage中的名字也要删除，其后面的名字往前移。
		int i;
		int j=0;
		for (i=0;i<s->name_count;i++,j++) {
			if (s->name[i].handle == handle) {
				free(s->name[i].name);
				++j;
				--s->name_count;
			} else {
				if (i!=j) {
					s->name[i] = s->name[j];
				}
			}
		}
	}

	rwlock_wunlock(&s->lock);
}

//获取此handle对应的ctx，并将引用计数加1
struct skynet_context * 
skynet_handle_grab(int handle) {
	struct handle_storage *s = H;
	struct skynet_context * result = NULL;

	rwlock_rlock(&s->lock);

	int hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];
	if (skynet_context_handle(ctx) == handle) {
		result = ctx;
		skynet_context_grab(result);	//引用计数加1
	}

	rwlock_runlock(&s->lock);

	return result;
}

//通过名字获取对应的handle。
//从这个函数可以发现，handle_storage中名字的存储是按照字母从小到大排序的
int 
skynet_handle_findname(const char * name) {
	struct handle_storage *s = H;

	rwlock_rlock(&s->lock);

	int handle = -1;

	int begin = 0;
	int end = s->name_count - 1;
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c==0) {
			handle = n->handle;
			break;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}

	rwlock_runlock(&s->lock);

	return handle;
}

//此文件的私有函数，为skynet_handle_namehandle服务
//将一个新的用户定义的handle name插入到handle_storage中
static void
_insert_name_before(struct handle_storage *s, char *name, int handle, int before) {
	if (s->name_count >= s->name_cap) {
		s->name_cap *= 2;
		struct handle_name * n = malloc(s->name_cap * sizeof(struct handle_name));
		int i;
		for (i=0;i<before;i++) {
			n[i] = s->name[i];
		}
		for (i=before;i<s->name_count;i++) {
			n[i+1] = s->name[i];
		}
		free(s->name);
		s->name = n;
	} else {
		int i;
		for (i=s->name_count;i>=before;i--) {
			s->name[i] = s->name[i-1];
		}
	}
	s->name[before].name = name;
	s->name[before].handle = handle;
	s->name_count ++;
}

//此文件的私有函数，为skynet_handle_namehandle服务
//为即将插入的新handle name通过字母排序找到合适位置，然后插入
static const char *
_insert_name(struct handle_storage *s, const char * name, int handle) {
	int begin = 0;
	int end = s->name_count - 1;
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c==0) {
			return NULL;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}
	char * result = strdup(name);

	_insert_name_before(s, result, handle, begin);

	return result;
}

//将新的handle name插入handle_storage
const char * 
skynet_handle_namehandle(int handle, const char *name) {
	rwlock_wlock(&H->lock);

	const char * ret = _insert_name(H, name, handle);

	rwlock_wunlock(&H->lock);

	return ret;
}

//初始化handle_storage
void 
skynet_handle_init(void) {
	assert(H==NULL);
	struct handle_storage * s = malloc(sizeof(*H));
	s->slot_size = DEFAULT_SLOT_SIZE;
	s->slot = malloc(s->slot_size * sizeof(struct skynet_context *));
	memset(s->slot, 0, s->slot_size * sizeof(struct skynet_context *));		//写错了handle_slot，之后改成了skynet_context

	rwlock_init(&s->lock);
	s->handle_index = 0;
	s->name_cap = 2;
	s->name_count = 0;
	s->name = malloc(s->name_cap * sizeof(struct handle_name));

	H = s;

	// Don't need to free H
}

