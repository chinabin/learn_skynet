#include "skynet_mq.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

struct message_queue {
	int cap;		// 消息队列能容纳的消息数
	int head;		// 指向当前可以取出消息的位置，由取出操作 skynet_mq_leave 来管理
	int tail;		// 指向当前可以插入的队列中的位置，由插入操作 skynet_mq_enter 来管理
	int lock;		// 自旋锁，确保添加消息和取出消息不出错
	struct skynet_message *queue;
};

static struct message_queue *Q = NULL;

struct message_queue * 
skynet_mq_create(int cap) {
	struct message_queue *q = malloc(sizeof(*q));
	q->cap = cap;
	q->head = 0;
	q->tail = 0;
	q->lock = 0;
	q->queue = malloc(sizeof(struct skynet_message) * cap);

	return q;
}

// 释放消息队列
void 
skynet_mq_release(struct message_queue *q) {
	free(q->queue);
	free(q);
}

// 锁定消息队列，q->lock 为 0 表示没锁
static inline void
_lock_queue(struct message_queue *q) {
	while (__sync_lock_test_and_set(&q->lock,1)) {}		// __sync_lock_test_and_set(type *ptr, type value, ...)，将*ptr设为value并返回*ptr操作之前的值。
}

// 解锁消息队列
static inline void
_unlock_queue(struct message_queue *q) {
	__sync_lock_release(&q->lock);						// void __sync_lock_release (type *ptr, ...)，将*ptr置0
}

// 从指定消息队列头中取出一个消息，返回此消息的目的地，返回 -1 表示阻塞( 也就是没消息 )
uint32_t 
skynet_mq_leave(struct message_queue *q, struct skynet_message *message) {
	uint32_t ret = 0;
	_lock_queue(q);

	if (q->head != q->tail) {
		*message = q->queue[q->head];
		ret = message->destination;
		if ( ++ q->head >= q->cap) {
			q->head = 0;
		}
	}
	
	_unlock_queue(q);

	return ret;
}

// 将新消息添加到指定队列尾，队列如果满了则扩大一倍
void 
skynet_mq_enter(struct message_queue *q, struct skynet_message *message) {
	_lock_queue(q);

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
	
	_unlock_queue(q);
}

// skynet_mq_leave 的语法糖，从 Q 中弹出队列头消息
uint32_t 
skynet_mq_pop(struct skynet_message *message) {
	return skynet_mq_leave(Q,message);
}

// skynet_mq_enter 的语法糖，将新消息插入 Q 中
void 
skynet_mq_push(struct skynet_message *message) {
	skynet_mq_enter(Q,message);
}

// skynet_mq_create 的语法糖，创建消息队列 Q
void 
skynet_mq_init(int cap) {
	Q = skynet_mq_create(cap);
}
