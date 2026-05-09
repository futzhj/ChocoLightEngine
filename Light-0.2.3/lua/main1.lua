-- ... 然后正常使用
local HelloWindow = Light(Light.UI.Window):New()

function HelloWindow:OnOpen()
  -- 窗口图形初始化的时候
  -- 放一些图片、字体的初始化

  -- 属性：字体初始化，例如方正字体导入，字体大小 18
  self.fangZhengFont = Light(Light.Graphics.Font):New("assets/fangzheng.ttf", 18)

  -- 属性：图片初始化
  self.birdImage = Light(Light.Graphics.Image):New("assets/bird.jpg")

  self.sound = Light(Light.AV.Audio):New("assets/sound.wav")

  -- 视频播放测试: 先确保 Video 子模块已加载
  local ok, err = pcall(require, "Light.AV.Video")
  if ok and Light.AV.Video then
    self.video = Light(Light.AV.Video):New("assets/test1.mp4")
    print("Video loaded:", self.video)
  else
    print("Video module load failed:", err)
  end
  
end

function HelloWindow:Update(dt)
  -- 更新数据：一些成员、变量每帧 的更新逻辑写在这里

  -- 视频帧推进 (按 PTS 时序自动解码下一帧)
  if self.video then
    Light.AV.Video.Update(self.video)
  end
end

function HelloWindow:Draw()
  -- 更新绘制：要画什么写这里

  -- 绘制纹理
  Light.Graphics.Draw(
    self.birdImage,
    0, 0, 0 -- 坐标：x, y, z
  -- 0, 0, 0, -- 旋转：绕x轴，绕y轴，绕z轴
  -- 1, 1, 1, -- 放大：x 轴放大，y 轴放大，z 轴放大
  -- 0, 0, 0 -- 原点：偏移 x，偏移 y，偏移 z
  )

  -- 绘制片区纹理
  Light.Graphics.DrawQuad(
    self.birdImage,
    0, 350, 0,
    0, 64, 64, 64
  )

  -- 绘制字体
  Light.Graphics.Print(
    "Hello World! 你好世界！引擎重建完成", -- 中英文混合文案
    self.fangZhengFont, -- 字体：使用方正字体
    200, 200, 0, -- 坐标：x, y, z
    0, 0, 0, -- 旋转：绕x轴，绕y轴，绕z轴
    1, 1, 1, -- 放大：x 轴放大，y 轴放大，z 轴放大
    0, 0, 0 -- 原点：偏移 x，偏移 y，偏移 z
  )

  -- 绘制线条
  Light.Graphics.Line(
    25, 50, 0,  -- 坐标 1：x, y, z
    25, 100, 0, -- 坐标 2：x, y, z
    0, 0, 0,    -- 旋转：绕x轴，绕y轴，绕z轴
    1, 1, 1,    -- 放大：x 轴放大，y 轴放大，z 轴放大
    0, 0, 0     -- 原点：偏移 x，偏移 y，偏移 z
  )

  -- 画三角形
  Light.Graphics.Triangle(
    Light.Graphics.FillMode, -- 模式：FillMode 填充，LineMode 描边
    0, 100, 0,               -- 坐标 1：x, y, z
    0, 200, 0,               -- 坐标 2：x, y, z
    100, 200, 0,             -- 坐标 3：x, y, z
    0, 0, 0,                 -- 旋转：绕x轴，绕y轴，绕z轴
    1, 1, 1,                 -- 放大：x 轴放大，y 轴放大，z 轴放大
    0, 0, 0                  -- 原点：偏移 x，偏移 y，偏移 z
  )

  -- 画长方形
  Light.Graphics.Rectangle(
    Light.Graphics.FillMode, -- 模式：FillMode 填充，LineMode 描边
    100, 0, 0,               -- 坐标：x, y, z
    100, 100, 0,             -- 大小：width 宽, height 高, depth 深
    0, 0, 0,                 -- 旋转：绕x轴，绕y轴，绕z轴
    1, 1, 1,                 -- 放大：x 轴放大，y 轴放大，z 轴放大
    0, 0, 0                  -- 原点：偏移 x，偏移 y，偏移 z
  )

  -- 画四边形（梯形、只要有四条边的）
  Light.Graphics.Quad(
    Light.Graphics.FillMode, -- 模式：FillMode 填充，LineMode 描边
    150, 150, 0,             -- 坐标 1：x, y, z
    150 + 50, 150, 0,        -- 坐标 2：x, y, z
    150 + 50, 150 + 50, 0,   -- 坐标 3：x, y, z
    150, 150 + 50, 0,        -- 坐标 4：x, y, z
    0, 0, 0,                 -- 旋转：绕x轴，绕y轴，绕z轴
    1, 1, 1,                 -- 放大：x 轴放大，y 轴放大，z 轴放大
    0, 0, 0                  -- 原点：偏移 x，偏移 y，偏移 z
  )

  -- 弧形
  Light.Graphics.Arc(
    Light.Graphics.FillMode, -- 模式：FillMode 填充，LineMode 描边
    100, 200, 0,             -- 坐标：x, y, z
    50,                      -- 半径
    0,                       -- 角度1
    90,                      -- 角度2
    16,                      -- 分段：分段越多，棱角越少，但越费资源
    0, 0, 0,                 -- 旋转：绕x轴，绕y轴，绕z轴
    1, 1, 1,                 -- 放大：x 轴放大，y 轴放大，z 轴放大
    0, 0, 0                  -- 原点：偏移 x，偏移 y，偏移 z
  )

  -- -- 画圆
  Light.Graphics.Circle(
    Light.Graphics.LineMode, -- 模式：FillMode 填充，LineMode 描边
    200, 200, 0,             -- 坐标：x, y, z
    50,                      -- 半径
    16,                      -- 分段：分段越多，棱角越少，但越费资源
    0, 0, 0,                 -- 旋转：绕x轴，绕y轴，绕z轴
    1, 1, 1,                 -- 放大：x 轴放大，y 轴放大，z 轴放大
    0, 0, 0                  -- 原点：偏移 x，偏移 y，偏移 z
  )

  -- 保存当前状态（颜色、平移、旋转等状态值）
  Light.Graphics.Push()

  -- 平移（到某个位置开始画）
  Light.Graphics.Translate(400, 300, 0)

  -- 画不规则形状
  Light.Graphics.Polygon(
    Light.Graphics.LineMode, -- 模式：FillMode 填充，LineMode 描边
    0, 0, 0,
    100, 50, 0,
    100, 100, 0,
    0, 100, 0
  )

  -- 切回上一个状态
  Light.Graphics.Pop()

  -- 视频播放: 绘制在 (300, 50), 宽 320, 高 240
  if self.video then
    Light.AV.Video.Draw(self.video, 300, 50, 320, 240)

    -- 显示视频状态
    local status = Light.AV.Video.IsPlaying(self.video) and "播放中" or "已结束"
    Light.Graphics.Print(
      "Video: " .. status,
      self.fangZhengFont,
      300, 30, 0
    )
  end
end

function HelloWindow:OnKey(key, scanCode, action, mods)
  -- 键盘事件
  print(self, key, scanCode, action, mods)
end

function HelloWindow:OnMouseButton(x, y, button, action, mods)
  -- 鼠标事件
  print(self, x, y, button, action, mods)

  -- 右键弹起 时候播放音效
  if button == 1 and action == 0 then
    Light.AV.Play(self.sound)
  end
end

function HelloWindow:OnMousePosition(x, y)
  -- 鼠标移动
  print(self, x, y)
end

function HelloWindow:OnMouseWheel(x, y)
  -- 鼠标滚轮
  print(self, x, y)
end

HelloWindow:Open(1000, 600)

-- 打开窗口可以带一些参数：
-- HelloWindow:Open(400, 300)
-- HelloWindow:Open(400, 300, "Haha")

-- 设置垂直同步：
HelloWindow:SetVSync(true)

-- 事件主循环：驱动每帧的 Update/Draw/SwapBuffers
while Light.UI.Loop() do
  Light.UI.Resume()
end
