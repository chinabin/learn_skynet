#include "skynet_mq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define DEFAULT_QUEUE_SIZE 64;

// 二级消息队列，和服务挂钩
struct message_queue {
	uint32_t handle;	// 服务 id
	int cap;		// 消息队列能容纳的消息数
	int head;		// 指向当前可以取出消息的位置，由取出操作 skynet_mq_pop 来管理
	int tail;		// 指向当前可以插入的队列中的位置，由插入操作 skynet_mq_push 来管理
	int lock;		// 自旋锁，确保添加消息和取出消息不出错
	struct skynet_message *queue;
};

// 全局消息队列
struct global_queue {
	int cap;
	int head;		// 指向当前可以取出消息的位置，由取出操作 skynet_globalmq_pop 来管理
	int tail;		// 指向当前可以插入的队列中的位置，由插入操作 skynet_globalmq_push 管理
	int lock;
	struct message_queue ** queue;	// 消息队列数组，里面存储的是各个服务相关的消息队列
};

// 远程消息队列
struct message_remote_queue {
	int cap;
	int head;		// 指向当前可以取出消息的位置，由取出操作 skynet_remotemq_pop 来管理
	int tail;		// 指向当前可以插入的队列中的位置，由插入操作 skynet_remotemq_push 管理
	int lock;
	struct skynet_remote_message *queue;
};

static struct global_queue *Q = NULL;

static inline void
_lock_global_queue() {
	while (__sync_lock_test_and_set(&Q->lock,1)) {}
}

#define LOCK(q) while (__sync_lock_test_and_set(&(q)->lock,1)) {}
#define UNLOCK(q) __sync_lock_release(&(q)->lock);

// 插入消息队列
void 
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

// 弹出一个消息队列
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

// 创建一个和服务挂钩的消息队列
struct message_queue * 
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = malloc(sizeof(*q));
	q->handle = handle;
	q->cap = DEFAULT_QUEUE_SIZE;
	q->head = 0;
	q->tail = 0;
	q->lock = 0;
	q->queue = malloc(sizeof(struct skynet_message) * q->cap);

	return q;
}

// 释放消息队列
void 
skynet_mq_release(struct message_queue *q) {
	free(q->queue);
	free(q);
}

// 返回一个消息队列的 handle
uint32_t 
skynet_mq_handle(struct message_queue *q) {
	return q->handle;
}

// 从指定的消息队列中弹出消息，返回 -1 表示没消息，返回 0 表示获取成功
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = -1;
	LOCK(q)

	if (q->head != q->tail) {
		*message = q->queue[q->head];
		ret = 0;
		if ( ++ q->head >= q->cap) {
			q->head = 0;
		}
	}
	
	UNLOCK(q)

	return ret;
}

// 将新消息添加到指定队列尾，队列如果满了则扩大一倍
void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	LOCK(q)

	q->queue[q->tail] = *message;
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}

	// 消息队列满了，需要扩展
	if (q->head == q->tail) {
		struct skynet_message *new_queue = malloc(sizeof(struct skynet_message) * q->cap * 2);
		int i;
		// 确保消息顺序没乱
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
 创建全局消息队列 Q
 cap: 使得消息队列的长度为 X ， X 的值是大于 cap 的第一个 2 的次方，
 	同时 X 也是二级消息队列的个数
 	例如传入 5 ，则 X 等于 8
 	例如传入 8 ，则 X 等于 16
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

// 创建远程消息队列
// remote message queue
struct message_remote_queue * 
skynet_remotemq_create(void) {
	struct message_remote_queue *q = malloc(sizeof(*q));
	q->cap = DEFAULT_QUEUE_SIZE;
	q->head = 0;
	q->tail = 0;
	q->lock = 0;
	q->queue = malloc(sizeof(struct skynet_remote_message) * q->cap);

	return q;
}

// 释放远程消息队列
void 
skynet_remotemq_release(struct message_remote_queue *q) {
	free(q->queue);
	free(q);
}

// 从指定的远程消息队列中弹出消息
int 
skynet_remotemq_pop(struct message_remote_queue *q, struct skynet_remote_message *message) {
	int ret = -1;
	LOCK(q)

	if (q->head != q->tail) {
		*message = q->queue[q->head];
		ret = 0;
		if ( ++ q->head >= q->cap) {
			q->head = 0;
		}
	}
	
	UNLOCK(q)

	return ret;
}

// 往指定的远程消息队列中加消息
void 
skynet_remotemq_push(struct message_remote_queue *q, struct skynet_remote_message *message) {
	assert(message->destination != 0);
	LOCK(q)

	q->queue[q->tail] = *message;

	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}

	if (q->head == q->tail) {
		struct skynet_remote_message *new_queue = malloc(sizeof(struct skynet_remote_message) * q->cap * 2);
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
