#include "skynet.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <string.h>
#include <stdlib.h>

//由 skynet_module_instance_create 调用，返回值作为 snlua_init 的第一个参数
lua_State *
snlua_create(void) {
	return luaL_newstate();		//创建新的 Lua 状态机。
}

static int
_load(lua_State *L, char ** filename) {
	const char * name = strsep(filename, " \r\n");
	int r = luaL_loadfile(L,name);
	return r != LUA_OK;
}

// 加载并执行 snlua XX.lua parms 中的 XX.lua 代码并传入 parms 参数
// snlua 服务的 init 接口中关于回调函数的设置放在 XX.lua 文件中。
int
snlua_init(lua_State *L, struct skynet_context *ctx, const char * args) {
	// 暂时关闭 GC 是为了加快速度
	lua_gc(L, LUA_GCSTOP, 0);	// 停止GC 
	luaL_openlibs(L);		//打开 Lua 标准库
	//下面两句等价于：找到 LUA_REGISTRYINDEX 索引所在的表t，使得t["skynet_context"] = ctx
	//将 ctx 参数最终传递给 luaopen_skynet 函数
	lua_pushlightuserdata(L, ctx);
	lua_setfield(L, LUA_REGISTRYINDEX, "skynet_context");
	lua_gc(L, LUA_GCRESTART, 0);	//重启GC 

	char tmp[strlen(args)+1];
	char *parm = tmp;
	strcpy(parm,args);

	//将lua文件加载
	const char * filename = parm;
	int r = _load(L, &parm);
	if (r) {
		skynet_error(ctx, "lua parser [%s] error : %s", filename, lua_tostring(L,-1));
		return 1;
	}
	//为后面调用lua文件代码做准备：获取参数
	int n=0;
	while(parm) {
		const char * arg = strsep(&parm, " \r\n");
		if (arg && arg[0]!='\0') {
			lua_pushstring(L, arg);
			++n;
		}
	}
	/*
		调用lua代码，因为lua代码第一句都是 require "skynet" ，
		所以将会调用 lua-skynet.c 文件中的 luaopen_skynet 函数
	*/
	r = lua_pcall(L,n,0,0);
	switch (r) {
	case LUA_OK:
		return 0;
	case LUA_ERRRUN:
		skynet_error(ctx, "lua do [%s] error : %s", filename, lua_tostring(L,-1));
		break;
	case LUA_ERRMEM:
		skynet_error(ctx, "lua memory error : %s",filename);
		break;
	case LUA_ERRERR:
		skynet_error(ctx, "lua message error : %s",filename);
		break;
	case LUA_ERRGCMM:
		skynet_error(ctx, "lua gc error : %s",filename);
		break;
	};

	return 1;
}

void
snlua_release(lua_State *L) {
	lua_close(L);
}
