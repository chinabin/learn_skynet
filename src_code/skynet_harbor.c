#include "skynet_harbor.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_system.h"
#include "skynet_server.h"
#include "skynet.h"

#include <zmq.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#define HASH_SIZE 4096
#define DEFAULT_QUEUE_SIZE 1024

// see skynet_handle.h for HANDLE_REMOTE_SHIFT
#define REMOTE_MAX 255

struct keyvalue {
	struct keyvalue * next;
	uint32_t hash;
	char * key;							// 全局服务名
	uint32_t value;						// 全局服务名对应的 handle id
	struct message_queue * queue;		// 发给某个具有名字的服务，但是这个名字对于本地 skynet 暂时还是不认识的，消息就放在这。
};

struct hashmap {
	struct keyvalue *node[HASH_SIZE];
};

struct remote_header {
	uint32_t source;
	uint32_t destination;
	uint32_t session;
};

struct remote {
	void *socket;
	struct message_remote_queue *queue;		// 发给某个 harbor ，只是这个 harbor 还没起来，则先把消息存在这里。
};

struct harbor {
	void * zmq_context;					// zmq 上下文
	void * zmq_master_request;			// 连接 master 的 socket ，用来查询 skynet 地址或者查询全局服务 id。 REQ 	客户端
	void * zmq_local;					// 连接 master 的 socket ，用来接收广播消息： skynet 节点地址更新，全局服务名及其地址更新
	/*
	 zmq_queue_notice 是线程内通信的 socket
	 这个通信的流程类似一种提醒，例如 skynet_harbor_register 注册全局服务名，先准备好消息，
	 之后调用 send_notice 使得 skynet_harbor_dispatch_thread 的 _remote_send 被调用，然后
	 从消息队列中取出注册全局服务名的消息。
	*/
	void * zmq_queue_notice;			// ZMQ_PULL 服务端
	int notice_event;					// 标识 notice 是否处理
	struct hashmap *map;				// 保存服务名字和 handle id 键值对
	struct remote remote[REMOTE_MAX];	// 远端 harbor 存储
	struct message_remote_queue *queue;	// 发往远端的消息，并且已经与之建立了联系，则把消息放这。向远端查询名字。
	int harbor;							// harbor id

	int lock;
};

static struct harbor *Z = NULL;

// todo: optimize for little endian system

static inline void
buffer_to_remote_header(uint8_t *buffer, struct remote_header *header) {
	header->source = buffer[0] | buffer[1] << 8 | buffer[2] << 16 | buffer[3] << 24;
	header->destination = buffer[4] | buffer[5] << 8 | buffer[6] << 16 | buffer[7] << 24;
	header->session = buffer[8] | buffer[9] << 8 | buffer[10] << 16 | buffer[11] << 24;
}

static inline void
remote_header_to_buffer(struct remote_header *header, uint8_t *buffer) {
	buffer[0] = header->source & 0xff;
	buffer[1] = (header->source >> 8) & 0xff;
	buffer[2] = (header->source >>16) & 0xff;
	buffer[3] = (header->source >>24)& 0xff;
	buffer[4] = (header->destination) & 0xff;
	buffer[5] = (header->destination >>8) & 0xff;
	buffer[6] = (header->destination >>16) & 0xff;
	buffer[7] = (header->destination >>24) & 0xff;
	buffer[8] = (header->session) & 0xff;
	buffer[9] = (header->session >>8) & 0xff;
	buffer[10] = (header->session >>16) & 0xff;
	buffer[11] = (header->session >>24) & 0xff;
}

static uint32_t
calc_hash(const char *name) {
	int i;
	uint32_t h = 0;
	for (i=0;name[i];i++) {
	    h = h ^ ((h<<5)+(h>>2)+(uint8_t)name[i]);
	}
	h ^= i;
	return h;
}

static inline void
_lock() {
	while (__sync_lock_test_and_set(&Z->lock,1)) {}
}

static inline void
_unlock() {
	__sync_lock_release(&Z->lock);
}

static struct hashmap *
_hash_new(void) {
	struct hashmap * hash = malloc(sizeof(*hash));
	memset(hash, 0, sizeof(*hash));
	return hash;
}

static struct keyvalue *
_hash_search(struct hashmap * hash, const char * key) {
	uint32_t h = calc_hash(key);
	struct keyvalue * n = hash->node[h & (HASH_SIZE-1)];
	while (n) {
		if (n->hash == h && strcmp(n->key, key) == 0) {
			return n;
		}
		n = n->next;
	}
	return NULL;
}

static void
_hash_insert(struct hashmap * hash, const char * key, uint32_t handle, struct message_queue *queue) {
	uint32_t h = calc_hash(key);
	struct keyvalue * node = malloc(sizeof(*node));
	node->next = hash->node[h & (HASH_SIZE-1)];
	node->hash = h;
	node->key = strdup(key);
	node->value = handle;
	node->queue = queue;

	hash->node[h & (HASH_SIZE-1)] = node;
}

/*
 给哨兵发消息：嘿，告诉大佬，有事做了
*/
// thread safe function
static void
send_notice() {
	if (__sync_lock_test_and_set(&Z->notice_event,1)) {
		// already send notice
		return;
	}
	static __thread void * queue_notice = NULL;
	if (queue_notice == NULL) {
		void * pub = zmq_socket(Z->zmq_context, ZMQ_PUSH);
		int r = zmq_connect(pub , "inproc://notice");
		assert(r==0);
		queue_notice = pub;
	}
	zmq_msg_t dummy;
	zmq_msg_init(&dummy);
	zmq_send(queue_notice,&dummy,0);
	zmq_msg_close(&dummy);
}

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
// thread safe function
void 
skynet_harbor_send(const char *name, uint32_t destination, struct skynet_message * msg) {
	if (name == NULL) {		// 不知道服务名，只知道地址
		assert(destination!=0);
		int remote_id = destination >> HANDLE_REMOTE_SHIFT;
		assert(remote_id > 0 && remote_id <= REMOTE_MAX);
		--remote_id;
		struct remote * r = &Z->remote[remote_id];
		struct skynet_remote_message message;
		message.destination = destination;
		message.message = *msg;
		if (r->socket) {	// 已经与对应的 skynet 节点建立了连接
			skynet_remotemq_push(Z->queue, &message);
			send_notice();
		} else {			// 还没有与对应的 skynet 节点建立连接
			// QUESTION : 如何确定消息队列就已经建立了？
			skynet_remotemq_push(r->queue, &message);
		}
	} else {				// 明确发送给某个服务名对应的服务。
		_lock();		// 加锁，免得查找的时候有插入
		struct keyvalue * node = _hash_search(Z->map, name);
		if (node) {			// name 对应的服务信息之前已经知道了
			uint32_t dest = node->value;		// 目的地上的对应的服务的 handle id
			_unlock();
			if (dest == 0) {	// 表示 name 对应的服务是未知的。例如 _hash_insert(Z->map, name, 0, queue); 就会导致这种情况
				// push message to unknown name service queue
				skynet_mq_push(node->queue, msg);
			} else {
				if (!skynet_harbor_message_isremote(dest)) {
					// local message
					if (skynet_context_push(dest, msg)) {
						skynet_error(NULL, "Drop local message from %u to %s",msg->source, name);
					}
					return;
				}
				struct skynet_remote_message message;
				message.destination = dest;
				message.message = *msg;
				skynet_remotemq_push(Z->queue,&message);
				send_notice();
			}
		} else {			// name 对应的服务不知道
			// never seen name before
			struct message_queue * queue =  skynet_mq_create(0);
			skynet_mq_push(queue, msg);
			_hash_insert(Z->map, name, 0, queue);	// 在 _register_name 会添加这个服务名字对应的服务 id
			_unlock();
			// 0 for query
			skynet_harbor_register(name,0);
		}
	}
}

/*
 请求 注册/查询 某个服务名字
 name : 将要注册的全局服务名
 handle : 对应的服务 id 。设置为 0 表示查询 name
*/
// thread safe function
//queue a register message (destination = 0)
void 
skynet_harbor_register(const char *name, uint32_t handle) {
	struct skynet_remote_message msg;
	msg.destination = SKYNET_SYSTEM_NAME;
	msg.message.source = handle;
	msg.message.data = strdup(name);

	msg.message.sz = 0;
	skynet_remotemq_push(Z->queue,&msg);
	send_notice();
}

/*
 将全局服务名和对应的服务 id 键值对插入 Z->map ，并将之前堆积在此服务的消息队列中的消息处理
 这个全局服务名也可能是本 skynet 节点的
*/
// Always in main harbor thread
static void
_register_name(const char *name, uint32_t addr) {
	_lock();
	struct keyvalue * node = _hash_search(Z->map, name);
	if (node) {
		if (node->value) {	// 已经有了，则是更新
			node->value = addr;
			assert(node->queue == NULL);	// 既然之前就已经有了，必然创建了消息队列
		} else {			// 之前已经有消息发往这个服务名字对应的服务了，只是之前还不知道这个服务名对应的服务 id 是多少
			node->value = addr;
		}
	} else {
		_hash_insert(Z->map, name, addr, NULL);
	}

	if (addr == 0) {
		_unlock();
		return;
	}

	struct skynet_message msg;
	struct message_queue * queue = node ? node->queue : NULL;

	if (queue) {
		if (skynet_harbor_message_isremote(addr)) {		// 此服务属于别的 skynet 节点，则将之前堆积的消息放入 harbor 全局消息队列
			while (!skynet_mq_pop(queue, &msg)) {
				struct skynet_remote_message message;
				message.destination = addr;
				message.message = msg;
				skynet_remotemq_push(Z->queue, &message);
				send_notice();
			}
		} else {										// 此服务属于本地 skynet 节点，则将之前堆积的消息放入本地消息队列
			while (!skynet_mq_pop(queue, &msg)) {
				if (skynet_context_push(addr,&msg)) {
					skynet_error(NULL,"Drop local message from %u to %s",msg.source,name);
				}
			}
		}

		node->queue = NULL;
	}

	_unlock();

	if (queue) {
		skynet_mq_release(queue);
	}
}

/*
 将 harbor id 以及地址信息保存到本地，并建立联系
*/
// Always in main harbor thread

static void
_remote_harbor_update(int harbor_id, const char * addr) {
	struct remote * r = &Z->remote[harbor_id-1];
	void *socket = zmq_socket( Z->zmq_context, ZMQ_PUSH);
	int rc = zmq_connect(socket, addr);
	if (rc<0) {
		skynet_error(NULL, "Can't connect to %d %s",harbor_id,addr);
		zmq_close(socket);
		socket = NULL;
	}
	if (socket) {
		void *old_socket = r->socket;
		if (old_socket) {
			zmq_close(old_socket);
		}
		struct message_remote_queue * queue = r->queue;

		if (queue) {
			struct skynet_remote_message msg;
			while (!skynet_remotemq_pop(queue, &msg)) {
				skynet_remotemq_push(Z->queue, &msg);
			}
			skynet_remotemq_release(queue);
			r->queue = NULL;
		}

		r->socket = socket;
	}
}

static void
_report_zmq_error(int rc) {
	if (rc) {
		fprintf(stderr, "zmq error : %s\n",zmq_strerror(errno));
		exit(1);
	}
}

static int
_isdecimal(int c) {
	return c>='0' && c<='9';
}

/*
 如果 buf 形式为 %d=%s 则 *np = %d ，返回等号的位置
 如果 buf 形式为 %s=%d 则返回 %s 的长度
 如果 buf 形式为 %s 则返回 -1
*/
// Name-updating protocols:
//
// 1) harbor_id=harbor_address
// 2) context_name=context_handle
static int
_split_name(uint8_t *buf, int len, int *np) {
	uint8_t *sep;
	if (len > 0 && _isdecimal(buf[0])) {
		int i=0;
		int n=0;
		do {
			n = n*10 + (buf[i]-'0');
		} while(++i<len && _isdecimal(buf[i]));
		if (i < len && buf[i] == '=') {
			buf[i] = '\0';
			*np = n;
			return i;
		}
	} else if ((sep = memchr(buf, '=', len)) != NULL) {
		*sep = '\0';
		return (int)(sep-buf);
	}
	return -1;
}

/*
 接收来自另外的 master 的广播消息：
 1. harbor id 和地址更新
 2. 服务名和地址更新
*/
// Always in main harbor thread
static void
_name_update() {
	zmq_msg_t content;
	zmq_msg_init(&content);
	int rc = zmq_recv(Z->zmq_local,&content,0);
	_report_zmq_error(rc);
	int sz = zmq_msg_size(&content);
	uint8_t * buffer = zmq_msg_data(&content);

	int n = 0;
	int i = _split_name(buffer, sz, &n);
	if (i == -1) {
		char tmp[sz+1];
		memcpy(tmp,buffer,sz);
		tmp[sz] = '\0';
		skynet_error(NULL, "Invalid master update [%s]",tmp);
		zmq_msg_close(&content);
		return;
	}

	char tmp[sz-i];
	memcpy(tmp,buffer+i+1,sz-i-1);
	tmp[sz-i-1]='\0';

	if (n>0 && n <= REMOTE_MAX) {		// %d=%s ，例如 3=145.23.65.78 ，前面是 harbor id ，后面是 ip 地址
		_remote_harbor_update(n, tmp);
	} else {
		uint32_t source = strtoul(tmp,NULL,16);
		if (source == 0) {
			skynet_error(NULL, "Invalid master update [%s=%s]",(const char *)buffer,tmp);
		} else {		// %s=%d，例如 "DATACENTER"=45123 ，前面是全局服务别名，后面是 handle id
			_register_name((const char *)buffer, source);
		}
	}

	zmq_msg_close(&content);
}

/*
 向 master 查询某个 harbor id ，并与其建立连接
 形式为 %d 
*/
// Always in main harbor thread
static void
remote_query_harbor(int harbor_id) {
	char tmp[32];
	int sz = sprintf(tmp,"%d",harbor_id);
	zmq_msg_t request;
	zmq_msg_init_size(&request,sz);
	memcpy(zmq_msg_data(&request),tmp,sz);
	zmq_send(Z->zmq_master_request, &request, 0);
	zmq_msg_close(&request);
	zmq_msg_t reply;
	zmq_msg_init(&reply);
	int rc = zmq_recv(Z->zmq_master_request, &reply, 0);
	_report_zmq_error(rc);
	sz = zmq_msg_size(&reply);
	char tmp2[sz+1];
	memcpy(tmp2,zmq_msg_data(&reply),sz);
	tmp2[sz] = '\0';
	_remote_harbor_update(harbor_id, tmp2);
	zmq_msg_close(&reply);
}

/*
 向 master 注册全局服务名
 形式为 %s=%X 前者为服务名，后者为服务 id
*/
// Always in main harbor thread
static void
_remote_register_name(const char *name, uint32_t source) {
	char tmp[strlen(name) + 20];
	int sz = sprintf(tmp,"%s=%X",name,source);
	zmq_msg_t msg;
	zmq_msg_init_size(&msg,sz);
	memcpy(zmq_msg_data(&msg), tmp , sz);
	zmq_send(Z->zmq_master_request, &msg,0);
	zmq_msg_close(&msg);
	zmq_msg_init(&msg);
	int rc = zmq_recv(Z->zmq_master_request, &msg,0);
	_report_zmq_error(rc);
	zmq_msg_close(&msg);
}

/*
 向 master 查询某个服务名，并将服务名对应的 handle id 保存到本地
 形式为 %s 
*/
// Always in main harbor thread
static void
_remote_query_name(const char *name) {
	int sz = strlen(name);
	zmq_msg_t msg;
	zmq_msg_init_size(&msg,sz);
	memcpy(zmq_msg_data(&msg), name , sz);
	zmq_send(Z->zmq_master_request, &msg,0);
	zmq_msg_close(&msg);
	zmq_msg_init(&msg);
	int rc = zmq_recv(Z->zmq_master_request, &msg,0);
	_report_zmq_error(rc);
	sz = zmq_msg_size(&msg);
	char tmp[sz+1];
	memcpy(tmp, zmq_msg_data(&msg),sz);
	tmp[sz] = '\0';

	uint32_t addr = strtoul(tmp,NULL,16);
	_register_name(name,addr);

	zmq_msg_close(&msg);
}

// Always in main harbor thread
static void 
free_message(void *data, void *hint) { 
	free(data);
}

// 向 socket 发送 msg
static void
remote_socket_send(void * socket, struct skynet_remote_message *msg) {
	struct remote_header rh;
	rh.source = msg->message.source;
	rh.destination = msg->destination;
	zmq_msg_t part;
	zmq_msg_init_size(&part,sizeof(struct remote_header));
	uint8_t * buffer = zmq_msg_data(&part);
	remote_header_to_buffer(&rh,buffer);
	zmq_send(socket, &part, ZMQ_SNDMORE);
	zmq_msg_close(&part);

	zmq_msg_init_data(&part,msg->message.data,msg->message.sz,free_message,NULL);
	zmq_send(socket, &part, 0);
	zmq_msg_close(&part);
}

// Always in main harbor thread

// remote message has two part
// when part one is nil (size == 0), part two is name update
// Or part one is source:destination (8 bytes little endian), part two is a binary block for message
static void
_remote_recv() {
	zmq_msg_t header;
	zmq_msg_init(&header);
	int rc = zmq_recv(Z->zmq_local,&header,0);
	_report_zmq_error(rc);
	size_t s = zmq_msg_size(&header);
	if (s!=sizeof(struct remote_header)) {
		// s should be 0
		if (s>0) {
			char tmp[s+1];
			memcpy(tmp, zmq_msg_data(&header),s);
			tmp[s] = '\0';
			skynet_error(NULL,"Invalid master header [%s]",tmp);
		}
		_name_update();
		return;
	}
	uint8_t * buffer = zmq_msg_data(&header);
	struct remote_header rh;
	buffer_to_remote_header(buffer, &rh);
	zmq_close(&header);

	zmq_msg_t * data = malloc(sizeof(zmq_msg_t));
	zmq_msg_init(data);
	rc = zmq_recv(Z->zmq_local,data,0);
	_report_zmq_error(rc);

	struct skynet_message msg;
	msg.source = rh.source;
	msg.data = data;
	msg.sz = zmq_msg_size(data);

	// push remote message to local message queue
	if (skynet_context_push(rh.destination, &msg)) {
		zmq_msg_close(data);
		free(data);
		skynet_error(NULL, "Drop remote message from %u to %u",rh.source, rh.destination);
	}
}

/*
 处理 harbor 全局队列中的消息
*/
// Always in main harbor thread
static void
_remote_send() {
	struct skynet_remote_message msg;
	while (!skynet_remotemq_pop(Z->queue,&msg)) {
_goback:
		if (msg.destination == SKYNET_SYSTEM_NAME) {	// skynet_harbor_register
			// register name
			char * name = msg.message.data;

			if (msg.message.source) {		// 和 master 通信
				_remote_register_name(name, msg.message.source);	// 注册全局服务名
			} else {
				_remote_query_name(name);					// 查询某个名字，并注册到本地
			}

			free(name);
		} else {	// 和 harbor 通信
			int harbor_id = (msg.destination >> HANDLE_REMOTE_SHIFT);
			assert(harbor_id > 0);
			struct remote * r = &Z->remote[harbor_id-1];
			if (r->socket == NULL) {	// 对应的 skynet 节点并未与之建立连接
				/*
				 这里可能会奇怪为什么有的 r->socket == NULL 但是 r->queue != NULL
				 因为，当第一次对一个没有建立连接的 skynet 节点发送消息的时候，会建立消息队列，
				 所以当第二次其发送消息的时候消息队列已经存在了。
				*/
				if (r->queue == NULL) {
					r->queue = skynet_remotemq_create();
					skynet_remotemq_push(r->queue, &msg);
					remote_query_harbor(harbor_id);
				} else {
					skynet_remotemq_push(r->queue, &msg);		// QUESTION : 为什么还会进这里？第一次的时候已经与对应的 skynet 节点建立连接了
				}
			} else {
				remote_socket_send(r->socket, &msg);		// 给对应的 skynet 节点发送消息
			}
		}
	}
	__sync_lock_release(&Z->notice_event);
	// double check
	if (!skynet_remotemq_pop(Z->queue,&msg)) {
		__sync_lock_test_and_set(&Z->notice_event, 1);
		goto _goback;
	}
}

// Main harbor thread
void *
skynet_harbor_dispatch_thread(void *ud) {
	zmq_pollitem_t items[2];

	items[0].socket = Z->zmq_queue_notice;
	items[0].events = ZMQ_POLLIN;
	items[1].socket = Z->zmq_local;
	items[1].events = ZMQ_POLLIN;

	for (;;) {
		zmq_poll(items,2,-1);			// -1 表示超时时间是无限
		if (items[0].revents) {			// master 消息
			/*
			 查询某个服务名对应的地址，为发送消息做准备: skynet_harbor_send
			 更新某个全局服务: skynet_harbor_register
			*/
			zmq_msg_t msg;
			zmq_msg_init(&msg);
			int rc = zmq_recv(Z->zmq_queue_notice,&msg,0);
			_report_zmq_error(rc);
			zmq_msg_close(&msg);
			_remote_send();
		}
		if (items[1].revents) {			// harbor 消息
			_remote_recv();
		}
	}
}

/*
 向 master 注册 harbor id
*/
// Call only at init
static void
register_harbor(void *request, const char *local, int harbor) {
	// 测试 harbor id 是否被注册
	char tmp[1024];
	sprintf(tmp,"%d",harbor);
	size_t sz = strlen(tmp);

	zmq_msg_t req;
	zmq_msg_init_size (&req , sz);
	memcpy(zmq_msg_data(&req),tmp,sz);
	zmq_send (request, &req, 0);
	zmq_msg_close (&req);

	// 测试结果
	zmq_msg_t reply;
	zmq_msg_init (&reply);
	int rc = zmq_recv(request, &reply, 0);
	_report_zmq_error(rc);

	sz = zmq_msg_size (&reply);
	if (sz > 0) {
		memcpy(tmp,zmq_msg_data(&reply),sz);
		tmp[sz] = '\0';
		fprintf(stderr, "Harbor %d is already registered by %s\n", harbor, tmp);
		exit(1);
	}
	zmq_msg_close (&reply);

	// 正式请求注册 harbor id 
	// 还是可能失败的，因为在测试结果返回之后，正式请求注册之前，是不可控。
	sprintf(tmp,"%d=%s",harbor,local);
	sz = strlen(tmp);
	zmq_msg_init_size (&req , sz);
	memcpy(zmq_msg_data(&req),tmp,sz);
	zmq_send (request, &req, 0);
	zmq_msg_close (&req);

	// 注册结果
	zmq_msg_init (&reply);
	rc = zmq_recv (request, &reply, 0);
	_report_zmq_error(rc);

	zmq_msg_close (&reply);
}

/*
 master : master 的地址
 local : 此 skynet 的地址
 harbor : harbor id
*/
void 
skynet_harbor_init(const char * master, const char *local, int harbor) {
	if (harbor <=0 || harbor>255 || strlen(local) > 512) {
		fprintf(stderr,"Invalid harbor id\n");
		exit(1);
	}
	void *context = zmq_init (1);
	void *request = zmq_socket (context, ZMQ_REQ);	// 所以是 skynet 节点先向 master 服务器发送消息，即请求注册 harbor id 消息
	int r = zmq_connect(request, master);
	if (r<0) {
		fprintf(stderr, "Can't connect to master: %s\n",master);
		exit(1);
	}
	void *harbor_socket = zmq_socket(context, ZMQ_PULL);
	r = zmq_bind(harbor_socket, local);
	if (r<0) {
		fprintf(stderr, "Can't bind to local : %s\n",local);
		exit(1);
	}
	register_harbor(request,local, harbor);
	printf("Start harbor on : %s\n",local);

	struct harbor * h = malloc(sizeof(*h));
	memset(h, 0, sizeof(*h));
	h->zmq_context = context;
	h->zmq_master_request = request;
	h->zmq_local = harbor_socket;
	h->map = _hash_new();
	h->harbor = harbor;
	h->queue = skynet_remotemq_create();
	h->zmq_queue_notice = zmq_socket(context, ZMQ_PULL);
	r = zmq_bind(h->zmq_queue_notice, "inproc://notice");	// 所以很多线程可以发送消息给它接收
	assert(r==0);

	Z = h;
}

// thread safe api
void * 
skynet_harbor_message_open(struct skynet_message * message) {
	return zmq_msg_data(message->data);
}

void 
skynet_harbor_message_close(struct skynet_message * message) {
	zmq_msg_close(message->data);
}

int 
skynet_harbor_message_isremote(uint32_t handle) {
	int harbor_id = handle >> HANDLE_REMOTE_SHIFT;
	return !(harbor_id == 0 || harbor_id == Z->harbor);
}