local skynet = require "skynet"

skynet.dispatch()

skynet.start(function()
	while true do
		local cmd = io.read()
		local handle = skynet.launch(cmd)
		if handle == nil then
			print("Launch error:",cmd)
		else
			print("Lauch:",handle)
		end
		skynet.yield()
	end
end)