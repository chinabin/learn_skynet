#include "skynet.h"

#include <stdio.h>
#include <stdlib.h>

struct logger {
	FILE * handle;	// logger 输出文件句柄
	int close;	// 为 1 表示 logger 输出是到文件，在关闭的时候需要关闭打开的文件。为 0 表示输入是到标准输出，在关闭的时候无需关闭。
};

struct logger *
logger_create(void) {
	struct logger * inst = malloc(sizeof(*inst));
	inst->handle = NULL;
	inst->close = 0;
	return inst;
}

void
logger_release(struct logger * inst) {
	if (inst->close) {
		fclose(inst->handle);
	}
	free(inst);
}

// uid: 源 handle
static void
_logger(struct skynet_context * context, void *ud, const char * uid, const void * msg, size_t sz) {
	struct logger * inst = ud;
	fprintf(inst->handle, "[%s] ",uid);
	fwrite(msg, sz , 1, inst->handle);	// size_t fwrite(const void* buffer, size_t size, size_t count, FILE* stream); 
	fprintf(inst->handle, "\n");
}

// parm: 日志输出文件名
int
logger_init(struct logger * inst, struct skynet_context *ctx, const char * parm) {
	if (parm) {
		inst->handle = fopen(parm,"w");
		if (inst->handle == NULL) {
			return 1;
		}
		inst->close = 1;
	} else {
		inst->handle = stdout;
	}
	if (inst->handle) {
		skynet_callback(ctx, inst, _logger);
		skynet_command(ctx, "REG", ".logger");
		return 0;
	}
	return 1;
}
