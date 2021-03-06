--[[
demo说明:
1. 演示wifi联网操作
2. 演示长连接操作
3. 演示简易的网络状态灯
]]
_G.sys = require("sys")
_G.httpv2 = require("httpv2")

log.info("main", "simple http demo")

-- //////////////////////////////////////////////////////////////////////////////////////
-- wifi 相关的代码
if wlan ~= nil then
    log.info("mac", wlan.get_mac())
    local ssid = "uiot"
    local password = "12345678"
    -- 方式1 直接连接, 简单快捷
    --wlan.connect(ssid, password) -- 直接连
    -- 方式2 先扫描,再连接. 例如根据rssi(信号强度)的不同, 择优选择ssid
    sys.taskInit(function()
        wlan.scan()
        sys.waitUntil("WLAN_SCAN_DONE", 30000)
        local re = wlan.scan_get_info()
        log.info("wlan", "scan done", #re)
        for i in ipairs(re) do
            log.info("wlan", "info", re[i].ssid, re[i].rssi)
        end
        log.info("wlan", "try connect to wifi")
        wlan.connect(ssid, password)
        sys.waitUntil("WLAN_READY", 15000)
        log.info("wifi", "self ip", socket.ip())
    end)
    -- 方法3 airkiss配网, 可参考 app/playit/main.lua
end

-- airkiss.auto(27) -- 预留的功能,未完成 
-- //////////////////////////////////////////////////////////////////////////////////////

--- 从这里开始, 代码与具体网络无关

-- 联网后自动同步时间
sys.subscribe("NET_READY", function ()
    log.info("net", "!!! network ready event !!! send ntp")
    sys.taskInit(function()
        sys.wait(2000)
        socket.ntpSync()
    end)
end)

gpio.setup(21, 0)
_G.use_netled = 1 -- 启用1, 关闭0
sys.taskInit(function()
    while 1 do
        --log.info("wlan", "ready?", wlan.ready())
        if socket.isReady() then
            --log.info("netled", "net ready, slow")
            gpio.set(21, 1 * use_netled)
            sys.wait(1900)
            gpio.set(21, 0)
            sys.wait(100)
        else
            --log.info("netled", "net not ready, fast")
            gpio.set(21, 1 * use_netled)
            sys.wait(100)
            gpio.set(21, 0)
            sys.wait(100)
        end
    end
end)

sys.taskInit(function()
    -- 等待联网成功
    while true do
        while not socket.isReady() do 
            log.info("net", "wait for network ready")
            sys.waitUntil("NET_READY", 1000)
        end
        log.info("main", "http loop")
        
        collectgarbage("collect")
        local code, headers, body = httpv2.request("GET", "http://ip.nutz.cn/json")
        log.info("http", "resp", code, body)
        if tonumber(code) == 200 then
            log.info("ip", "my outer network ip", json.decode(body).ip)
        end
        sys.wait(5000)
    end
end)


sys.run()
