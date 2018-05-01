local skynet = require "skynet"
-- 传入 agent.lua 文件的参数
local client = ...

skynet.dispatch(function(msg,session)
	if session == 0 then
		print("client command",msg)
		local result = skynet.call("SIMPLEDB",msg)
		skynet.send(client, result)
	else
		print("server command",msg)
		if msg == "CLOSE" then
			skynet.kill(client)
			skynet.exit()
		end
	end
end)