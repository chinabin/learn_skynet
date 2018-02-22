local skynet = require "skynet"

skynet.callback(function(addr,content)
	if content == nil then
		print("sn:",addr)
		skynet.command("TIMEOUT","100:"..tostring(addr+1))	--100表示一秒后(100 * 10毫秒)，后面的整数只是一个简单的测试
	end
end)

skynet.command("TIMEOUT","0:0")