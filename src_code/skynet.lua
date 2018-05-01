local c = require "skynet.c"
local tostring = tostring
local tonumber = tonumber
local coroutine = coroutine
local assert = assert

local skynet = {}
local session_id_coroutine = {}			-- session -> co
local session_coroutine_id = {}			-- co -> session
local session_coroutine_address = {}	-- co -> addr

--[[
因为 coroutine.resume 都是在 suspend 中调用，所以 coroutine.yield 之后
必定回到 suspend 中。
--]]

local function suspend(co, result, command, param)
	assert(result, command)
	if command == "CALL" or command == "SLEEP" then
		session_id_coroutine[param] = co
	elseif command == "RETURN" then
		local co_session = session_coroutine_id[co]
		local co_address = session_coroutine_address[co]
		c.send(co_address, co_session, param)
	else
		assert(command == nil, command)
		session_coroutine_id[co] = nil
		session_coroutine_address[co] = nil
	end
end

function skynet.timeout(ti, func, ...)
	local session = c.command("TIMEOUT",tostring(ti))
	assert(session)
	session = tonumber(session)
	local co = coroutine.create(func)
	assert(session_id_coroutine[session] == nil)
	session_id_coroutine[session] = co
	suspend(co, coroutine.resume(co, ...))
end

--[[
等价于 skynet.sleep(0)
--]]
function skynet.yield()
	local session = c.command("TIMEOUT","0")
	coroutine.yield("SLEEP", tonumber(session))
end

--[[
新建一个定时器消息在 ti 时间后执行，之后将
session co 对保存
--]]
function skynet.sleep(ti)
	local session = c.command("TIMEOUT",tostring(ti))
	assert(session)
	coroutine.yield("SLEEP", tonumber(session))
end

--[[
 注册服务名字，如果 name 是以 "." 开始的，则表示节点内注册
 否则表示注册全局服务名
--]]
function skynet.register(name)
	return c.command("REG", name)
end

--[[
 返回服务编号的十六进制字符串
--]]
function skynet.self()
	return c.command("REG")
end

--[[
 加载新的服务
--]]
function skynet.launch(...)
	return c.command("LAUNCH", table.concat({...}," "))
end

--[[
 返回系统开机到现在的时间，单位是 10 毫秒
--]]
function skynet.now()
	return tonumber(c.command("NOW"))
end

--[[
 服务销毁
--]]
function skynet.exit()
	c.command("EXIT")
end

function skynet.kill(name)
	c.command("KILL",name)
end

skynet.send = c.send

function skynet.call(addr, message)
	local session = c.send(addr, -1, message)
	return coroutine.yield("CALL", session)
end

function skynet.ret(message)
	coroutine.yield("RETURN", message)
end

function skynet.dispatch(f)
	c.callback(function(session, address , message)
		if session <= 0 then
			session = - session
			co = coroutine.create(f)
			session_coroutine_id[co] = session
			session_coroutine_address[co] = address
			suspend(co, coroutine.resume(co, message, session, address))
		else
			local co = session_id_coroutine[session]
			assert(co, session)
			session_id_coroutine[session] = nil
			suspend(co, coroutine.resume(co, message))
		end
	end)
end

function skynet.start(f)
	local session = c.command("TIMEOUT","0")
	local co = coroutine.create(f)
	session_id_coroutine[tonumber(session)] = co
end

return skynet
