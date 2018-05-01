#ifndef SKYNET_MESSAGE_QUEUE_H
#define SKYNET_MESSAGE_QUEUE_H

#include <stdlib.h>
#include <stdint.h>

struct skynet_message {
	uint32_t source;
	int session;
	void * data;
	size_t sz;
};

struct message_queue;

/*
 从全局消息队列中按照轮训弹出一个二级消息队列。
*/
struct message_queue * skynet_globalmq_pop(void);

/*
 创建二级消息队列
*/
struct message_queue * skynet_mq_create(uint32_t handle);
/*
 将二级消息队列标记为释放
*/
void skynet_mq_mark_release(struct message_queue *q);
/*
 两层含义：
 1. 如果有 release 标志，则丢弃二级消息队列。
 2. 如果无 release 标志，则使得二级消息队列中的消息被更快消耗完。
*/
int skynet_mq_release(struct message_queue *q);
/*
 返回二级消息队列对应的 handle id
*/
uint32_t skynet_mq_handle(struct message_queue *);

// 0 for success
/*
 从二级消息队列中轮询弹出一个消息，返回 0 表示取到消息。
*/
int skynet_mq_pop(struct message_queue *q, struct skynet_message *message);
/*
 往二级消息队列中添加一个消息
*/
void skynet_mq_push(struct message_queue *q, struct skynet_message *message);
/*
 强制将一个已存在于全局消息队列中的二级消息队列再次添加。
 猜测是为了使得 q 中的消息能够更快被消耗。因为本身是轮询机制，
 现在在一次轮询中就可以多次被访问。
*/
void skynet_mq_force_push(struct message_queue *q);

/*
 初始化全局消息队列。
 假设二级消息队列的个数为 X ，则 n <= X <= 2 ^ m
*/
void skynet_mq_init(int cap);

#endif
