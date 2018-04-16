local skynet = require "skynet"

skynet.callback(function(session,addr,content)
	print("sn:",session)
	skynet.command("TIMEOUT", -1 , "100")		--100表示一秒后(100 * 10毫秒)，后面的整数只是一个简单的测试
end)

skynet.command("TIMEOUT",0,"0")