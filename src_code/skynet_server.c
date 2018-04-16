#include "skynet_server.h"
#include "skynet_module.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_timer.h"
#include "skynet_harbor.h"
#include "skynet.h"

#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define BLACKHOLE "blackhole"
#define DEFAULT_MESSAGE_QUEUE 16 

/*
节点：运行可执行服务端程序文件 "skynet" ，即启动了一个“节点”。每个节点可以启动多个服务( service )。

模块( module )：模块是一个动态库文件(.so)，若要创建一个能被 skynet 调用的模块，
				假设模块名为xxx，必须实现4个函数xxx_create、xxx_init、xxx_release。

服务( service )： module 被 skynet 加载运行，就成为一个 service 。
				  每个 service 有唯一的 handle ID 。同节点的 service 之间可以通过全局
				  消息队列通讯。

harbor： harbor 是一种特殊的 service ，它代理不同节点之间的通讯。
		 用一个 32 位（4字节）的变量来唯一标识一个服务：高 8 位表示该 service 所
		 属的 harbor ID ，低 24 位表示 handle ID 。如果消息队列中的某个消息所指
		 定处理者的 ID 的高 8 位（即 harbor ID )与本节点 harbor ID 不同，表示该
		 消息将经过 harbor 转发到另外一个节点的服务来处理。

service 的 context ：每个 service 对应一个 context ， context 保存了本 service 运行
					 时的各种状态和变量。
*/
struct skynet_context {
	void * instance;				// 实例指针，通过调用创建实例函数返回
	struct skynet_module * mod;		// 实例对应的模块
	uint32_t handle;				// 服务编号

	int ref;						// 引用计数
	char handle_name[10];			// handle 的十六进制字符串形式
	char result[32];				// 不同的命令会设置不同的返回值
	void * cb_ud;					// 回调函数的第二个参数
	skynet_cb cb;					// 回调函数指针，定义在 skynet.h : typedef void (*skynet_cb)(struct skynet_context * context, void *ud, const char * uid , const void * msg, size_t sz_session);
	int session_id;					// session_id 是一种约定，当消息发送方需要接收方回复的时候，两个消息的 session_id 一致。
	int in_global_queue;			// 当前服务的消息队列是否已经在全局消息队列的标识
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
skynet_context_new(const char * name, const char *parm) {
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
	ctx->in_global_queue = 0;
	ctx->session_id = 0;
	char * uid = ctx->handle_name;
	uid[0] = ':';
	_id_to_hex(uid+1, ctx->handle);	// 应该放在后面一句 ctx->handle 赋值语句后面，后面的版本修复了

	ctx->handle = skynet_handle_register(ctx); // 服务注册，获得服务编号
	ctx->queue = skynet_mq_create(ctx->handle);	// 创建和服务挂钩的消息队列
	// init function maybe use ctx->handle, so it must init at last

	int r = skynet_module_instance_init(mod, inst, ctx, parm);
	if (r == 0) {
		return skynet_context_release(ctx);
	} else {
		skynet_context_release(ctx);
		skynet_handle_retire(ctx->handle);
		return NULL;
	}
}

static int
_new_session(struct skynet_context *ctx) {
	int session = ++ctx->session_id;
	if (session < 0) {
		ctx->session_id = 1;
		return 1;
	}

	return session;
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
	if (!ctx->in_global_queue) {
		skynet_mq_release(ctx->queue);
	}
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

// 调用ctx的回调函数
static void
_dispatch_message(struct skynet_context *ctx, struct skynet_message *msg) {
	// source 等于 SKYNET_SYSTEM_TIMER 表示源于系统，见 skynet_timeout
	if (msg->source == SKYNET_SYSTEM_TIMER) {
		ctx->cb(ctx, ctx->cb_ud, msg->session, NULL, msg->data, msg->sz);
	} else {
		char tmp[10];
		tmp[0] = ':';
		_id_to_hex(tmp+1, msg->source);
		if (skynet_harbor_message_isremote(msg->source)) {
			void * data = skynet_harbor_message_open(msg);
			ctx->cb(ctx, ctx->cb_ud, msg->session, tmp, data, msg->sz);
			skynet_harbor_message_close(msg);
		} else {
			ctx->cb(ctx, ctx->cb_ud, msg->session, tmp, msg->data, msg->sz);
		}

		free(msg->data);
	}
}

// 丢弃消息队列
static void
_drop_queue(struct message_queue *q) {
	// todo: send message back to message source
	struct skynet_message msg;
	while(!skynet_mq_pop(q, &msg)) {
		if (skynet_harbor_message_isremote(msg.source)) {
			skynet_harbor_message_close(&msg);
		}
		free(msg.data);
	}
	skynet_mq_release(q);
}

/*
 从全局消息队列中取出消息分发，返回 1 表示阻塞，当前无消息
 这个时候的消息队列比较简单，所有消息都是放进全局消息队列中，服务的私有消息队列只是
 存放当服务被占用导致无法处理消息的时候的消息
*/
int
skynet_context_message_dispatch(void) {
	struct message_queue * q = skynet_globalmq_pop();	// 从全局消息队列中取出一个消息队列
	if (q==NULL)
		return 1;

	uint32_t handle = skynet_mq_handle(q);				// 获得消息队列挂钩的服务 id

	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {	// 服务已销毁则丢弃消息到黑洞
		skynet_error(NULL, "Drop message queue %u ", handle);
		_drop_queue(q);
		return 0;
	}
	assert(ctx->in_global_queue);

	int re_q = 1;
	struct skynet_message msg;
	if (skynet_mq_pop(q,&msg)) {
		// empty queue
		__sync_lock_release(&ctx->in_global_queue);		// 将 ctx->in_global_queue 置 0
		if (skynet_mq_pop(q,&msg)) {
			skynet_context_release(ctx);
			return 0;
		}
		if (__sync_lock_test_and_set(&ctx->in_global_queue, 1)) {
			re_q = 0;
		}
	}

	if (ctx->cb == NULL) {
		if (skynet_harbor_message_isremote(msg.source)) {
			skynet_harbor_message_close(&msg);
		}
		free(msg.data);
		skynet_error(NULL, "Drop message from %u to %u without callback , size = %d",msg.source, handle, (int)msg.sz);
	} else {	// 处理此消息
		_dispatch_message(ctx, &msg);
	}

	skynet_context_release(ctx);

	if (re_q) {
		skynet_globalmq_push(q);	// 塞回全局消息队列，这样的 push 和 pop 保证了公平性
	}

	return 0;
}

/*
 session : 约定的 session
*/
const char * 
skynet_command(struct skynet_context * context, const char * cmd , int session, const char * parm) {
	if (strcmp(cmd,"TIMEOUT") == 0) {	// 添加一个定时器消息，自己给自己发消息
		//time:session
		// 上面的 time:session 是作者旧的注释，添加 session 之后就变成错误的注释了
		// 当发送 TIMEOUT 命令的时候， session_ptr 就是定时器时间
		// 使用方法例如： skynet.command("TIMEOUT", 0, "0") 
		char * session_ptr = NULL;
		//strtol会将parm按照10指定的基数转换然后返回。遇到的第一个非法值会将地址赋值给第二个参数
		int ti = strtol(parm, &session_ptr, 10);
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
		} else if (parm[0] == '.') {
			return skynet_handle_namehandle(context->handle, parm + 1);
		} else {
			assert(context->handle!=0);
			skynet_harbor_register(parm, context->handle);
			return NULL;
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
skynet_send(struct skynet_context * context, const char * addr , int session, void * msg, size_t sz) {
	if (session < 0) {
		session = _new_session(context);
	}
	uint32_t des = 0;
	if (addr[0] == ':') {
		des = strtol(addr+1, NULL, 16);
	} else if (addr[0] == '.') {
		des = skynet_handle_findname(addr + 1);
		if (des == 0) {
			free(msg);
			skynet_error(context, "Drop message to %s, size = %d", addr, (int)sz);
			return;
		}
	} else {
		struct skynet_message smsg;
		smsg.source = context->handle;
		smsg.session = session;
		smsg.data = msg;
		smsg.sz = sz;
		skynet_harbor_send(addr, 0, &smsg);
		return;
	}

	assert(des > 0);
	struct skynet_message smsg;
	smsg.source = context->handle;
	smsg.session = session;
	smsg.data = msg;
	smsg.sz = sz;

	if (skynet_harbor_message_isremote(des)) {
		skynet_harbor_send(NULL, des, &smsg);
	} else if (skynet_context_push(des, &smsg)) {
		free(msg);
		skynet_error(NULL, "Drop message from %u to %s (size=%d)", smsg.source, addr, (int)sz);
		return;
	}
}

// 返回 ctx 的 handle
uint32_t 
skynet_context_handle(struct skynet_context *ctx) {
	return ctx->handle;
}

// 设置 ctx 的 handle
void 
skynet_context_init(struct skynet_context *ctx, uint32_t handle) {
	ctx->handle = handle;
}

// 设置 ctx 的 回调函数接口以及传入回调函数的第二个参数
void 
skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb) {
	assert(context->cb == NULL);
	context->cb = cb;
	context->cb_ud = ud;
}

// 将消息放入服务对应的消息队列，并将消息队列加入全局消息队列
// 返回 0 表示成功，返回 -1 表示 handle 对应的服务不存在
int
skynet_context_push(uint32_t handle, struct skynet_message *message) {
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		return -1;
	}
	if (message->session < 0) {
		message->session = _new_session(ctx);
	}
	skynet_mq_push(ctx->queue, message);
	if (__sync_lock_test_and_set(&ctx->in_global_queue,1) == 0) {	// 将 ctx->in_global_queue 设为 1 并返回 ctx->in_global_queue 操作之前的值。
		skynet_globalmq_push(ctx->queue);
	}
	skynet_context_release(ctx);

	return 0;
}