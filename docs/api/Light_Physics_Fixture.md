# Light.Physics.Fixture

## `Light.Physics.Fixture.GetBody`

Get the Body that owns this Fixture

### 返回值

`table Body object or nil`

---

## `Light.Physics.Fixture.SetDensity`

Set Fixture density and refresh mass data

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `value` | `number` | Density |

---

## `Light.Physics.Fixture.GetDensity`

Get Fixture density

### 返回值

`number Density`

---

## `Light.Physics.Fixture.SetFriction`

Set Fixture friction

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `value` | `number` | Friction |

---

## `Light.Physics.Fixture.GetFriction`

Get Fixture friction

### 返回值

`number Friction`

---

## `Light.Physics.Fixture.SetRestitution`

Set Fixture restitution

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `value` | `number` | Restitution |

---

## `Light.Physics.Fixture.GetRestitution`

Get Fixture restitution

### 返回值

`number Restitution`

---

## `Light.Physics.Fixture.SetSensor`

Set whether this Fixture is a sensor

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `enabled` | `boolean` | Sensor flag |

---

## `Light.Physics.Fixture.IsSensor`

Check whether this Fixture is a sensor

### 返回值

`boolean Sensor flag`

---

## `Light.Physics.Fixture.SetFilterData`

Set collision filter data

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `categoryBits` | `number` | Category bits |
| `maskBits` | `number` | Mask bits |
| `groupIndex` | `number` | Group index |

---

## `Light.Physics.Fixture.GetFilterData`

Get collision filter data

### 返回值

`number categoryBits, maskBits, groupIndex`

---

## `Light.Physics.Fixture.SetUserData`

Attach Lua user data to this Fixture

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `value` | `any` | User data, nil clears it |

---

## `Light.Physics.Fixture.GetUserData`

Get Lua user data attached to this Fixture

### 返回值

`any User data or nil`

---
