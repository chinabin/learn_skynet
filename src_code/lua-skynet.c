#include "skynet.h"

// lua 代码中对 skynet 函数调用的实现

#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <string.h>

// lua 服务的回调函数入口，在 _callback 中设置，在 _dispatch_message 中被调用。最终调用 skynet.callback 中设定的回调函数
static void
_cb(struct skynet_context * context, void * ud, const char * addr, const void * msg, size_t sz_session) {
	lua_State *L = ud;
	//从注册表中获取 _cb 为 key 的值，放入栈中
	lua_rawgetp(L, LUA_REGISTRYINDEX, _cb);
	int r;
	if (msg == NULL) {
		lua_pushinteger(L, (int)sz_session);
		r = lua_pcall(L, 1, 0 , 0);
	} else {
		lua_pushstring(L, addr);
		lua_pushlstring(L, msg, sz_session);
		r = lua_pcall(L, 2, 0 , 0);
	}
	if (r == LUA_OK) 
		return;
	skynet_error(context, "lua error %s", lua_tostring(L,-1));
}

// 设置接收到消息时候的回调函数
static int
_callback(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	if (context == NULL) {
		return luaL_error(L, "Init skynet context first");
	}

	luaL_checktype(L,1,LUA_TFUNCTION);	//确保那些调用形式为 skynet.callback 的函数都是传入一个函数作为参数
	lua_settop(L,1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, _cb);
	// 获取主协程的 lua_State 。主协程就是在那个协程中产生其它协程的协程。
	lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
	lua_State *gL = lua_tothread(L,-1);

	//设置callback函数以及设置参数
	skynet_callback(context, gL, _cb);

	return 0;
}

// 执行 skynet 内置的命令
static int
_command(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	const char * cmd = luaL_checkstring(L,1);
	const char * result;
	if (lua_gettop(L) == 2) {
		const char * parm = luaL_checkstring(L,2);
		result = skynet_command(context, cmd, parm);
	} else {
		result = skynet_command(context, cmd, NULL);
	}
	if (result) {
		lua_pushstring(L, result);		// skynet.command 调用的返回结果
		return 1;
	}
	return 0;
}

// 给别的服务发消息
// skynet.send(des, str) 或者 skynet.send(des, userdata, size)
static int
_send(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	const char * dest = luaL_checkstring(L,1);
	int type = lua_type(L,2);
	if (type == LUA_TSTRING) {
		size_t len = 0;
		void * msg = (void *)lua_tolstring(L,2,&len);
		void * message = malloc(len);
		memcpy(message, msg, len);
		skynet_send(context, dest, message, len);
	} else {
		void * msg = lua_touserdata(L,2);
		if (msg == NULL) {
			return luaL_error(L, "skynet.send need userdata or string (%s)", lua_typename(L,type));
		}
		int size = luaL_checkinteger(L,3);
		skynet_send(context, dest, msg, size);
	}
	return 0;
}

// 将指定的字符串写入 logger
static int
_error(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	skynet_error(context, "%s", luaL_checkstring(L,1));
	return 0;
}

int
luaopen_skynet(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "send" , _send },
		{ "command" , _command },
		{ "callback" , _callback },
		{ "error", _error },
		{ NULL, NULL },
	};

	luaL_newlibtable(L,l);

	//把 LUA_REGISTRYINDEX 指向的表 t 中键为 "skynet_context" 的值入栈
	//表 t 中键为 "skynet_context" 的值在 snlua_init 中调用 lua_setfield 设置
	lua_getfield(L, LUA_REGISTRYINDEX, "skynet_context");
	struct skynet_context * ctx = lua_touserdata(L,-1);
	if (ctx == NULL) {
		return luaL_error(L, "Init skynet context first");
	}

	// 上面的 lua_getfield 调用是将 ctx 作为上值压入栈
	// 而 luaL_setfuncs 函数的最后一个参数设置使得所有 luaL_Reg 注册的函数都共享这个 ctx
	// 所有注册的函数第一句都是 struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1)); 来获取这个 ctx
	luaL_setfuncs(L,l,1);

	return 1;
}
