local c = require "skynet.c"
local tostring = tostring
local tonumber = tonumber
local coroutine = coroutine
local assert = assert
local pairs = pairs

local proto = {}
local skynet = {}

function skynet.register_protocol(class)
	local name = class.name
	local id = class.id
	assert(proto[name] == nil)
	assert(type(name) == "string" and type(id) == "number" and id >=0 and id <=255)
	proto[name] = class
	proto[id] = class
end

local session_id_coroutine = {}
local session_coroutine_id = {}
local session_coroutine_address = {}

local wakeup_session = {}
local sleep_session = {}

-- suspend is function
local suspend

local function dispatch_wakeup()
	local co = next(wakeup_session)
	if co then
		wakeup_session[co] = nil
		local session = sleep_session[co]
		if session then
			session_id_coroutine[session] = "BREAK"
			return suspend(co, coroutine.resume(co, true))
		end
	end
end

-- suspend is local function
function suspend(co, result, command, param, size)
	if not result then
		error(debug.traceback(co,command))
	end
	if command == "CALL" then
		session_id_coroutine[param] = co
	elseif command == "SLEEP" then
		session_id_coroutine[param] = co
		sleep_session[co] = param
	elseif command == "RETURN" then
		local co_session = session_coroutine_id[co]
		local co_address = session_coroutine_address[co]
		-- PTYPE_RESPONSE = 1 , see skynet.h
		if param == nil then
			error(debug.traceback(co))
		end
		c.send(co_address, 1, co_session, param, size)
		return suspend(co, coroutine.resume(co))
	elseif command == nil then
		-- coroutine exit
		session_coroutine_id[co] = nil
		session_coroutine_address[co] = nil
	else
		error("Unknown command : " .. command .. "\n" .. debug.traceback(co))
	end
	dispatch_wakeup()
end

function skynet.timeout(ti, func)
	local session = c.command("TIMEOUT",tostring(ti))
	assert(session)
	session = tonumber(session)
	local co = coroutine.create(func)
	assert(session_id_coroutine[session] == nil)
	session_id_coroutine[session] = co
end

function skynet.fork(func,...)
	local args = { ... }
	skynet.timeout("0", function()
		func(unpack(args))
	end)
end

function skynet.sleep(ti)
	local session = c.command("TIMEOUT",tostring(ti))
	assert(session)
	local ret = coroutine.yield("SLEEP", tonumber(session))
	sleep_session[coroutine.running()] = nil
	if ret == true then
		return "BREAK"
	end
end

function skynet.yield()
	local session = c.command("TIMEOUT","0")
	assert(session)
	coroutine.yield("SLEEP", tonumber(session))
	sleep_session[coroutine.running()] = nil
end

function skynet.register(name)
	c.command("REG", name)
end

function skynet.name(name, handle)
	c.command("NAME", name .. " " .. handle)
end

local function string_to_handle(str)
	return tonumber("0x" .. string.sub(str , 2))
end

local self_handle
function skynet.self()
	if self_handle then
		return self_handle
	end
	self_handle = string_to_handle(c.command("REG"))
	return self_handle
end

function skynet.launch(...)
	local addr = c.command("LAUNCH", table.concat({...}," "))
	if addr then
		return string_to_handle(addr)
	end
end

function skynet.now()
	return tonumber(c.command("NOW"))
end

function skynet.starttime()
	return tonumber(c.command("STARTTIME"))
end

function skynet.exit()
	c.command("EXIT")
end

function skynet.kill(name)
	c.command("KILL",name)
end

function skynet.getenv(key)
	return c.command("GETENV",key)
end

function skynet.setenv(key, value)
	c.command("SETENV",key .. " " ..value)
end

function skynet.send(addr, typename, ...)
	local p = proto[typename]
	return c.send(addr, p.id, 0 , p.pack(...))
end

skynet.genid = assert(c.genid)

skynet.redirect = function(dest,source,typename,...)
	return c.redirect(dest, source, proto[typename].id, ...)
end

skynet.pack = assert(c.pack)
skynet.unpack = assert(c.unpack)
skynet.tostring = assert(c.tostring)

function skynet.call(addr, typename, ...)
	local p = proto[typename]
	local session = c.send(addr, p.id , nil , p.pack(...))
	return p.unpack(coroutine.yield("CALL", session))
end

function skynet.rawcall(addr, typename, msg, sz)
	local p = proto[typename]
	local session = c.send(addr, p.id , nil , msg, sz)
	return coroutine.yield("CALL", session)
end

function skynet.ret(msg, sz)
	msg = msg or ""
	coroutine.yield("RETURN", msg, sz)
end

function skynet.wakeup(co)
	if sleep_session[co] and wakeup_session[co] == nil then
		wakeup_session[co] = true
		return true
	end
end

function skynet.dispatch(typename, func)
	local p = assert(proto[typename],tostring(typename))
	assert(p.dispatch == nil, tostring(typename))
	p.dispatch = func
end

local function unknown_response(session, address, msg, sz)
	print("Response message :" , c.tostring(msg,sz))
	error(string.format("Unknown session : %d from %x", session, address))
end

function skynet.dispatch_unknown_response(unknown)
	local prev = unknown_response
	unknown_response = unknown
	return prev
end

local function dispatch_message(prototype, msg, sz, session, source, ...)
	-- PTYPE_RESPONSE = 1, read skynet.h
	if prototype == 1 then
		local co = session_id_coroutine[session]
		if co == "BREAK" then
			session_id_coroutine[session] = nil
		elseif co == nil then
			unknown_response(session, source, msg, sz)
		else
			session_id_coroutine[session] = nil
			suspend(co, coroutine.resume(co, msg, sz))
		end
	else
		local p = assert(proto[prototype], prototype)
		local f = p.dispatch
		if f then
			local co = coroutine.create(f)
			session_coroutine_id[co] = session
			session_coroutine_address[co] = source
			suspend(co, coroutine.resume(co, session,source, p.unpack(msg,sz, ...)))
		else
			print("Unknown request :" , p.unpack(msg,sz))
			error(string.format("Can't dispatch type %s : ", p.name))
		end
	end
end

function skynet.filter(f ,start)
	c.callback(function(...)
		dispatch_message(f(...))
	end)
	skynet.timeout(0, function()
		start()
		skynet.send(".launcher","lua", nil)
	end)
end

function skynet.newservice(name, ...)
	local handle = skynet.call(".launcher", "lua" , "snlua", name, ...)
	return handle
end

local function group_command(cmd, handle, address)
	if address then
		return string.format("%s %d :%x",cmd, handle, address)
	else
		return string.format("%s %d",cmd,handle)
	end
end

function skynet.enter_group(handle , address)
	c.command("GROUP", group_command("ENTER", handle, address))
end

function skynet.leave_group(handle , address)
	c.command("GROUP", group_command("LEAVE", handle, address))
end

function skynet.clear_group(handle)
	c.command("GROUP", "CLEAR " .. tostring(handle))
end

function skynet.query_group(handle)
	return string_to_handle(c.command("GROUP","QUERY " .. tostring(handle)))
end

function skynet.address(addr)
	return string.format(":%x",addr)
end

function skynet.harbor(addr)
	return c.harbor(addr)
end

------ remote object --------

do
	-- proto is 11

	local remote_query, remote_alloc, remote_bind = c.remote_init(skynet.self())
	local weak_meta = { __mode = "kv" }
	local meta = getmetatable(c.unpack(c.pack({ __remote = 0 })))
	local remote_call_func = setmetatable({}, weak_meta)
	setmetatable(meta, weak_meta)

	local _send = assert(c.send)
	local _yield = coroutine.yield
	local _pack = assert(c.pack)
	local _unpack = assert(c.unpack)
	local _local = skynet.self()

	function meta__index(t, method)
		local f = remote_call_func[method]
		if f == nil then
			f = function(...)
				local addr = remote_query(t.__remote)
				-- the proto is 11 (lua is 10)
				local session = _send(addr, 11 , nil, _pack(t,method,...))
				local msg, sz = _yield("CALL", session)
				return select(2,assert(_unpack(msg,sz)))
			end
			remote_call_func[method] = f
		end
		rawset(t,method,f)
		return f
	end

	-- prevent gc
	meta.__index = meta__index

	meta.__newindex = error

	function skynet.remote_create(t, handle)
		t = t or {}
		if handle then
			remote_bind(handle)
		else
			handle = remote_alloc()
		end
		rawset(t, "__remote" , handle)
		rawset(meta, handle, t)
		return t
	end

	function skynet.remote_bind(handle)
		return setmetatable( { __remote = handle } , meta)
	end

	local function remote_call(obj, method, ...)
		if type(obj) ~= "table" or type(method) ~= "string" then
			return _yield("RETURN", _pack(false, "Invalid call"))
		end
		local f = obj[method]
		if type(f) ~= "function" then
			return _yield("RETURN", _pack(false, "Object has not method " .. method))
		end
		return _yield("RETURN", _pack(pcall(f,...)))
	end

	function skynet.remote_root()
		return skynet.remote_bind(0)
	end

	skynet.register_protocol {
		name = "remoteobj",
		id = 11,
		unpack = c.unpack,
		dispatch = function (session, source, ...)
			remote_call(...)
		end
	}
end

----- register protocol
do
	local REG = skynet.register_protocol

	REG {
		name = "text",
		id = 0,
		pack = function (...)
			local n = select ("#" , ...)
			if n == 0 then
				return ""
			elseif n == 1 then
				return tostring(...)
			else
				return table.concat({...}," ")
			end
		end,
		unpack = c.tostring
	}

	REG {
		name = "lua",
		id = 10,
		pack = skynet.pack,
		unpack = skynet.unpack,
	}

	REG {
		name = "response",
		id = 1,
	}
end

function skynet.start(f)
	c.callback(dispatch_message)
	skynet.timeout(0, function()
		f()
		skynet.send(".launcher","lua", nil)
	end)
end

return skynet
