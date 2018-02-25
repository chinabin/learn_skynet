#include "skynet_server.h"
#include "skynet_module.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_blackhole.h"
#include "skynet_timer.h"
#include "skynet.h"

#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define BLACKHOLE "blackhole"
#define DEFAULT_MESSAGE_QUEUE 16

struct skynet_context {
	void * instance;				// 实例指针，通过调用创建实例函数返回
	struct skynet_module * mod;
	int handle;						// 服务编号
	/*
	 calling 是一种标识，为 1 表示触发，为 0 表示没有触发
	 当前的作用是，因为 init 接口中可能会给自己发消息，而消息的响应是需要调用回调函数，而回调函数的设置是在 init 接口中
	 所以在这里设置一个标识，表示是否准备好。后面的消息分发代码中会查询这个设置，如果为 1 则将消息推送到消息队列，否则直接分发。
	*/
	int calling;
	int ref;						// 引用计数
	char handle_name[10];			// handle 的十六进制字符串形式
	char result[32];				// 不同的命令会设置不同的返回值
	void * cb_ud;					// 回调函数的第二个参数
	skynet_cb cb;					// 回调函数指针，定义在 skynet.h : typedef void (*skynet_cb)(struct skynet_context * context, void *ud, const char * uid , const void * msg, size_t sz_session);
	struct message_queue *queue;
};

// id 转为16进制字符串
static void
_id_to_hex(char * str, int id) {
	int i;
	static char hex[16] = { '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F' };
	for (i=0;i<8;i++) {
		str[i] = hex[(id >> ((7-i) * 4))&0xf];
	}
	str[8] = '\0';
}

/*
 创建新的 ctx 并返回
 name: 模块名
 parm: 模块的 init 接口使用的参数之一
*/
struct skynet_context * 
skynet_context_new(const char * name, char *parm) {
	struct skynet_module * mod = skynet_module_query(name);

	if (mod == NULL)
		return NULL;

	void *inst = skynet_module_instance_create(mod);
	if (inst == NULL)
		return NULL;
	struct skynet_context * ctx = malloc(sizeof(*ctx));
	ctx->mod = mod;
	ctx->instance = inst;
	/*
	 QUESTION: 为什么不直接设置为 1 ，而是先设置为 2 然后调用 skynet_context_release 
	 猜想是为了代码规范与行为统一：首先，使用 ctx 应该增加引用计数。其次，使用完 ctx 要调用 skynet_context_release 释放。
	 首先，创建 ctx 并给其成员赋值，属于引用，所以引用计数加 1 ，当前引用计数值为 1
	 因为使用完成必须调用 skynet_context_release 释放，但是不能刚刚创建好就直接销毁，所以设置引用计数为 2
	*/
	ctx->ref = 2;
	ctx->cb = NULL;
	ctx->cb_ud = NULL;
	char * uid = ctx->handle_name;
	uid[0] = ':';
	_id_to_hex(uid+1, ctx->handle);	// 应该放在后面一句 ctx->handle 赋值语句后面，后面的版本修复了

	ctx->handle = skynet_handle_register(ctx); // 服务注册，获得服务编号
	ctx->queue = skynet_mq_create(DEFAULT_MESSAGE_QUEUE);
	ctx->calling = 1;
	// init function maybe use ctx->handle, so it must init at last

	int r = skynet_module_instance_init(mod, inst, ctx, parm);
	if (r == 0) {
		__sync_synchronize();
		ctx->calling = 0;
		return skynet_context_release(ctx);
	} else {
		skynet_context_release(ctx);
		skynet_handle_retire(ctx->handle);
		return NULL;
	}
}

// 增加 ctx 的引用计数
void 
skynet_context_grab(struct skynet_context *ctx) {
	__sync_add_and_fetch(&ctx->ref,1);
}

// 销毁服务
static void 
_delete_context(struct skynet_context *ctx) {
	skynet_module_instance_release(ctx->mod, ctx->instance);
	skynet_mq_release(ctx->queue);
	free(ctx);
}

// ctx 释放
// 引用计数 为 0 则销毁
struct skynet_context * 
skynet_context_release(struct skynet_context *ctx) {
	if (__sync_sub_and_fetch(&ctx->ref,1) == 0) {
		_delete_context(ctx);
		return NULL;
	}
	return ctx;
}

// 将消息丢弃到黑洞
static void
_drop_message(int source, const char * addr , void * data, size_t sz) {
	struct blackhole * b = malloc(sizeof(*b));
	b->source = source;
	b->destination = strdup(addr);
	b->data = data;
	b->sz = sz;

	int des = skynet_handle_findname(BLACKHOLE);
	if (des<0)
		return;

	struct skynet_message msg;
	msg.source = source;
	msg.destination = des;
	msg.data = b;
	msg.sz = sizeof(*b);
	skynet_mq_push(&msg);
}

// 调用ctx的回调函数
static void
_dispatch_message(struct skynet_context *ctx, struct skynet_message *msg) {
	// source 等于 -1 表示源于系统，见 skynet_timeout
	if (msg->source == -1) {
		ctx->cb(ctx, ctx->cb_ud, NULL, msg->data, msg->sz);
	} else {
		char tmp[10];
		tmp[0] = ':';
		_id_to_hex(tmp+1, msg->source);
		ctx->cb(ctx, ctx->cb_ud, tmp, msg->data, msg->sz);

		free(msg->data);
	}
}

/*
 从全局消息队列中取出消息分发，返回 1 表示阻塞，当前无消息
 这个时候的消息队列比较简单，所有消息都是放进全局消息队列中，服务的私有消息队列只是
 存放当服务被占用导致无法处理消息的时候的消息
*/
int
skynet_context_message_dispatch(void) {
	struct skynet_message msg;
	/*
	 从全局消息队列中取出一个消息
	*/
	int handle = skynet_mq_pop(&msg);	// handle 为消息的目的地的服务编号
	if (handle < 0) {
		return 1;
	}
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {	// 服务已销毁则丢弃消息到黑洞
		char tmp[10];
		tmp[0] = ':';
		_id_to_hex(tmp+1, msg.destination);
		_drop_message(msg.source, tmp, msg.data, msg.sz);
		return 0;
	}
	if (__sync_lock_test_and_set(&ctx->calling, 1)) {	// 服务还没有设置好，无法处理此消息，所以先放入消息队列
		// When calling, push to context's message queue
		skynet_mq_enter(ctx->queue, &msg);
	} else {	// 处理此消息
		if (ctx->cb == NULL) { // 回调函数为空则丢弃消息到黑洞
			char tmp[10];
			tmp[0] = ':';
			_id_to_hex(tmp+1, msg.destination);
			_drop_message(msg.source, tmp, msg.data, msg.sz);
		} else {
			_dispatch_message(ctx, &msg);	// 处理消息
			while(skynet_mq_leave(ctx->queue,&msg) >=0) {	// 处理 ctx 私有消息队列剩余消息
				_dispatch_message(ctx,&msg);
			}
		}
		__sync_lock_release(&ctx->calling);
	}

	skynet_context_release(ctx);

	return 0;
}

const char * 
skynet_command(struct skynet_context * context, const char * cmd , const char * parm) {
	if (strcmp(cmd,"TIMEOUT") == 0) {	// 添加一个定时器消息
		//time:session
		char * session_ptr = NULL;
		//strtol会将parm按照10指定的基数转换然后返回。遇到的第一个非法值会将地址赋值给第二个参数
		int ti = strtol(parm, &session_ptr, 10);
		char sep = session_ptr[0];
		assert(sep == ':');
		int session = strtol(session_ptr+1, NULL, 10);
		skynet_timeout(context->handle, ti, session);
		return NULL;
	}

	if (strcmp(cmd,"NOW") == 0) {	// 返回系统开机到现在的时间，单位是 10 毫秒
		uint32_t ti = skynet_gettime();
		sprintf(context->result,"%u",ti);
		return context->result;
	}

	if (strcmp(cmd,"REG") == 0) {	// 为服务注册名字， parm 为空则返回服务编号的十六进制字符串
		if (parm == NULL || parm[0] == '\0') {
			return context->handle_name;
		} else {
			return skynet_handle_namehandle(context->handle, parm);
		}
	}

	if (strcmp(cmd,"EXIT") == 0) {	// 服务销毁
		skynet_handle_retire(context->handle);
		return NULL;
	}

	if (strcmp(cmd,"LAUNCH") == 0) {	// 加载新的服务
		size_t sz = strlen(parm);
		char tmp[sz+1];
		strcpy(tmp,parm);
		char * parm = tmp;
		char * mod = strsep(&parm, " \t\r\n");
		parm = strsep(&parm, "\r\n");
		struct skynet_context * inst = skynet_context_new(mod,parm);
		if (inst == NULL) {
			return NULL;
		} else {
			context->result[0] = ':';
			_id_to_hex(context->result+1, inst->handle);
			return context->result;
		}
	}

	return NULL;
}

/*
 服务 context->handle 给服务 addr 发消息
 addr: 如果以':'开头则后面跟的是 handle ，如果以'.'开头则后面跟的是 handle name
*/
void 
skynet_send(struct skynet_context * context, const char * addr , void * msg, size_t sz) {
	int des = -1;
	if (addr[0] == ':') {
		des = strtol(addr+1, NULL, 16);
	} else if (addr[0] == '.') {
		des = skynet_handle_findname(addr + 1);
		if (des < 0) {
			_drop_message(context->handle, addr, (void *)msg, sz);
			return;
		}
	}

	assert(des >= 0);
	struct skynet_message smsg;
	smsg.source = context->handle;
	smsg.destination = des;
	smsg.data = msg;
	smsg.sz = sz;
	skynet_mq_push(&smsg);
}

// 返回 ctx 的 handle
int 
skynet_context_handle(struct skynet_context *ctx) {
	return ctx->handle;
}

// 设置 ctx 的 handle
void 
skynet_context_init(struct skynet_context *ctx, int handle) {
	ctx->handle = handle;
}

// 设置 ctx 的 回调函数接口以及传入回调函数的第二个参数
void 
skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb) {
	assert(context->cb == NULL);
	context->cb = cb;
	context->cb_ud = ud;
}

