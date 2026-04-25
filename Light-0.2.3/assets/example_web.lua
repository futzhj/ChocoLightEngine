
--- 发送 GET HTTP 请求，后面以此类推
Light.Network.Web.Session:Get('127.0.0.1', 80, '/', {}, function (status, headers, body)
  print('Session:Get', status, headers, body)
end)

-- Light.Network.Web.Session:Post

-- Light.Network.Web.Session:Put

-- Light.Network.Web.Session:Delete

-- 新建 WebSocket 会话
local s = Light.Network.Web.Chat(
  '127.0.0.1', 80, '/',
  function (self) -- onJoin 加入成功的时候
    print('join ok', self)
    self:Send('nihao') -- 连接成功，发送一条问候
  end,
  function (self) -- onLeft 关闭连接的时候
    print('left')
  end,
  function (self, message, isBin) -- onMessage 有消息的时候
    print('on message', self, message, isBin)
  end
)

--- 以下是 Web 服务的用例
--- 绑定 '/' 网站根目录 的接口，并给回调函数
Light.Network.Web:Get('/', function (header, body)
  return 200, "Hello world!"
end)

--- 绑定 Web 服务器当中 WebSocket 消息回调
Light.Network.Web:Message(function (message, uId, isBin)
  print('Got message', message, '| from', uId, '| is bin?', isBin)
  Light.Network.Web:Send(uId, message, true)
end)

--- 有连接连上 Web 服务
Light.Network.Web:Join(function (uId)
  print('Join from', uId)
end)

--- 有连接断开 Web 服务
Light.Network.Web:Leave(function (uId)
  print('Leave from', uId)
end)

--- 绑定完了之后，开启服务
Light.Network.Web:Open('127.0.0.1', 80)
