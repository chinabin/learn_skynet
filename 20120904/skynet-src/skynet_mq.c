#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_multicast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define DEFAULT_QUEUE_SIZE 64;

// 二级消息队列，和服务挂钩
struct message_queue {
	uint32_t handle;
	int cap;
	int head;	// 指向当前可以取出消息的位置，由取出操作 skynet_mq_pop 来管理
	int tail;	// 指向当前可以插入的队列中的位置，由插入操作 skynet_mq_push 来管理
	int lock;
	int release;	// 二级消息队列释放标志
	int in_global;		// 初始置位。置位表示二级消息队列中还有消息，否则表示二级消息队列中消息为空。
	struct skynet_message *queue;
};

// 全局(一级)消息队列
struct global_queue {
	int cap;
	int head;	// 指向当前可以取出消息队列的位置，由取出操作 skynet_globalmq_pop 来管理
	int tail;	// 指向当前可以插入消息队列的位置，由插入操作 skynet_globalmq_push 管理
	int lock;
	struct message_queue ** queue;	// 二维消息数组
};

static struct global_queue *Q = NULL;

#define LOCK(q) while (__sync_lock_test_and_set(&(q)->lock,1)) {}
#define UNLOCK(q) __sync_lock_release(&(q)->lock);

static void 
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q= Q;
	LOCK(q)

	q->queue[q->tail] = queue;
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}

	if (q->head == q->tail) {
		struct message_queue **new_queue = malloc(sizeof(struct message_queue *) * q->cap * 2);
		int i;
		for (i=0;i<q->cap;i++) {
			new_queue[i] = q->queue[(q->head + i) % q->cap];
		}
		q->head = 0;
		q->tail = q->cap;
		q->cap *= 2;
		
		free(q->queue);
		q->queue = new_queue;
	}

	UNLOCK(q)
}

/*
 从全局消息队列中按照轮训弹出一个二级消息队列。
*/
struct message_queue * 
skynet_globalmq_pop() {
	struct global_queue *q = Q;
	struct message_queue * ret = NULL;
	LOCK(q)

	if (q->head != q->tail) {
		ret = q->queue[q->head];
		if ( ++ q->head >= q->cap) {
			q->head = 0;
		}
	}

	UNLOCK(q)
	
	return ret;
}

/*
 创建二级消息队列
*/
struct message_queue * 
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = malloc(sizeof(*q));
	q->handle = handle;
	q->cap = DEFAULT_QUEUE_SIZE;
	q->head = 0;
	q->tail = 0;
	q->lock = 0;
	q->in_global = 1;
	q->release = 0;
	q->queue = malloc(sizeof(struct skynet_message) * q->cap);

	return q;
}

static void 
_release(struct message_queue *q) {
	free(q->queue);
	free(q);
}

/*
 返回二级消息队列对应的 handle id
*/
uint32_t 
skynet_mq_handle(struct message_queue *q) {
	return q->handle;
}

/*
 从二级消息队列中轮询弹出一个消息，返回 0 表示取到消息。
*/
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;
	LOCK(q)

	if (q->head != q->tail) {
		*message = q->queue[q->head];
		ret = 0;
		if ( ++ q->head >= q->cap) {
			q->head = 0;
		}
	}

	if (ret) {
		q->in_global = 0;
	}
	
	UNLOCK(q)

	return ret;
}

/*
 往二级消息队列中添加一个消息
*/
void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	LOCK(q)

	if (message) {
		q->queue[q->tail] = *message;
		if (++ q->tail >= q->cap) {
			q->tail = 0;
		}

		if (q->head == q->tail) {
			struct skynet_message *new_queue = malloc(sizeof(struct skynet_message) * q->cap * 2);
			int i;
			for (i=0;i<q->cap;i++) {
				new_queue[i] = q->queue[(q->head + i) % q->cap];
			}
			q->head = 0;
			q->tail = q->cap;
			q->cap *= 2;
			
			free(q->queue);
			q->queue = new_queue;
		}
	}

	if (q->in_global == 0) {
		q->in_global = 1;
		skynet_globalmq_push(q);
	}
	
	UNLOCK(q)
}

/*
 初始化全局消息队列。
 假设二级消息队列的个数为 X ，则 n <= X <= 2 ^ m
*/
void 
skynet_mq_init(int n) {
	struct global_queue *q = malloc(sizeof(*q));
	memset(q,0,sizeof(*q));
	int cap = 2;
	while (cap < n) {
		cap *=2;
	}
	
	q->cap = cap;
	q->queue = malloc(cap * sizeof(struct skynet_message*));
	Q=q;
}

void 
skynet_mq_force_push(struct message_queue * queue) {
	assert(queue->in_global);
	skynet_globalmq_push(queue);
}

/*
 将二级消息队列标记为释放
*/
void 
skynet_mq_mark_release(struct message_queue *q) {
	assert(q->release == 0);
	q->release = 1;
}

// QUESTION: 不懂
static int
_drop_queue(struct message_queue *q) {
	// todo: send message back to message source
	struct skynet_message msg;
	int s = 0;
	while(!skynet_mq_pop(q, &msg)) {
		++s;
		int type = msg.sz >> HANDLE_REMOTE_SHIFT;
		if (type == PTYPE_MULTICAST) {
			assert(msg.sz == 0);
			skynet_multicast_dispatch((struct skynet_multicast_message *)msg.data, NULL, NULL);
		} else {
			free(msg.data);
		}
	}
	_release(q);
	return s;
}

/*
 两层含义：
 1. 如果有 release 标志，则丢弃二级消息队列。
 2. 如果无 release 标志，则使得二级消息队列中的消息被更快消耗完。
*/
int 
skynet_mq_release(struct message_queue *q) {
	int ret = 0;
	LOCK(q)
	
	if (q->release) {
		UNLOCK(q)
		ret = _drop_queue(q);
	} else {
		skynet_mq_force_push(q);
		UNLOCK(q)
	}
	
	return ret;
}
