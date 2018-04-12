#ifndef SKYNET_MESSAGE_QUEUE_H
#define SKYNET_MESSAGE_QUEUE_H

#include <stdlib.h>
#include <stdint.h>

struct skynet_message {
	uint32_t source;			// 源的 handle
	void * data;
	size_t sz;
};

struct message_queue;

void skynet_globalmq_push(struct message_queue *);
struct message_queue * skynet_globalmq_pop(void);

struct message_queue * skynet_mq_create(uint32_t handle);
// 释放消息队列
void skynet_mq_release(struct message_queue *);
// 从指定消息队列头中取出一个消息，返回此消息的目的地，返回 -1 表示阻塞( 也就是没消息 )
uint32_t skynet_mq_handle(struct message_queue *);

// 0 for success
int skynet_mq_pop(struct message_queue *q, struct skynet_message *message);
void skynet_mq_push(struct message_queue *q, struct skynet_message *message);

struct skynet_remote_message {
	uint32_t destination;
	struct skynet_message message;
};

struct message_remote_queue;

struct message_remote_queue * skynet_remotemq_create(void);
void skynet_remotemq_release(struct message_remote_queue *);

int skynet_remotemq_pop(struct message_remote_queue *q, struct skynet_remote_message *message);
void skynet_remotemq_push(struct message_remote_queue *q, struct skynet_remote_message *message);


// skynet_mq_create 的语法糖，创建消息队列 Q
void skynet_mq_init(int cap);

#endif
