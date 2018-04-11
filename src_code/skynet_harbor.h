#ifndef SKYNET_HARBOR_H
#define SKYNET_HARBOR_H

#include <stdint.h>

struct skynet_message;

/*
 给服务名为 name 的服务发消息
 发送消息（目的地非本节点内）归根结底还是需要知道两点：
 1. 与目的节点建立连接，也就是有目的节点的 socket
 2. 知道目的地上的对应的服务的 handle id ，
 	在代码中，其实 handle id 中既有 harbor id ，又有 handle id ，
 	而 harbor id 可以对应到某个 Z->remote ，查看其 socket 是否有值。
 	如果没有，那么表示这个 skynet 节点还没有起来，那么就把消息放在其消息队列中
 	等到它起来（ master 会广播的）再传给它。
	如果有，则满足两点要求，可以将消息发送。
*/
void skynet_harbor_send(const char *name, struct skynet_message * message);

/*
 请求 注册/查询 某个服务名字
 name : 将要注册的全局服务名
 handle : 对应的服务 id 。设置为 0 表示查询 name
*/
void skynet_harbor_register(const char *name, uint32_t handle);

// remote message is diffrent from local message.
// We must use these api to open and close message , see skynet_server.c
int skynet_harbor_message_isremote(uint32_t handle);
void * skynet_harbor_message_open(struct skynet_message * message);
void skynet_harbor_message_close(struct skynet_message * message);

// harbor worker thread
void * skynet_harbor_dispatch_thread(void *ud);
void skynet_harbor_init(const char * master, const char *local, int harbor);

#endif
