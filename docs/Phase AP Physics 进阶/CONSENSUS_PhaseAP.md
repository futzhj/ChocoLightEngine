# Phase AP — Physics 进阶 — Consensus

## 锁定的决策

### 1. 范围: 一次交付 (71 fns)

| 子模块 | fns | 优先级 |
|---|---|---|
| Joint Limits + Motor 扩展 (Revolute/Prismatic) | 10 | P0 |
| DistanceJoint 弹簧参数 | 4 | P0 |
| WheelJoint | 9 | P0 |
| MotorJoint | 9 | P0 |
| FrictionJoint | 4 | P0 |
| PulleyJoint | 6 | P0 |
| GearJoint | 4 | P0 |
| PreSolve/PostSolve + Contact ops | 8 | P1 |
| Body 邻居遍历 (GetContactList/GetJointList) + Fixture.RayCast | 3 | P1 |
| Body 高级属性 (GravityScale/FixedRotation/SleepingAllowed/MassData) | 10 | P2 |
| World 仿真控制 (SubStepping/WarmStarting) | 4 | P2 |
| **合计** | **71** | |

### 2. 架构决策

| 决策 | 选择 |
|---|---|
| **Q1 PreSolve 重入** | **(A)** 文档标注, 不额外防护 — 与 Box2D 原生一致, 用户自行避免 Step 内 CreateBody/DestroyBody |
| **Q2 GearJoint 销毁顺序** | 文档标注 (GearJoint 必须先于组合 joint 销毁) — 不强持有 ref |
| **Q3 PulleyJoint 签名** | **flat 11 参数** — 与其他 CreateXxxJoint 一致 |

### 3. 实施顺序 (一次 commit)

1. **扩展 PhysicsJoint wrapper** — GearJoint 需要 joint1/joint2 的 selfRef 存储 (弱引用, 仅用于 SafetyCheck)
2. **扩展 PhysicsContactListener** — 加 PreSolve/PostSolve virtual override
3. **新增 Contact 对象方法** — SetEnabled/IsEnabled/SetFriction/Restitution 等, userdata 保护 alive
4. **7 种 Joint API 扩展** — Limit/Motor/Spring 共 14 fns
5. **5 种新 Joint 类型 + CreateXxxJoint** — 共 32 fns (+ 5 World fns)
6. **Body 邻居 + Fixture.RayCast** — 3 fns
7. **Body 高级属性** — 10 fns
8. **World 仿真控制** — 4 fns
9. **smoke 扩展** — 复用 `physics.lua`, 追加 14/15 阶段 (Phase AP 专属)
10. **本地 Release 编译 + smoke 验证**
11. **commit + push + 6 平台 CI 验证**

## 技术约束 (不变量)

- **单位**: 像素 / 弧度 / 秒 (与现有绑定一致)
- **Handle 模式**: wrapper struct + `__joint` field + selfRef + userData.pointer 反向链接
- **生命周期**: 复用 Phase AO 的 DestructionListener
- **CI**: 6 平台 build-templates.yml 同入口

## 验收清单

- [ ] 71 fns 全部注册到对应 table
- [ ] Phase AO smoke 13 阶段仍全绿
- [ ] Phase AP smoke ~14 阶段全部通过
- [ ] 本地 Release build pass
- [ ] 6 平台 CI all success
- [ ] 文档: ACCEPTANCE_PhaseAP.md + TODO_PhaseAP.md
