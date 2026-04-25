# Light.DB.SQLite

## `Light.DB.SQLite.Execute`

执行 SQL 语句

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `sql` | `string` | SQL 语句 |

### 返回值

`number,table affected_rows, 结果行数组`

---

## `Light.DB.SQLite.Escape`

SQL 字符串转义 (单引号双写)

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `str` | `string` | 要转义的字符串 |

### 返回值

`string`

---

## `Light.DB.SQLite.Blob`

创建 X'AABBCC...' 格式 BLOB 字面量

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `data` | `string` | 二进制数据 |

### 返回值

`string SQL BLOB 字面量`

---

## `Light.DB.SQLite.TypeName`

Record ORM 字段类型 ID 转 SQL DDL 类型名

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `typeId` | `number` | 类型 ID (0=Serial ~ 13=TimeStamp) |

### 返回值

`string SQL 类型名`

---

## `Light.DB.SQLite.__call`

构造函数, 打开 SQLite 数据库

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `path` | `string` | 数据库文件路径 |

### 返回值

`void`

### 示例

```lua
local db = Light(Light.DB.SQLite):New("game.db")
```

---
