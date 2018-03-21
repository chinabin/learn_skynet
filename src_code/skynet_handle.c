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
	char * name;		// 服务名字
	int handle;			// 服务编号
};

struct handle_storage {
	struct rwlock lock;	// 读写锁

	/*
	 一个不停增长的索引，确保永不重复，用来计算 handle 值
	*/
	int handle_index;
	int slot_size;		// 能容纳的 skynet_context 数目
	// 下标通过 handle 计算得来：(handle & (s->slot_size-1))
	struct skynet_context ** slot;
	
	// 每个服务都可以设置一个名字(但不是必须)，名字与服务是通过 handle 来关联的
	int name_cap;		// 能容纳的 handle_name 数目，由于 handle_name 和 hadle 不是必须的一一对应，所以开始的时候这个值比较小
	int name_count;		// 已有的 handle_name 个数
	/* 
	 通过循环挨个比较 handle 来找到服务的别名
	 名字的存储是按照字母从小到大排序的
	*/
	struct handle_name *name;
};

static struct handle_storage *H = NULL;

// 注册服务，返回服务编号
int 
skynet_handle_register(struct skynet_context *ctx) {
	struct handle_storage *s = H;

	// 写锁，确保写入 slot 不冲突
	rwlock_wlock(&s->lock);
	
	for (;;) {
		int i;
		for (i=0;i<s->slot_size;i++) {
			int hash = (i+s->handle_index) & (s->slot_size-1);
			if (s->slot[hash] == NULL) {
				s->slot[hash] = ctx;
				int handle = s->handle_index + i;
				skynet_context_init(ctx, handle);	// 设置 ctx 的 handle

				rwlock_wunlock(&s->lock);

				s->handle_index = handle + 1;

				return handle;
			}
		}
		//空间不够，分配空间并迁移数据
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

//注销服务，并尝试(如果 ctx 的引用计数为0)释放对应 ctx 的资源
void
skynet_handle_retire(int handle) {
	struct handle_storage *s = H;

	rwlock_wlock(&s->lock);		// 写锁，确保对 slot 和 name 数组的操作无冲突

	int hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];
	if (skynet_context_handle(ctx) == handle) {
		skynet_context_release(ctx);
		s->slot[hash] = NULL;

		int i;
		int j=0;
		for (i=0;i<s->name_count;i++,j++) {
			if (s->name[i].handle == handle) {
				free(s->name[i].name);
				++j;
				--s->name_count;
			} else {	// 别名位置前移
				if (i!=j) {
					s->name[i] = s->name[j];
				}
			}
		}
	}

	rwlock_wunlock(&s->lock);
}

// 获取此 handle 对应的 ctx ，并将引用计数加 1
struct skynet_context * 
skynet_handle_grab(int handle) {
	struct handle_storage *s = H;
	struct skynet_context * result = NULL;

	rwlock_rlock(&s->lock);	// 读锁，确保读取 slot 数组没问题

	int hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];
	if (skynet_context_handle(ctx) == handle) {
		result = ctx;
		skynet_context_grab(result);	// 引用计数加 1
	}

	rwlock_runlock(&s->lock);

	return result;
}

// 通过名字获取对应的handle， -1 表示没有
int 
skynet_handle_findname(const char * name) {
	struct handle_storage *s = H;

	rwlock_rlock(&s->lock);	// 读锁，确保读取 slot 数组没问题

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

// 在 handle_storage 的 name 中，在索引为 before 的前面插入一个 handle_name
// 也就是将 before 位置和之后的位置都后移，然后将新的数据放在 before 位置
static void
_insert_name_before(struct handle_storage *s, char *name, int handle, int before) {
	if (s->name_count >= s->name_cap) {		//空间不够，扩容
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

//在 handle_storage 中为 name 找到一个合适的位置并插入
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

//为 handle 定义名字
const char * 
skynet_handle_namehandle(int handle, const char *name) {
	rwlock_wlock(&H->lock);		// 写锁，确保插入名字无冲突

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
	memset(s->slot, 0, s->slot_size * sizeof(struct skynet_context *));		//写错了 handle_slot ，之后改成了skynet_context

	rwlock_init(&s->lock);
	s->handle_index = 0;
	s->name_cap = 2;
	s->name_count = 0;
	s->name = malloc(s->name_cap * sizeof(struct handle_name));

	H = s;

	// Don't need to free H
}

