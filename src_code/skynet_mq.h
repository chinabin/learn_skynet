#ifndef SKYNET_MESSAGE_QUEUE_H
#define SKYNET_MESSAGE_QUEUE_H

#include <stdlib.h>

struct skynet_message {
	int source;			//源的 handle
	int destination;	//目标的 handle
	void * data;
	size_t sz;
};

struct message_queue;

//skynet_mq_leave的语法糖，从Q中弹出队列头消息
int skynet_mq_pop(struct skynet_message *message);
//skynet_mq_enter的语法糖，将新消息插入Q中
void skynet_mq_push(struct skynet_message *message);

struct message_queue * skynet_mq_create(int cap);
//释放消息队列
void skynet_mq_release(struct message_queue *q);
//从指定消息队列头中取出一个消息，成功则返回此消息的目的地（大于等于0）
int skynet_mq_leave(struct message_queue *q, struct skynet_message *message);
//将指定新消息添加到队列尾
void skynet_mq_enter(struct message_queue *q, struct skynet_message *message);

//skynet_mq_create的语法糖，创建消息队列Q
void skynet_mq_init(int cap);

#endif
