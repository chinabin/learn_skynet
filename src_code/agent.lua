local skynet = require "skynet"

-- 输出传入 agent.lua 文件的参数
print("agent",...)

skynet.callback(function(addr, msg)
	print("[agent]",addr,msg)
end)
