#include <zmq.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define HASH_SIZE 4096
#define MAX_SLAVE 255

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

/*
 key - value 是键值对，通过 key 对应 value ，这是对外
 对内是通过 hash 来高效查找对应的 value
*/
struct keyvalue {
	struct keyvalue * next;
	uint32_t hash;
	char * key;
	size_t value_size;
	char * value;
};

/*
 skynet 的 key 其实是一个主机的一个编号，0 - 254 ， value 其实是主机的地址。
 每个 key 就是 slave 数组的下标
*/
struct hashmap {
	struct keyvalue *node[HASH_SIZE];		// 键值对
	void * zmq;
	/*
	 客户端 socket ，对于 master 来说，也就是各个 skynet 的 socket
	 下标就是 harbor id
	*/
	void * slave[MAX_SLAVE];
};

static struct hashmap *
_hash_new(void *zmq) {
	struct hashmap * hash = malloc(sizeof(*hash));
	memset(hash, 0, sizeof(*hash));
	hash->zmq = zmq;
	return hash;
}

/*
 存在不同的 key 计算出相同的 hash
*/
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

/*
 插入键值对
 对于 next 指针的解释：
 	最开始的时候 hashmap 中的内容都是 0 ，所以 key1 和 value1 插入的时候， node->next 是指向 NULL
 	运行到函数的最后一句才会将 hash->node[h & (HASH_SIZE-1)] 赋值。
 	当插入 key2 和 value2 并且 key2 和 key1 计算出来的 hash 值相同，这时候新的 node->next 赋值
 	指向之前的 key1 对应的 node ，然后在函数的最后一句更新 hash->node[h & (HASH_SIZE-1)] 。
*/
static void
_hash_insert(struct hashmap * hash, const char * key, const char *value) {
	uint32_t h = calc_hash(key);
	struct keyvalue * node = malloc(sizeof(*node));
	node->next = hash->node[h & (HASH_SIZE-1)];
	node->hash = h;
	node->key = strdup(key);
	node->value_size = strlen(value);
	node->value = malloc(node->value_size+1);
	memcpy(node->value, value, node->value_size+1);

	hash->node[h & (HASH_SIZE-1)] = node;
}

static void
_hash_delete(struct hashmap * hash, const char * key) {
	uint32_t h = calc_hash(key);
	struct keyvalue ** ptr = &hash->node[h & (HASH_SIZE-1)];
	while(*ptr) {
		struct keyvalue *n = *ptr;
		if (n->hash == h && strcmp(n->key, key) == 0) {
			*ptr = n->next;
			free(n->key);
			free(n->value);
			free(n);
			return ;
		}
		ptr = &(n->next);
	}
}

static int
_hash_bind(struct hashmap * hash, int slave, const char *addr) {
	void *pub = NULL;
	if (addr) {
		pub = zmq_socket(hash->zmq, ZMQ_PUSH);
		int r = zmq_connect(pub, addr);
		if (r<0) {
			fprintf(stderr,"Can't connect to [%d] %s\n",slave,addr);
			return -1;
		}
		printf("Connect to [%d] %s\n",slave,addr);
	}
	void * old_pub = hash->slave[slave-1];
	hash->slave[slave-1] = pub;
	if (old_pub) {
		zmq_close(old_pub);
	}

	return 0;
}

// 向所有的 skynet 节点广播 msg
static void
broadcast(struct hashmap * hash, zmq_msg_t * msg) {
	int i;
	for (i=0;i<MAX_SLAVE;i++) {
		void * pub = hash->slave[i];
		if (pub) {
			zmq_msg_t part;
			zmq_msg_init(&part);
			int rc = zmq_send(pub, &part, ZMQ_SNDMORE);
			if (rc != 0) {
				fprintf(stderr,"Can't publish to %d : %s",i+1,zmq_strerror(errno));
			}
			zmq_msg_close(&part);
			zmq_msg_init(&part);
			zmq_msg_copy(&part,msg);
			rc = zmq_send(pub, &part, 0);
			if (rc != 0) {
				fprintf(stderr,"Can't publish to %d : %s",i+1,zmq_strerror(errno));
			}
			zmq_msg_close(&part);
		}
	}
}

static void
replace(void * responder, struct hashmap * map, const char *key, const char *value) {
//	printf("Replace %s %s\n",key,value);
	struct keyvalue * node = _hash_search(map, key);
	zmq_msg_t reply;
	if (node == NULL) {
		_hash_insert(map, key, value);
		zmq_msg_init_size (&reply, 0);
	} else {
		zmq_msg_init_size (&reply, node->value_size);
		memcpy (zmq_msg_data (&reply), node->value, node->value_size);

		free(node->value);
		node->value_size = strlen(value);
		node->value = malloc(node->value_size + 1);
		memcpy(node->value, value, node->value_size +1);
	}
	zmq_send (responder, &reply, 0);
	zmq_msg_close (&reply);
}

static void
erase(void * responder, struct hashmap *map, const char *key) {
//	printf("Erase %s\n",key);
	struct keyvalue * node = _hash_search(map, key);
	zmq_msg_t reply;
	if (node == NULL) {
		zmq_msg_init_size (&reply, 0);
	} else {
		zmq_msg_init_size (&reply, node->value_size);
		memcpy (zmq_msg_data (&reply), node->value, node->value_size);
		_hash_delete(map, key);
	}
	zmq_send (responder, &reply, 0);
	zmq_msg_close (&reply);
}

/*
 查询某个 key 是否存在
 不存在则给 responder 发送消息，消息大小是 0
 存在则给 responder 发送消息，消息内容是 key 对应的 value
*/
static void
query(void * responder, struct hashmap *map, const char * key) {
	struct keyvalue * node = _hash_search(map, key);
	zmq_msg_t reply;
	if (node == NULL) {
		zmq_msg_init_size (&reply, 0);
	} else {
		zmq_msg_init_size (&reply, node->value_size);
		memcpy (zmq_msg_data (&reply), node->value, node->value_size);
	}
	zmq_send (responder, &reply, 0);
	zmq_msg_close (&reply);
}

/*
 "%d=%s"	->	harbor=address			register_harbor
 "%s=%X"	->	name=handle id 			_remote_register_name
*/
static void
update(void * responder, struct hashmap *map, zmq_msg_t * msg) {
	size_t sz = zmq_msg_size (msg);
	const char * command = zmq_msg_data (msg);
	int i;
	for (i=0;i<sz;i++) {
		if (command[i] == '=') {
			char key[i+1];
			memcpy(key, command, i);
			key[i] = '\0';
			int slave = strtol(key, NULL, 10);
			if (sz-i == 1) {	// 清除 key 对应的 value
				/*
				 传过来的命令如果是 key = ，则表示擦除 key 对应的 value
				*/
				if (slave > 0 && slave <= MAX_SLAVE) {
					_hash_bind(map, slave, NULL);
				}
				erase(responder, map,key);
				broadcast(map, msg);
			} else {			// 更新 key 对应的 value
				char value[sz-i];
				memcpy(value, command+i+1, sz-i-1);
				value[sz-i-1] = '\0';
				if (slave > 0 && slave <= MAX_SLAVE) {
					_hash_bind(map, slave, value);
				}

				replace(responder, map, key,value);
				broadcast(map, msg);
			}
			return;
		}
	}

	// 查询
	char key[sz+1];
	memcpy(key, command, sz);
	key[sz] = '\0';
	query(responder, map, key);
}

int 
main (int argc, char * argv[]) {
	const char * default_port = "tcp://127.0.0.1:2012";
	if (argc > 1) {
		default_port = argv[2];
	}

	void *context = zmq_init (1);
	void *responder = zmq_socket (context, ZMQ_REP);		// 设置服务端为 请求-应答模式 

	int r = zmq_bind(responder, default_port);
	if (r < 0) {
		fprintf(stderr, "Can't bind to %s\n",default_port);
		return 1;
	}
	printf("Start master on %s\n",default_port);

	struct hashmap *map = _hash_new(context);

	/*
	- 你需要创建和传递 zmq_msg_t 对象，而不是一组数据块；
	- 读取消息时，先用 zmq_msg_init() 初始化一个空消息，再将其传递
		给 zmq_recv() 函数；
	- 获取消息内容时需使用 zmq_msg_data() 函数；若想知道消息的长
		度，可以使用 zmq_msg_size() 函数；
	- 写入消息时，先用 zmq_msg_init_size() 来创建消息（同时也已初
		始化了一块内存区域），然后用 memcpy() 函数将信息拷贝到该
		对象中，最后传给 zmq_send() 函数；
	- 释放消息（并不是销毁）时，使用 zmq_msg_close() 函数，它会将
		对消息对象的引用删除，最终由 ZMQ 将消息销毁；
	*/
	for (;;) {
		zmq_msg_t request;
		zmq_msg_init (&request);
		zmq_recv (responder, &request, 0);
		update(responder, map, &request);
		zmq_msg_close (&request);
	}

	zmq_close (responder);
	zmq_term (context);
	return 0;
}
