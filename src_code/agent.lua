local skynet = require "skynet"

-- 输出传入 agent.lua 文件的参数
print("agent",...)

skynet.dispatch(function(msg , addr)
	print("[agent]",addr,msg)
end)
