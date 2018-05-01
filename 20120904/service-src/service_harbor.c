#include "skynet.h"
#include "skynet_harbor.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <errno.h>

#define HASH_SIZE 4096
#define DEFAULT_QUEUE_SIZE 1024

struct msg {
	char * buffer;
	size_t size;
};

struct msg_queue {
	int size;
	int head;
	int tail;
	struct msg * data;
};

struct keyvalue {
	struct keyvalue * next;
	char key[GLOBALNAME_LENGTH];
	uint32_t hash;
	uint32_t value;
	struct msg_queue * queue;
};

struct hashmap {
	struct keyvalue *node[HASH_SIZE];
};

/*
	message type (8bits) is in destination high 8bits
	harbor id (8bits) is also in that place , but  remote message doesn't need harbor id.
 */
struct remote_message_header {
	uint32_t source;
	uint32_t destination;	// (destination & HANDLE_MASK) | ((uint32_t)type << HANDLE_REMOTE_SHIFT)
	uint32_t session;
};

struct harbor {
	int id;
	struct hashmap * map;
	int master_fd;
	char * master_addr;
	int remote_fd[REMOTE_MAX];
	char * remote_addr[REMOTE_MAX];
};

// hash table
/*
 往消息队列中增加一条消息，消息由 buffer + remote_message_header 组成
 queue: 目的消息队列
 buffer: 消息的内容
 sz: buffer 的大小
 header: 消息头
*/
static void
_push_queue(struct msg_queue * queue, const void * buffer, size_t sz, struct remote_message_header * header) {
	struct msg * slot = &queue->data[queue->tail];
	queue->tail = (queue->tail + 1) % queue->size;
	if (queue->tail == queue->head) {
		struct msg * new_buffer = malloc(queue->size * 2 * sizeof(struct msg));
		int i;
		for (i=0;i<queue->size;i++) {
			new_buffer[i] = queue->data[(i+queue->head) % queue->size];
		}
		free(queue->data);
		queue->data = new_buffer;
		queue->head = 0;
		queue->tail = queue->size;
		queue->size *= 2;
		slot = &queue->data[queue->tail];
	}
	slot->buffer = malloc(sz + sizeof(*header));
	memcpy(slot->buffer, buffer, sz);
	memcpy(slot->buffer + sz, header, sizeof(*header));
	slot->size = sz + sizeof(*header);
}

/*
 从消息队列中取出一条消息
*/
static struct msg *
_pop_queue(struct msg_queue * queue) {
	if (queue->head == queue->tail) {
		return NULL;
	}
	struct msg * slot = &queue->data[queue->head];
	queue->head = (queue->head + 1) % queue->size;
	return slot;
}

/*
 新建一个消息队列
*/
static struct msg_queue *
_new_queue() {
	struct msg_queue * queue = malloc(sizeof(*queue));
	queue->size = DEFAULT_QUEUE_SIZE;
	queue->head = 0;
	queue->tail = 0;
	queue->data = malloc(DEFAULT_QUEUE_SIZE * sizeof(struct msg));

	return queue;
}

/*
 销毁一个消息队列
*/
static void
_release_queue(struct msg_queue *queue) {
	if (queue == NULL)
		return;
	struct msg * m = _pop_queue(queue);
	while (m) {
		free(m->buffer);
		m = _pop_queue(queue);
	}
	free(queue->data);
	free(queue);
}

/*
 查找服务名对应的服务信息
*/
static struct keyvalue *
_hash_search(struct hashmap * hash, const char name[GLOBALNAME_LENGTH]) {
	uint32_t *ptr = (uint32_t*) name;
	uint32_t h = ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3];
	struct keyvalue * node = hash->node[h % HASH_SIZE];
	while (node) {
		if (node->hash == h && strncmp(node->key, name, GLOBALNAME_LENGTH) == 0) {
			return node;
		}
		node = node->next;
	}
	return NULL;
}

/*

// Don't support erase name yet

static struct void
_hash_erase(struct hashmap * hash, char name[GLOBALNAME_LENGTH) {
	uint32_t *ptr = name;
	uint32_t h = ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3];
	struct keyvalue ** ptr = &hash->node[h % HASH_SIZE];
	while (*ptr) {
		struct keyvalue * node = *ptr;
		if (node->hash == h && strncmp(node->key, name, GLOBALNAME_LENGTH) == 0) {
			_release_queue(node->queue);
			*ptr->next = node->next;
			free(node);
			return;
		}
		*ptr = &(node->next);
	}
}
*/

/*
 新建一个服务别名，但是对应的服务 id 还是 0 ，并且消息队列为空。
*/
static struct keyvalue *
_hash_insert(struct hashmap * hash, const char name[GLOBALNAME_LENGTH]) {
	uint32_t *ptr = (uint32_t *)name;
	uint32_t h = ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3];
	struct keyvalue ** pkv = &hash->node[h % HASH_SIZE];
	struct keyvalue * node = malloc(sizeof(*node));
	memcpy(node->key, name, GLOBALNAME_LENGTH);
	node->next = *pkv;
	node->queue = NULL;
	node->hash = h;
	node->value = 0;
	*pkv = node;

	return node;
}

static struct hashmap * 
_hash_new() {
	struct hashmap * h = malloc(sizeof(struct hashmap));
	memset(h,0,sizeof(*h));
	return h;
}

static void
_hash_delete(struct hashmap *hash) {
	int i;
	for (i=0;i<HASH_SIZE;i++) {
		struct keyvalue ** ptr = &hash->node[i];
		while (*ptr) {
			struct keyvalue * node = *ptr;
			ptr = &node->next;
			_release_queue(node->queue);
			free(node);
		}
	}
	free(hash);
}

///////////////

/*
 新建 harbor ，并设置默认初始值
*/
struct harbor *
harbor_create(void) {
	struct harbor * h = malloc(sizeof(*h));
	h->id = 0;
	h->master_fd = -1;
	h->master_addr = NULL;
	int i;
	for (i=0;i<REMOTE_MAX;i++) {
		h->remote_fd[i] = -1;
		h->remote_addr[i] = NULL;
	}
	h->map = _hash_new();
	return h;
}

/*
 销毁 harbor
*/
void
harbor_release(struct harbor *h) {
	if (h->master_fd >= 0) {
		close(h->master_fd);
	}
	free(h->master_addr);
	int i;
	for (i=0;i<REMOTE_MAX;i++) {
		if (h->remote_fd[i] >= 0) {
			close(h->remote_fd[i]);
			free(h->remote_addr[i]);
		}
	}
	_hash_delete(h->map);
	free(h);
}

/*
 ctx 对应的服务与 ipaddress 建立连接，返回 socket 
 ipaddress: ip:port
*/
static int
_connect_to(struct skynet_context *ctx, const char *ipaddress) {
	int fd = socket(AF_INET,SOCK_STREAM,0);
	struct sockaddr_in my_addr;
	char * port = strchr(ipaddress,':');
	if (port==NULL) {
		return -1;
	}
	int sz = port - ipaddress;
	char tmp[sz + 1];
	memcpy(tmp,ipaddress,sz);
	tmp[sz] = '\0';

	my_addr.sin_addr.s_addr=inet_addr(tmp);
	my_addr.sin_family=AF_INET;
	my_addr.sin_port=htons(strtol(port+1,NULL,10));

	int r = connect(fd,(struct sockaddr *)&my_addr,sizeof(struct sockaddr_in));

	if (r == -1) {
		close(fd);
		skynet_error(ctx, "Connect to %s error", ipaddress);
		return -1;
	}

	return fd;
}

/*
 字节序转换，本地转网络
*/
static inline void
_header_to_message(const struct remote_message_header * header, uint32_t * message) {
	message[0] = htonl(header->source);
	message[1] = htonl(header->destination);
	message[2] = htonl(header->session);
}

/*
 字节序转换，网络转本地
*/
static inline void
_message_to_header(const uint32_t *message, struct remote_message_header *header) {
	header->source = ntohl(message[0]);
	header->destination = ntohl(message[1]);
	header->session = ntohl(message[2]);
}

/*
 给 fd 发送数据，消息的前两个字节是数据大小，后面才是数据内容。
 成功返回 0 ，失败返回 1 。
*/
static int
_send_package(int fd, const void * buffer, size_t sz) {
	/*
	 通过writev函数可以将分散保存在多个buff的数据一并进行发送，通过readv可以由多个 buff 分别
	 接受数据，适当的使用这两个函数可以减少I/O函数的调用次数。这些操作被称为分散读( scatter read )和集合写( gather write )。
	 头文件: sys/uio.h
	 函数原型: 
	 ssize_t readv(int filedes, const struct iovec *iov, int iovcnt);
	 ssize_t writev(int filedes, const struct iovec *iov, int iovcnt);
	 函数的返回值是读取的字节数或是写入的字节数。如果有错误发生，就会返回-1，而errno存有错误代码。
	 返回错误码 EINTR 来表明被一个信号所中断，返回错误码 EAGAIN 来表明当前缓存暂时不可读或写，需要再次尝试。
	*/
	uint16_t header = htons(sz);
	struct iovec part[2];
	part[0].iov_base = &header;
	part[0].iov_len = 2;
	part[1].iov_base = (void*)buffer;
	part[1].iov_len = sz;

	for (;;) {
		int err = writev(fd, part, 2);
		if (err < 0) {
			switch (errno) {
			case EAGAIN:	// 表明 buf 不可写
			case EINTR:
				continue;
			}
		}
		if (err != sz+2) {
			return 1;
		}
		return 0;
	}
}

/*
 给 fd 发送数据，消息的前两个字节是数据大小，后面跟着数据内容，最后还有 remote_message_header 。
 成功返回 0 ，失败返回 1 。
*/
static int
_send_remote(int fd, const char * buffer, size_t sz, struct remote_message_header * cookie) {
	uint16_t sz_header = htons(sz+sizeof(*cookie));
	struct iovec part[3];

	part[0].iov_base = &sz_header;
	part[0].iov_len = 2;

	part[1].iov_base = (char *)buffer;
	part[1].iov_len = sz;

	uint32_t header[3];
	_header_to_message(cookie, header);

	part[2].iov_base = header;
	part[2].iov_len = sizeof(header);
	for (;;) {
		int err = writev(fd, part, 3);
		if (err < 0) {
			switch (errno) {
			case EAGAIN:
			case EINTR:
				continue;
			}
		}
		if (err != sz+sizeof(*cookie)+2) {
			return 1;
		}
		return 0;
	}
}

/*
 更新 harbor id 及其地址。
 h: 本地 harbor
 harbor_id: 目的 harbor id
 ipaddr: 目的 harbor 的 ip 和 port
*/
static void
_update_remote_address(struct skynet_context * context, struct harbor *h, int harbor_id, const char * ipaddr) {
	if (harbor_id == h->id) {
		return;
	}
	assert(harbor_id > 0  && harbor_id< REMOTE_MAX);
	if (h->remote_fd[harbor_id] >=0) {
		close(h->remote_fd[harbor_id]);
		free(h->remote_addr[harbor_id]);
		h->remote_addr[harbor_id] = NULL;
	}
	h->remote_fd[harbor_id] = _connect_to(context, ipaddr);
	if (h->remote_fd[harbor_id] >= 0) {
		free(h->remote_addr[harbor_id]);
		h->remote_addr[harbor_id] = strdup(ipaddr);
	}
}

/*
 将消息队列 queue 中的消息发给 handle 对应的服务。
 name 和 handle 是 handle 对应的服务的别名和 handle id
*/
static void
_dispatch_queue(struct harbor *h, struct skynet_context * context, struct msg_queue * queue, uint32_t handle,  const char name[GLOBALNAME_LENGTH] ) {
	int harbor_id = handle >> HANDLE_REMOTE_SHIFT;
	assert(harbor_id != 0);
	int fd = h->remote_fd[harbor_id];
	if (fd < 0) {
		char tmp [GLOBALNAME_LENGTH+1];
		memcpy(tmp, name , GLOBALNAME_LENGTH);
		tmp[GLOBALNAME_LENGTH] = '\0';
		skynet_error(context, "Drop message to %s (in harbor %d)",tmp,harbor_id);
		return;
	}
	struct msg * m = _pop_queue(queue);
	while (m) {
		struct remote_message_header * cookie = (struct remote_message_header *)(m->buffer + m->size - sizeof(*cookie));
		cookie->destination |= (handle & HANDLE_MASK);	// (destination & HANDLE_MASK) | ((uint32_t)type << HANDLE_REMOTE_SHIFT)
		_header_to_message(cookie, (uint32_t *)cookie);
		int err = _send_package(fd, m->buffer, m->size);
		if (err) {
			close(fd);
			h->remote_fd[harbor_id] = _connect_to(context, h->remote_addr[harbor_id]);
			if (h->remote_fd[harbor_id] < 0) {
				skynet_error(context, "Reconnect to harbor %d %s failed",harbor_id, h->remote_addr[harbor_id]);
				return;
			}
		}
		m = _pop_queue(queue);
	}
}

/*
 更新服务名和 id ，并处理(如果之前有发往此服务的消息 _remote_send_name )堆积发往此服务的消息。
*/
static void
_update_remote_name(struct harbor *h, struct skynet_context * context, const char name[GLOBALNAME_LENGTH], uint32_t handle) {
	struct keyvalue * node = _hash_search(h->map, name);
	if (node == NULL) {
		node = _hash_insert(h->map, name);
	}
	node->value = handle;
	if (node->queue) {
		_dispatch_queue(h, context, node->queue, handle, name);
		_release_queue(node->queue);
		node->queue = NULL;
	}
}

/*
 给 master 服务发消息，内容是 handle + name
 i: name 的实际大小
*/
static void
_request_master(struct harbor *h, struct skynet_context * context, const char name[GLOBALNAME_LENGTH], size_t i, uint32_t handle) {
	char buffer[4+i];
	handle = htonl(handle);
	memcpy(buffer, &handle, 4);
	memcpy(buffer+4,name,i);

	int err = _send_package(h->master_fd, buffer, 4+i);
	if (err) {
		close(h->master_fd);
		h->master_fd = _connect_to(context, h->master_addr);
		if (h->master_fd < 0) {
			skynet_error(context, "Reconnect to master server %s failed", h->master_addr);
			return;
		}
		_send_package(h->master_fd, buffer, 4+i); 
	}
}

/*
	update global name to master

	2 bytes (size)
	4 bytes (handle) (handle == 0 for request)
	n bytes string (name)
 */

static int
_remote_send_handle(struct harbor *h, struct skynet_context * context, uint32_t source, uint32_t destination, int type, int session, const char * msg, size_t sz) {
	int harbor_id = destination >> HANDLE_REMOTE_SHIFT;
	assert(harbor_id != 0);
	if (harbor_id == h->id) {
		// local message
		skynet_send(context, source, destination , type | PTYPE_TAG_DONTCOPY, session, (void *)msg, sz);
		return 1;
	}

	int fd = h->remote_fd[harbor_id];
	if (fd >= 0) {
		struct remote_message_header cookie;
		cookie.source = source;
		cookie.destination = (destination & HANDLE_MASK) | ((uint32_t)type << HANDLE_REMOTE_SHIFT);
		cookie.session = (uint32_t)session;
		int err = _send_remote(fd, msg,sz,&cookie);
		if (err) {
			close(fd);
			h->remote_fd[harbor_id] = _connect_to(context, h->remote_addr[harbor_id]);
			if (h->remote_fd[harbor_id] < 0) {
				skynet_error(context, "Reconnect to harbor %d : %s failed", harbor_id, h->remote_addr[harbor_id]);
				return 0;
			}
		}
	} else {
		_request_master(h, context, NULL, 0, harbor_id);
		skynet_error(context, "Drop message to harbor %d from %x to %x (session = %d, msgsz = %d)",harbor_id, source, destination,session,(int)sz);
	}
	return 0;
}

static void
_remote_register_name(struct harbor *h, struct skynet_context * context, const char name[GLOBALNAME_LENGTH], uint32_t handle) {
	int i;
	for (i=0;i<GLOBALNAME_LENGTH;i++) {
		if (name[i] == '\0')
			break;
	}
	if (handle != 0) {
		_update_remote_name(h, context, name, handle);
	}
	_request_master(h,context,name,i,handle);
}

static int
_remote_send_name(struct harbor *h, struct skynet_context * context, uint32_t source, const char name[GLOBALNAME_LENGTH], int type, int session, const char * msg, size_t sz) {
	struct keyvalue * node = _hash_search(h->map, name);
	if (node == NULL) {
		node = _hash_insert(h->map, name);
	}
	if (node->value == 0) {
		if (node->queue == NULL) {
			node->queue = _new_queue();
		}
		struct remote_message_header header;
		header.source = source;
		header.destination = type << HANDLE_REMOTE_SHIFT;
		header.session = (uint32_t)session;
		_push_queue(node->queue, msg, sz, &header);
		// 0 for request
		_remote_register_name(h, context, name, 0);
		return 1;
	} else {
		return _remote_send_handle(h, context, source, node->value, type, session, msg, sz);
	}
}

static void
_report_local_address(struct harbor *h, struct skynet_context * context, const char * local_address, int harbor_id) {
	size_t sz = strlen(local_address);
	_request_master(h, context, local_address, sz, harbor_id);
}

static int
_mainloop(struct skynet_context * context, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct harbor * h = ud;
	switch (type) {
	case PTYPE_HARBOR: {
		// remote message in
		const char * cookie = msg;
		cookie += sz - 12;
		struct remote_message_header header;
		_message_to_header((const uint32_t *)cookie, &header);
		if (header.source == 0) {
			if (header.destination < REMOTE_MAX) {
				// 1 byte harbor id (0~255)
				// update remote harbor address
				char ip [sz - 11];
				memcpy(ip, msg, sz-12);
				ip[sz-11] = '\0';
				_update_remote_address(context, h, header.destination, ip);
			} else {
				// update global name
				if (sz - 12 > GLOBALNAME_LENGTH) {
					char name[sz-11];
					memcpy(name, msg, sz-12);
					name[sz-11] = '\0';
					skynet_error(context, "Global name is too long %s", name);
				}
				_update_remote_name(h, context, msg, header.destination);
			}
		} else {
			uint32_t destination = header.destination;
			int type = (destination >> HANDLE_REMOTE_SHIFT) | PTYPE_TAG_DONTCOPY;
			destination = (destination & HANDLE_MASK) | ((uint32_t)h->id << HANDLE_REMOTE_SHIFT);
			skynet_send(context, header.source, destination, type, (int)header.session, (void *)msg, sz-12);
			return 1;
		}
		return 0;
	}
	case PTYPE_SYSTEM: {
		// register name message
		const struct remote_message *rmsg = msg;
		assert (sz == sizeof(rmsg->destination));
		_remote_register_name(h, context, rmsg->destination.name, rmsg->destination.handle);
		return 0;
	}
	default: {
		// remote message out
		const struct remote_message *rmsg = msg;
		if (rmsg->destination.handle == 0) {
			if (_remote_send_name(h, context, source , rmsg->destination.name, type, session, rmsg->message, rmsg->sz)) {
				return 0;
			}
		} else {
			if (_remote_send_handle(h, context, source , rmsg->destination.handle, type, session, rmsg->message, rmsg->sz)) {
				return 0;
			}
		}
		free((void *)rmsg->message);
		return 0;
	}
	}
}

int
harbor_init(struct harbor *h, struct skynet_context *ctx, const char * args) {
	int sz = strlen(args)+1;
	char master_addr[sz];
	char local_addr[sz];
	int harbor_id = 0;
	sscanf(args,"%s %s %d",master_addr, local_addr, &harbor_id);
	int master_fd = _connect_to(ctx, master_addr);
	if (master_fd < 0) {
		skynet_error(ctx, "Harbor : Connect to master %s faild",master_addr);
		return 1;
	}
	printf("Connect to master %s\n",master_addr);

	h->master_addr = strdup(master_addr);
	h->master_fd = master_fd;

	char tmp[128];
	sprintf(tmp,"gate ! %s %d %d 0",local_addr, PTYPE_HARBOR, REMOTE_MAX);
	const char * gate_addr = skynet_command(ctx, "LAUNCH", tmp);
	if (gate_addr == NULL) {
		skynet_error(ctx, "Harbor : launch gate failed");
		return 1;
	}
	uint32_t gate = strtoul(gate_addr+1 , NULL, 16);
	if (gate == 0) {
		skynet_error(ctx, "Harbor : launch gate invalid %s", gate_addr);
		return 1;
	}
	const char * self_addr = skynet_command(ctx, "REG", NULL);
	int n = sprintf(tmp,"broker %s",self_addr);
	skynet_send(ctx, 0, gate, PTYPE_TEXT, 0, tmp, n);
	skynet_send(ctx, 0, gate, PTYPE_TEXT, 0, "start", 5);

	h->id = harbor_id;
	skynet_callback(ctx, h, _mainloop);

	_report_local_address(h, ctx, local_addr, harbor_id);

	return 0;
}
