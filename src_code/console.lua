local skynet = require "skynet"

skynet.callback(function()
	local cmd = io.read()
	local handle = skynet.command("LAUNCH",cmd)
	if handle == nil then
		print("Launch error:",cmd)
	else
		print("Lauch:",handle)
	end
	skynet.command("TIMEOUT","0:0")
end)

skynet.command("TIMEOUT","0:0")	--定时器消息，循环读取输入的关键
