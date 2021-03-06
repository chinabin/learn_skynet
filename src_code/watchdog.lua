local skynet = require "skynet"

local command = {}
local agent_all = {}

--[[
	%d 	表示数字
	%w 	表示字母和数字
	+ 	表示重复一次或者多次
	() 	表示捕获
	[] 	表示字符集，在字符集前面加一个 ^ 可以得到这个字符集的补集
	? 	表示重复0次或者多次
--]]--
function command:open(parm)
	local fd,addr = string.match(parm,"(%d+) ([^%s]+)")
	fd = tonumber(fd)
	skynet.send("LOG", string.format("%d %d %s",self,fd,addr))
	local client = skynet.launch("client",fd)
	skynet.send("LOG", "client " .. client)
	-- 这里启动一个 snlua 服务，返回服务地址。并且因为 open 命令处理的是用户连接，所以经常在网上看到的说每个用户接入会启动一个 agent 。
	local agent = skynet.launch("snlua","agent.lua",client)
	if agent then
		agent_all[self] = agent
		skynet.send("gate", "forward ".. self .. " " .. agent)
	end
end

function command:close()
	skynet.send("LOG", string.format("close %d",self))
	skynet.send(agent_all[self], -1,"CLOSE")
	agent_all[self] = nil
end

function command:data(data)
	local agent = agent_all[self]
	if agent then
		skynet.send(agent, data)
	else
		skynet.send("LOG", string.format("data %d size=%d",self,#data))
	end
end

-- 给 watchdog 发消息的回调函数
skynet.dispatch(function(message)
	local id, cmd , parm = string.match(message, "(%d+) (%w+) ?(.*)")
	id = tonumber(id)
	local f = command[cmd]
	if f then
		f(id,parm)	-- 上面的函数都有一个默认参数 self ，所以 id 其实是赋值给 self
	else
		skynet.error(string.format("[watchdog] Unknown command : %s",message))
	end
end)

skynet.register ".watchdog"