# Light.Physics

## `Light.Physics.NewCircleShape`

Create a circle Shape

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `radius` | `number` | Radius in pixels |

### 返回值

`table Shape object`

---

## `Light.Physics.NewRectangleShape`

Create a rectangle Shape

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `width` | `number` | Width in pixels |
| `height` | `number` | Height in pixels |

### 返回值

`table Shape object`

---

## `Light.Physics.NewPolygonShape`

Create a polygon Shape

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `vertices` | `table` | Vertex array {x1,y1,x2,y2,...} |

### 返回值

`table Shape object`

---

## `Light.Physics.NewEdgeShape`

Create an edge Shape

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `x1` | `number` | Start X position |
| `y1` | `number` | Start Y position |
| `x2` | `number` | End X position |
| `y2` | `number` | End Y position |

### 返回值

`table Shape object`

---

## `Light.Physics.NewChainShape`

Create a chain Shape

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `vertices` | `table` | Vertex array {x1,y1,x2,y2,...} |
| `loop` | `boolean` | Whether the chain is closed |

### 返回值

`table Shape object`

---
