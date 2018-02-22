local skynet = require "skynet"

--这里的function虽然没有声明参数，但是真实调用的时候其实是有传入参数的，只是暂时不需要
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
