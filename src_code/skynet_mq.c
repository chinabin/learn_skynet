#include "skynet_mq.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

struct message_queue {
	int cap;		//消息队列能容纳的消息数
	int head;		//指向当前可以取出的消息位置
	int tail;		//指向当前可以插入的队列中的位置
	int lock;
	struct skynet_message *queue;
};

static struct message_queue *Q = NULL;

//传入大小创建消息队列
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

//释放消息队列
void 
skynet_mq_release(struct message_queue *q) {
	free(q->queue);
	free(q);
}

//锁定消息队列，q->lock为0表示没锁
static inline void
_lock_queue(struct message_queue *q) {
	while (__sync_lock_test_and_set(&q->lock,1)) {}		//__sync_lock_test_and_set(type *ptr, type value, ...)，将*ptr设为value并返回*ptr操作之前的值。
}

//解锁消息队列
static inline void
_unlock_queue(struct message_queue *q) {
	__sync_lock_release(&q->lock);						//void __sync_lock_release (type *ptr, ...)，将*ptr置0
}

//从指定消息队列头中取出一个消息，返回此消息的目的地
int 
skynet_mq_leave(struct message_queue *q, struct skynet_message *message) {
	int ret = -1;
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

//将新消息添加到指定队列尾
void 
skynet_mq_enter(struct message_queue *q, struct skynet_message *message) {
	_lock_queue(q);

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
	
	_unlock_queue(q);
}

//skynet_mq_leave的语法糖，从Q中弹出队列头消息
int 
skynet_mq_pop(struct skynet_message *message) {
	return skynet_mq_leave(Q,message);
}

//skynet_mq_enter的语法糖，将新消息插入Q中
void 
skynet_mq_push(struct skynet_message *message) {
	skynet_mq_enter(Q,message);
}

//skynet_mq_create的语法糖，创建消息队列Q
void 
skynet_mq_init(int cap) {
	Q = skynet_mq_create(cap);
}