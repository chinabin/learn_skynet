local skynet = require "skynet"
local c = require "socket.c"

local socket = {}
local fd
local object

function socket.connect(addr)
	local ip, port = string.match(addr,"([^:]+):(.+)")
	port = tonumber(port)
	fd = c.open(ip,port)
	if fd == nil then
		return true
	end
	skynet.send(".connection", "text", "ADD", fd , skynet.address(skynet.self()))
	object = c.new()
end

function socket.stdin()
	skynet.send(".connection", "text", "ADD", 1 , skynet.address(skynet.self()))
	object = c.new()
end

function socket.push(msg,sz)
	if msg then
		c.push(object, msg, sz)
	end
end

function socket.read(bytes)
	return c.read(object, bytes)
end

function socket.readline(sep)
	return c.readline(object, sep)
end

function socket.readblock(...)
	return c.readblock(object,...)
end

function socket.write(...)
	c.write(fd, ...)
end

function socket.writeblock(...)
	c.writeblock(fd, ...)
end

function socket.close()
	if fd then
		c.close(fd)
		skynet.send(".connection","text", "DEL", fd)
		fd = nil
	end
end

return socket

