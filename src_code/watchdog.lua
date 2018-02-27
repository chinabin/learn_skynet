local skynet = require "skynet"

local command = {}

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
	print("[watchdog] open",self,fd,addr)
	local agent = skynet.command("LAUNCH","snlua agent.lua ".. self)
	if agent then
		skynet.send(".gate","forward ".. self .. " " .. agent)
	end
end

function command:close()
	print("[watchdog] close",self)
end

function command:data(data)
	print("[watchdog] data",self,#data,data)
end

-- 给 watchdog 发消息的回调函数
skynet.callback(function(from , message)
	local id, cmd , parm = string.match(message, "(%d+) (%w+) ?(.*)")
	id = tonumber(id)
	local f = command[cmd]
	if f then
		f(id,parm)	-- 上面的函数都有一个默认参数 self ，所以 id 其实是赋值给 self
	else
		skynet.error(string.format("[watchdog] Unknown command : %s %d %s",cmd,id,parm))
	end
end)

skynet.command("REG","watchdog")