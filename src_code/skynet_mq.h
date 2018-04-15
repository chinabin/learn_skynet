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

// 插入消息队列
void skynet_globalmq_push(struct message_queue *);
// 弹出一个消息队列
struct message_queue * skynet_globalmq_pop(void);

// 创建一个和服务挂钩的消息队列
struct message_queue * skynet_mq_create(uint32_t handle);
// 释放消息队列
void skynet_mq_release(struct message_queue *);
// 返回一个消息队列的 handle
uint32_t skynet_mq_handle(struct message_queue *);

// 从指定的消息队列中弹出消息，返回 -1 表示没消息，返回 0 表示获取成功
int skynet_mq_pop(struct message_queue *q, struct skynet_message *message);
// 将新消息添加到指定队列尾，队列如果满了则扩大一倍
void skynet_mq_push(struct message_queue *q, struct skynet_message *message);

struct skynet_remote_message {
	uint32_t destination;
	struct skynet_message message;
};

struct message_remote_queue;

// 创建远程消息队列
struct message_remote_queue * skynet_remotemq_create(void);
// 释放远程消息队列
void skynet_remotemq_release(struct message_remote_queue *);

// 从指定的远程消息队列中弹出消息
int skynet_remotemq_pop(struct message_remote_queue *q, struct skynet_remote_message *message);
// 往指定的远程消息队列中加消息
void skynet_remotemq_push(struct message_remote_queue *q, struct skynet_remote_message *message);


/*
 创建全局消息队列 Q
 cap: 使得消息队列的长度为 X ， X 的值是大于 cap 的第一个 2 的次方，
 	同时 X 也是二级消息队列的个数
 	例如传入 5 ，则 X 等于 8
 	例如传入 8 ，则 X 等于 16
*/
void skynet_mq_init(int cap);

#endif
