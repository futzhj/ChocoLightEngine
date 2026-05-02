# Light.Physics.World

## `Light.Physics.World.New`

Create a Box2D physics World instance

---

## `Light.Physics.World.SetGravity`

Set world gravity

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `gx` | `number` | X gravity in pixels per second squared |
| `gy` | `number` | Y gravity in pixels per second squared |

---

## `Light.Physics.World.GetGravity`

Get world gravity

### 返回值

`number gx, gy in pixels per second squared`

---

## `Light.Physics.World.Step`

Step the physics world and dispatch queued contact events

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `dt` | `number` | Time step in seconds |

---

## `Light.Physics.World.ClearForces`

Clear accumulated world forces

---

## `Light.Physics.World.SetAllowSleeping`

Set whether bodies may sleep

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `enabled` | `boolean` | Allow-sleep flag |

---

## `Light.Physics.World.SetContinuousPhysics`

Set whether continuous physics is enabled

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `enabled` | `boolean` | Continuous-physics flag |

---

## `Light.Physics.World.CreateBody`

Create a Body

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `type` | `string` | static, dynamic, or kinematic |
| `x` | `number` | X position in pixels |
| `y` | `number` | Y position in pixels |

### 返回值

`table Body object`

---

## `Light.Physics.World.DestroyBody`

Destroy a Body

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `body` | `table` | Body object |

---

## `Light.Physics.World.GetBodyCount`

Get the current number of live bodies

### 返回值

`number Body count`

---

## `Light.Physics.World.OnCollision`

Register the legacy collision callback

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `callback` | `function` | Callback with bodyA, bodyB arguments |

---

## `Light.Physics.World.BeginContact`

Register the BeginContact callback

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `callback` | `function` | Callback with a Contact object |

---

## `Light.Physics.World.EndContact`

Register the EndContact callback

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `callback` | `function` | Callback with a Contact object |

---
