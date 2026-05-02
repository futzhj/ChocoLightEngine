# Light.Physics.Body

## `Light.Physics.Body.CreateFixture`

Create a Fixture from a Shape

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `shape` | `table` | Shape object |
| `density` | `number` | Density, defaults to 1.0 |

### 返回值

`table Fixture object`

---

## `Light.Physics.Body.DestroyFixture`

Destroy a Fixture on this Body

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `fixture` | `table` | Fixture object |

---

## `Light.Physics.Body.AddBox`

Compatibility API that adds a rectangle Fixture

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `width` | `number` | Width in pixels |
| `height` | `number` | Height in pixels |

### 返回值

`table Fixture object`

---

## `Light.Physics.Body.AddCircle`

Compatibility API that adds a circle Fixture

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `radius` | `number` | Radius in pixels |

### 返回值

`table Fixture object`

---

## `Light.Physics.Body.GetType`

Get Body type

### 返回值

`string static, dynamic, or kinematic`

---

## `Light.Physics.Body.SetType`

Set Body type

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `type` | `string` | static, dynamic, or kinematic |

---

## `Light.Physics.Body.GetPosition`

Get Body position in pixels

### 返回值

`number x, y`

---

## `Light.Physics.Body.SetPosition`

Set Body position in pixels

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `x` | `number` | X coordinate |
| `y` | `number` | Y coordinate |

---

## `Light.Physics.Body.GetAngle`

Get Body angle

### 返回值

`number Angle in radians`

---

## `Light.Physics.Body.SetAngle`

Set Body angle

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `angle` | `number` | Angle in radians |

---

## `Light.Physics.Body.GetLinearVelocity`

Get linear velocity

### 返回值

`number vx, vy in pixels per second`

---

## `Light.Physics.Body.SetLinearVelocity`

Set linear velocity

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `vx` | `number` | X velocity in pixels per second |
| `vy` | `number` | Y velocity in pixels per second |

---

## `Light.Physics.Body.GetAngularVelocity`

Get angular velocity

### 返回值

`number Angular velocity`

---

## `Light.Physics.Body.SetAngularVelocity`

Set angular velocity

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `value` | `number` | Angular velocity |

---

## `Light.Physics.Body.ApplyForce`

Apply force to the Body center

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `fx` | `number` | X force |
| `fy` | `number` | Y force |

---

## `Light.Physics.Body.ApplyImpulse`

Apply linear impulse to the Body center

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `ix` | `number` | X impulse |
| `iy` | `number` | Y impulse |

---

## `Light.Physics.Body.ApplyTorque`

Apply torque to this Body

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `torque` | `number` | Torque |

---

## `Light.Physics.Body.SetAwake`

Set whether this Body is awake

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `enabled` | `boolean` | Awake flag |

---

## `Light.Physics.Body.IsAwake`

Check whether this Body is awake

### 返回值

`boolean Awake flag`

---

## `Light.Physics.Body.SetActive`

Set whether this Body is enabled

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `enabled` | `boolean` | Enabled flag |

---

## `Light.Physics.Body.IsActive`

Check whether this Body is enabled

### 返回值

`boolean Enabled flag`

---

## `Light.Physics.Body.SetBullet`

Enable or disable bullet continuous collision mode

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `enabled` | `boolean` | Bullet flag |

---

## `Light.Physics.Body.IsBullet`

Check whether this Body uses bullet mode

### 返回值

`boolean Bullet flag`

---

## `Light.Physics.Body.SetLinearDamping`

Set linear damping

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `value` | `number` | Linear damping |

---

## `Light.Physics.Body.GetLinearDamping`

Get linear damping

### 返回值

`number Linear damping`

---

## `Light.Physics.Body.SetAngularDamping`

Set angular damping

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `value` | `number` | Angular damping |

---

## `Light.Physics.Body.GetAngularDamping`

Get angular damping

### 返回值

`number Angular damping`

---

## `Light.Physics.Body.GetMass`

Get Body mass

### 返回值

`number Mass`

---

## `Light.Physics.Body.GetInertia`

Get Body rotational inertia

### 返回值

`number Rotational inertia`

---

## `Light.Physics.Body.GetWorldCenter`

Get Body world center

### 返回值

`number x, y in pixels`

---

## `Light.Physics.Body.GetLocalCenter`

Get Body local center

### 返回值

`number x, y in pixels`

---

## `Light.Physics.Body.SetRestitution`

Set restitution for all current Fixtures on this Body

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `value` | `number` | Restitution |

---

## `Light.Physics.Body.SetFriction`

Set friction for all current Fixtures on this Body

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `value` | `number` | Friction |

---
