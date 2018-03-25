#ifndef SKYNET_MESSAGE_QUEUE_H
#define SKYNET_MESSAGE_QUEUE_H

#include <stdlib.h>
#include <stdint.h>

struct skynet_message {
	uint32_t source;			// 源的 handle
	uint32_t destination;	// 目标的 handle
	void * data;
	size_t sz;
};

struct message_queue;

// skynet_mq_leave 的语法糖，从 Q 中弹出队列头消息
uint32_t skynet_mq_pop(struct skynet_message *message);
// skynet_mq_enter 的语法糖，将新消息插入 Q 中
void skynet_mq_push(struct skynet_message *message);

struct message_queue * skynet_mq_create(int cap);
// 释放消息队列
void skynet_mq_release(struct message_queue *q);
// 从指定消息队列头中取出一个消息，返回此消息的目的地，返回 -1 表示阻塞( 也就是没消息 )
uint32_t skynet_mq_leave(struct message_queue *q, struct skynet_message *message);
// 将新消息添加到指定队列尾，队列如果满了则扩大一倍
void skynet_mq_enter(struct message_queue *q, struct skynet_message *message);

// skynet_mq_create 的语法糖，创建消息队列 Q
void skynet_mq_init(int cap);

#endif
