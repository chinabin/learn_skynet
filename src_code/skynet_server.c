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
	void * instance;				//调用创建实例函数返回值，具体类型取决于创建函数
	struct skynet_module * mod;		// skynet 中的so库抽象
	int handle;
	int calling;
	int ref;
	char handle_name[10];	//handle 的十六进制字符串形式
	char result[32];
	void * cb_ud;			//回调函数的第二个参数
	skynet_cb cb;			//回调函数指针，定义在skynet.h
	struct message_queue *queue;
};

//数字转为16进制字符串
static void
_id_to_hex(char * str, int id) {
	int i;
	static char hex[16] = { '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F' };
	for (i=0;i<8;i++) {
		str[i] = hex[(id >> ((7-i) * 4))&0xf];
	}
	str[8] = '\0';
}

//传入模块名和参数，创建新的ctx并返回
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
	ctx->ref = 2;	//应该为1，但是因为后面调用了skynet_context_release会将引用计数减1，所以设置为2。//QUESTION: 为什么要调用skynet_context_release
	ctx->cb = NULL;
	ctx->cb_ud = NULL;
	char * uid = ctx->handle_name;
	uid[0] = ':';
	_id_to_hex(uid+1, ctx->handle);	//这里写错了，应该放在ctx->handle赋值语句后面，后面的版本修复了

	ctx->handle = skynet_handle_register(ctx);
	ctx->queue = skynet_mq_create(DEFAULT_MESSAGE_QUEUE);
	ctx->calling = 1;	//QUESTION: calling应该是标识ctx当前是否在被使用（1为使用，0为没使用）
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

//ctx->ref加1并返回更新后的值
void 
skynet_context_grab(struct skynet_context *ctx) {
	__sync_add_and_fetch(&ctx->ref,1);
}

//删除实例、消息队列，释放内存
static void 
_delete_context(struct skynet_context *ctx) {
	skynet_module_instance_release(ctx->mod, ctx->instance);
	skynet_mq_release(ctx->queue);
	free(ctx);
}

//ctx的引用计数减1，如果ctx的引用计数为0则删除ctx
struct skynet_context * 
skynet_context_release(struct skynet_context *ctx) {
	if (__sync_sub_and_fetch(&ctx->ref,1) == 0) {
		_delete_context(ctx);
		return NULL;
	}
	return ctx;
}

//将消息丢弃到黑洞
static void
_drop_message(int source, const char * addr , void * data, size_t sz) {
	struct blackhole * b = malloc(sizeof(*b));
	b->source = source;
	b->destination = strdup(addr);
	b->data = data;
	b->sz = sz;

	//QUESTION: 什么是黑洞？
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

//调用ctx的回调函数
static void
_dispatch_message(struct skynet_context *ctx, struct skynet_message *msg) {
	//source等于-1表示定时器时间到了
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

//从队列Q中取出一个消息，如果目的handle的ctx正在使用，则将该消息添加到ctx的消息队列
//否则则将ctx剩余的消息全部处理完
int
skynet_context_message_dispatch(void) {
	struct skynet_message msg;
	int handle = skynet_mq_pop(&msg);
	if (handle < 0) {
		return 1;
	}
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		char tmp[10];
		tmp[0] = ':';
		_id_to_hex(tmp+1, msg.destination);
		_drop_message(msg.source, tmp, msg.data, msg.sz);
		return 0;
	}
	if (__sync_lock_test_and_set(&ctx->calling, 1)) {
		// When calling, push to context's message queue
		skynet_mq_enter(ctx->queue, &msg);
	} else {
		if (ctx->cb == NULL) {
			char tmp[10];
			tmp[0] = ':';
			_id_to_hex(tmp+1, msg.destination);
			_drop_message(msg.source, tmp, msg.data, msg.sz);
		} else {
			_dispatch_message(ctx, &msg);
			while(skynet_mq_leave(ctx->queue,&msg) >=0) {
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
	if (strcmp(cmd,"TIMEOUT") == 0) {
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

	if (strcmp(cmd,"NOW") == 0) {
		uint32_t ti = skynet_gettime();
		sprintf(context->result,"%u",ti);
		return context->result;
	}

	if (strcmp(cmd,"REG") == 0) {
		if (parm == NULL || parm[0] == '\0') {
			return context->handle_name;
		} else {
			return skynet_handle_namehandle(context->handle, parm);
		}
	}

	if (strcmp(cmd,"EXIT") == 0) {
		skynet_handle_retire(context->handle);
		return NULL;
	}

	if (strcmp(cmd,"LAUNCH") == 0) {
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

void 
skynet_send(struct skynet_context * context, const char * addr , void * msg, size_t sz) {
	int des = -1;
	//':'后面跟的是handle，'.'后面跟的是handle name
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

//返回ctx的handle
int 
skynet_context_handle(struct skynet_context *ctx) {
	return ctx->handle;
}

//设置ctx的handle
void 
skynet_context_init(struct skynet_context *ctx, int handle) {
	ctx->handle = handle;
}

void 
skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb) {
	assert(context->cb == NULL);
	context->cb = cb;
	context->cb_ud = ud;
}

