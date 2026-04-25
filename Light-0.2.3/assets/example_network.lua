
local window = Light(Light.UI.Window):New()
local client = Light(Light.Network.Http):New('127.0.0.1', 80)
local server = Light(Light.Network.HttpServer):New('127.0.0.1', 8081)

function window:OnOpen()

end

function window:Update(dt)
end

function window:Draw()
end

function window:OnMouseButton(x, y, button, action, mods)
end

function client:OnConnect()
  print('client:OnConnect')
  client:SendRequest(2, '/', '')
end

function client:OnHttp(status, headers, body)
  print('client:OnHttp', status, headers, body)
end

function server:OnConnect(client)
  -- print('server:OnConnect', self, client)
end

function server:OnClose(client)
  -- print('server:OnClose', self, client)
end

-- local resp = "Hello ChocoLight!" .. string.rep("1", 1024 * 64)

function server:OnHttp(client, status, headers, body)
  -- print('server:OnHttp', client, status, headers, body)
  return 200, "Hello ChocoLight!"
  -- return 200, resp
end

window:Open()
-- client:Open()
server:Open()
