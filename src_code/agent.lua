local skynet = require "skynet"

-- 输出传入 agent.lua 文件的参数
print("agent",...)

skynet.callback(function(session, addr, msg)
	print("[agent]",session, addr,msg)
end)
