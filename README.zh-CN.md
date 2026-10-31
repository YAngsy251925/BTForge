# BT Forge

**面向 Unreal Engine 的确定性行为树 / 黑板 / EQS 构建器，通过 MCP 供 AI agent 驱动。**

BT Forge 把 LLM（或人）写出的 JSON 规格变成真实、可编辑的 UE 资产：

- **行为树 + 黑板**：复合节点、任务、服务、装饰器、黑板键引用、资产引用、项目蓝图节点类。
- **环境查询（EQS）**：Option、原生 generator/test、评分、context、项目类路径。
- **校验与发现**：带精确 JSON 路径的空跑校验；能力目录让 agent 使用工程里真实存在的类，而不是猜路径。

引擎目标：**UE 5.4 – 5.8**，零版本条件编译。已实测 **5.5**（空白工程）与 **5.8**（Lyra；自研 server 与 Epic 官方 MCP server 双端点）。M3 运行层验证（in-process PIE 真跑 + EQS 实例查询 + 机器可读断言）已于 2026-10-29 落地。

> English documentation: [README.md](README.md).

---

## 为什么做这个

行为树资产通常靠在编辑器里手工搭。这对人来说慢、难 review，对 AI agent 更不可靠。运行时 LLM 直接驱动 NPC 也不合适（延迟、不确定、战斗逻辑必须可设计可审查）。

本项目面向的工作流：

```
设计文档 / 自然语言需求
        │  （LLM 参考能力目录，写出规格）
        ▼
   JSON 规格 ──► BT Forge ──► 真实的 .uasset（BT / BB / EQS）
        ▲                        │
        └── inspect / validate ◄──┘        （带精确 JSON 路径的反馈闭环）
```

Agent 负责**规划与写规格**；确定性写入器负责**建资产**；校验器/读回器负责**判断对错**。这样生成出来的 AI 可审查、可进 git diff、可复现。

---

## 功能

### 13 个 MCP 工具

| 工具 | 作用 |
|---|---|
| `create_behavior_tree_asset` | 创建黑板 + 行为树资产对 |
| `build_behavior_tree` | 按 JSON 规格写入行为树数据链 |
| `inspect_behavior_tree` | 读回树结构 + 标量参数 |
| `validate_behavior_tree` | 空跑校验树 + 黑板规格，不创建资产 |
| `create_eqs_asset` | 按 JSON 规格创建 EQS（`UEnvQuery`）资产 |
| `inspect_eqs_asset` | 读回 EQS 的 Option/Generator/Test/参数 |
| `validate_eqs_spec` | 空跑校验 EQS 规格，不创建资产 |
| `get_capability_catalog` | 可用 BT 节点类、EQS 资产/context、规格别名与参数 JSON 形状清单 |
| `inspect_references` | 引用/内容域策略：每个引用的域与能否进 cook（资产模式与规格模式） |
| `create_node_blueprint` | 生成蓝图节点资产（编译出的 `_C` 类 + CDO 默认值），可选 `RecipeJson` 合成受限事件图；返回的类路径可直接作为 `build_behavior_tree` 的节点 `type` |
| `start_verify` | 运行层验证：起一个 PIE 会话，真正跑一遍生成的 BT/EQS 资产 |
| `get_verify_result` | 轮询该会话：`pass` + `checks[]` + 执行时间线 + 终态 |
| `repair_graph` | 丢弃陈旧的编辑器图，让行为树编辑器下次打开时从数据链重建（无头 `build_behavior_tree` 之后的维护步骤；资产在编辑器中打开时拒绝执行） |

### 规格覆盖范围

- **BT 节点**：复合 `Sequence`/`Selector`；任务 `Wait`、`MoveTo`、`RunEQS`；服务 `RunEQS`；装饰器 `Blackboard`。此外接受任意非抽象 `UBTCompositeNode` / `UBTTaskNode` / `UBTService` / `UBTDecorator` **类路径**——包括项目蓝图类，如 `/Game/AI/BTS_Shoot.BTS_Shoot_C`。
- **键数据流**（`validate_behavior_tree` / `build_behavior_tree`）：反射读出每个节点的黑板键绑定并分类，报告死门（被读但无人写）、孤儿写者、写者作用域与 `FlowAbortMode` 问题；外部喂的键用 `externalKeys` 声明（树根或 BlackboardJson），项目节点用 `nodeEffects` 声明读写。
- **黑板键类型**：Bool、Int、Float、String、Name、Vector、Rotator、Object。`SelfActor` 由 `UBlackboardData` 自动创建，规格里不要声明（声明会被拒绝）；节点仍可正常引用它。
- **EQS generator**：`SimpleGrid`、`CurrentLocation`、`OnCircle`、`Donut`、`Cone`、`PathingGrid`、`ActorsOfClass`、`PerceivedActors`（也接受任意非抽象 `UEnvQueryGenerator` 类路径）。
- **EQS test**：`Distance`、`Dot`、`Trace`、`Pathfinding`、`Random`、`Overlap`、`Project`、`Volume`（也接受任意非抽象 `UEnvQueryTest` 类路径）。
- **参数写入**走 Unreal 反射：未知参数名会报错并列出合法项；枚举校验并列出全部合法值；类引用会加载并做类型校验；`FAIDataProvider*` 这类包装 struct 允许直接写数字，降低规格出错率。

### 设计约束

- **先校验再创建**：规格先构建到 transient 对象里全量校验；任何错误都不会落盘（若已创建资产会删除）。
- **错误可行动**：`{"ok":false,"error":"<JSON 路径>: <原因>"}`，agent 可以据此直接修规格。
- **只写数据**：BT/EQS 的编辑器图故意留空，由引擎在打开资产时自动补图。这样写入器不依赖任何图编辑 API 和易变的编辑器代码。
- **双 MCP 端点**：任意版本可用自研 HTTP server（`:8090`）；5.8+ 可额外注册到 Epic 官方 `ModelContextProtocol` server（`:8000`）。
- **零 Epic 源码**：MCP 层按公开 MCP 规范原创实现；官方 Experimental 插件源码不拷贝、不再分发。

---

## 框架

### 仓库结构

```
BTForge/                      ← 插件：UE 5.4 – 5.8
├── BTForge.uplugin
├── Source/
│   ├── BTForgeRuntime/       Runtime：被生成资产引用的节点/context 类
│   ├── BTForgeCore/          Editor：BT/BB/EQS 写入器、能力目录、校验
│   └── BTForgeMCP/           Editor：自研 MCP server（JSON-RPC 2.0 over HTTP）
└── Tools/                    smoke 测试（python）

BTForgeOfficial/              ← 插件：仅 5.8+
└── Source/BTForgeOfficial/   适配层：通过 ToolsetRegistry 注册同一批工具
```

### 模块关系

```
                 仅 5.8
        ┌─────────────────────────┐
        │ BTForgeOfficial         │───► Epic ModelContextProtocol (:8000)
        │ (UToolsetDefinition)    │
        └──────────┬──────────────┘
                   │ 三行委托
┌──────────────────▼──────────────┐
│ BTForgeCore (Editor)            │  规格 → 资产；读回；校验；能力目录
└───────┬─────────────────┬───────┘
        │                 │
┌───────▼───────┐  ┌──────▼─────────────┐
│ BTForgeMCP    │  │ BTForgeRuntime     │  被生成资产引用的类
│ (:8090)       │  │ (Runtime 模块)     │  （打包后必须存在）
└───────────────┘  └────────────────────┘
```

**为什么拆 Runtime 模块？** 生成的 BT/EQS 资产会在 cook 时硬引用节点/context 类。只放在 Editor 模块里的类，PIE 能跑，打包后就会丢。正确判据是"资产引用了什么类"（Runtime），而不是"谁创建资产"（Editor）。

**为什么拆两个插件？** 5.8 官方 toolset API（`UToolsetDefinition`、`ToolsetRegistry`、`ModelContextProtocol`）在旧引擎不存在，且官方插件是 `NoRedist`。拆成独立适配插件后：旧引擎不装即可，主插件保持零依赖、可公开分发。

### 写入方式

- **BT/BB**：写 `UBehaviorTree::RootNode`、`UBTCompositeNode::Children`、黑板 `Keys`、服务/装饰器数组。打开资产时 `UBehaviorTreeGraph::SpawnMissingNodes` 从数据链自动补图。
- **EQS**：写 `UEnvQuery::Options` 及内联的 Option/Generator/Test 对象。打开资产时 `UEnvironmentQueryGraph::SpawnMissingNodes` 从 `Options` 重建图。
- **参数**：反射写入器覆盖数字、布尔、字符串、Name、枚举、嵌套 struct、类引用、对象引用，带类型校验与精确报错。

### 工作流

```
自然语言 / 设计文档
   → LLM 调 get_capability_catalog（拿到真实类路径与参数 JSON 形状）
   → LLM 写规格
   → validate_*（不落盘）或 create_* + build_*
   → inspect_*（核对写入结果）
   → [路线图] PIE 运行层验证
   → 行为树被游戏内 AI Controller 使用
```

---

## 快速开始

### 1. 安装插件

把 `BTForge/` 拷到 `<你的工程>/Plugins/BTForge/`；UE 5.8 再额外把 `BTForgeOfficial/` 拷到 `<你的工程>/Plugins/BTForgeOfficial/`。

在插件窗口启用 **BT Forge**（5.8 上再启用 **BT Forge Official**）。

### 2. 启动 MCP server

控制台命令：

| 命令 | 作用 |
|---|---|
| `BTForge.MCP.StartServer [port]` | 启动自研 server（默认 `8090`） |
| `BTForge.MCP.StopServer` | 停止 |
| `BTForge.MCP.Status` | 查看状态 |

可选自启动，写入 `<工程>/Saved/Config/WindowsEditor/EditorPerProjectUserSettings.ini`：

```ini
[BTForgeMCP]
bAutoStartServer=True
ServerPort=8090
```

### 3. 配置 MCP 客户端

```json
{
  "mcpServers": {
    "btforge": {
      "type": "http",
      "url": "http://127.0.0.1:8090/mcp"
    }
  }
}
```

5.8 装 `BTForgeOfficial` 后，同一批工具也会出现在 Epic 官方端点 `http://127.0.0.1:8000/mcp`。

### 4. 验证

```bash
python BTForge/Tools/mcp_smoke.py             # BT 基础套件
python BTForge/Tools/mcp_smoke.py --l2        # + MoveTo/RunEQS 写路径
python BTForge/Tools/mcp_m1m2_smoke.py        # EQS 构建 + 能力目录 + 空跑校验（35 项）
python BTForge/Tools/mcp_m1m2_smoke.py --official  # 同一套件走 Epic 官方 :8000
python BTForge/Tools/mcp_m3_smoke.py          # PIE 运行层验证
python BTForge/Tools/mcp_m4_goldenpath.py     # M4 黄金路径（catalog → 引用 → PIE）
python BTForge/Tools/mcp_m5_nodes.py          # M5 原生节点库（catalog → 规格 → 8 个 PIE run）
```

---

## 规格语法（简版）

### 行为树

```json
{
  "type": "Selector", "name": "Root",
  "children": [
    {"type": "Sequence", "name": "Patrol", "children": [
      {"type": "RunEQS", "name": "FindGoal",
       "params": {"BlackboardKey": "MoveGoal",
                  "EQSRequest": {"QueryTemplate": "/Game/AI/EQS_FindWeapon",
                                 "RunMode": "SingleResult"}}},
      {"type": "MoveTo", "params": {"BlackboardKey": "MoveGoal", "AcceptableRadius": 100}},
      {"type": "Wait", "params": {"WaitTime": 3.0, "RandomDeviation": 1.0}}
    ]}
  ]
}
```

服务与装饰器按结构挂接：

```json
{"type": "Sequence", "services": [{"type": "RunEQS", "params": {"EQSRequest": {"QueryTemplate": "/Game/AI/EQS_AIPerceptionEnemy"}}}],
 "children": [{"type": "MoveTo", "decorators": [{"type": "Blackboard", "params": {"BlackboardKey": "TargetEnemy"}}]}]}
```

### EQS

```json
{
  "options": [{
    "optionName": "FindCover",
    "generator": {"type": "SimpleGrid",
                  "params": {"GridSize": 1500, "SpaceBetween": 200}},
    "tests": [{
      "type": "Distance",
      "params": {"TestPurpose": "Score",
                 "ScoringEquation": "InverseLinear",
                 "ScoringFactor": 1.0,
                 "DistanceTo": "/Script/AIModule.EnvQueryContext_Querier"}
    }]
  }]
}
```

实践中的关键点：

- `TestPurpose` 默认是 `FilterAndScore` + `Range[0,0]` 过滤——纯评分测试必须显式写 `"TestPurpose": "Score"`，否则所有 item 都会被过滤掉。
- 参数名就是 UPROPERTY 精确名。先调 `get_capability_catalog` 可看到参数名、C++ 类型和 **JSON 形状**（`boolean`、`number`、`enum: ...`、`class path (...)`）。
- EQS 的 context 是**按类引用**而不是按实例。`BTForgeRuntime` 提供 `BTForgeEnvQueryContext_Blackboard`（`/Script/BTForgeRuntime.BTForgeEnvQueryContext_Blackboard`），其 `BlackboardKey` 取类默认值（按 query 配置键需要蓝图子类，见路线图）。

### 原生节点库（M5）

`BTForgeRuntime` 内置 7 个参数化原生节点——过去需要写蓝图才能有的常用行为。它们带完整参数 schema 出现在 `get_capability_catalog` 里，规格里可用短别名：

| 别名 | 类 | 作用 |
|---|---|---|
| `ComputeValue`（任务） | `BTForgeTask_ComputeValue` | `LeftKey` <op> （`RightKey` 或 `RightConstant`）→ `ResultKey`；Add/Subtract/Multiply/Divide/Min/Max；Float/Int 键 |
| `Log`（任务） | `BTForgeTask_LogMessage` | 向 `LogBTForge` 写检查点消息（可附带一个键值）；`Severity=Error` 会让 verify 的 `no_log_errors` 如实失败，`Warning` 出现在 `logWarnings` |
| `SetValue`（服务） | `BTForgeService_SetBlackboardValue` | 周期性按类型写值：各类型常量，或从兼容的 `SourceKey` 复制 |
| `UpdateDistance`（服务） | `BTForgeService_UpdateDistance` | 周期性算 `dist(SourceKey, TargetKey)`（Object-Actor 或 Vector 键）→ Float `ResultKey` |
| `FloatCompare`（装饰器） | `BTForgeDecorator_FloatCompare` | `BlackboardKey` <op> （`CompareToKey` 或 `CompareToValue`），六种运算符；观察键变化并中止分支 |
| `DistanceCheck`（装饰器） | `BTForgeDecorator_DistanceCheck` | 距离范围门 `dist(SourceKey, TargetKey)` ∈ `[MinDistance, MaxDistance]`；负数边界=该侧不启用；两侧都不启用=失效关闭 |
| `Chance`（装饰器） | `BTForgeDecorator_Chance` | 以 `Probability`% 概率通过（0/100 精确） |

示例——持续刷新 `DistToTarget` 并用它门控攻击分支：

```json
{"type": "Selector", "services": [
   {"type": "UpdateDistance", "params": {"SourceKey": "SelfActor", "TargetKey": "TargetEnemy", "ResultKey": "DistToTarget"}}],
 "children": [
   {"type": "Sequence",
    "decorators": [{"type": "FloatCompare", "params": {"BlackboardKey": "DistToTarget", "Operator": "LessOrEqual", "CompareToValue": 800}}],
    "children": [{"type": "MoveTo", "params": {"BlackboardKey": "TargetEnemy", "AcceptableRadius": 150}}]},
   {"type": "Log", "params": {"Message": "target out of range", "Severity": "Warning"}}]}
```

引擎自带节点（5.6+ 的 Set-key 任务家族支持键绑定写法 `{"Value": {"Key": "SomeKey"}}`）与蓝图节点类出现在同一个目录里，可与这些别名自由混用。

### 蓝图节点生成（M6）

`create_node_blueprint` 按规格生成**数据-only 的蓝图节点资产**：一个派生自 BT 节点基类的、已编译的 `/Game/..._C` 类，项目级默认值写在类 CDO 上——不写蓝图、不生成事件图。BTForge 节点类都标了 `Blueprintable`，天然适合做父类：

```json
{"AssetPath": "/Game/AI/BTA_LogShot",
 "ParentClassPath": "/Script/BTForgeRuntime.BTForgeTask_LogMessage",
 "ParamsJson": "{\"Message\":\"shot fired\",\"Severity\":\"Warning\"}"}
```

返回的 `classPath` 可直接作为 `build_behavior_tree` 的节点 `type`，能力目录自动收录新类，PIE 验证证明 CDO 默认值真实驱动运行时行为。引擎的 `*BlueprintBase` 也是合法父类；引擎原生 BT 类不可 `Blueprintable`，会被拒绝并给出解释。

**事件图配方**（`RecipeJson`）更进一步：直接合成可运行的 K2 事件图。配方 `"call_function"` 在节点的主事件上（任务 `ReceiveExecuteAI`、服务 `ReceiveActivationAI`）生成一个函数调用——目标对象取自事件自带的 `OwnerController`/`ControlledPawn` 引脚——调用 AI 控制器或其 pawn 上零参数 BlueprintCallable 函数，任务随后自动以成功结束：

```json
{"AssetPath": "/Game/AI/BTA_Reload",
 "ParentClassPath": "/Script/AIModule.BTTask_BlueprintBase",
 "RecipeJson": "{\"recipe\":\"call_function\",\"target\":\"controller\",\"function\":\"ForceNetUpdate\"}"}
```

图形状固定、创建期全量校验（函数须存在于 `AAIController`/`APawn`、BlueprintCallable、非纯、零参数）；失败即删资产、不留残骸。PIE 验证证明合成图端到端执行。

---

## 开发进度

| 里程碑 | 状态 | 说明 |
|---|---|---|
| v0 — 官方 ToolsetRegistry 版 BT 构建器 | ✅ 2026-10-20 | 5.8 上 3 个工具 |
| 跨版本架构定稿 | ✅ 2026-10-23 | 自研 MCP server；`BTForge` + `BTForgeOfficial` 双插件；目标 5.4–5.8 |
| UE 5.5 实测 | ✅ 2026-10-24 | 空白工程零改动编译；挖出两个 5.8 被掩盖的 bug（启动时序、HTTP listener 全局开关） |
| L1 — Inspect 读侧补全 | ✅ 2026-10-27 | 服务/装饰器/struct 读回；反射指针算术的崩溃取证 |
| L2 — 写侧扩容 | ✅ 2026-10-27 | `MoveTo`、`RunEQS`；键/引用/枚举/struct/数组写入器 |
| L3 — 服务与装饰器挂接 | ✅ 2026-10-28 | `Blackboard` 装饰器、`RunEQS` 服务；空 ops = AND 的引擎原生路径 |
| L3.5 — 蓝图节点类 | ✅ 2026-10-28 | `type` 接受 `/Game/..._C` 与 `/Script/...`；基类/抽象校验 |
| **M1 — EQS 构建器** | ✅ 2026-10-28 | 按规格创建 `UEnvQuery`；Runtime 模块拆分；运行时黑板 context |
| **M2 — 能力目录 + 空跑校验** | ✅ 2026-10-28 | 能力目录、BT/EQS 空跑校验；工具总数 8 |
| **M3 — 运行层验证** | ✅ 2026-10-29 | 进程内 PIE：`StartTree(SingleRun/Looped)` + 执行时间线采样；EQS instant query 断言；两段式异步协议；2.2.1 补齐 `pass` 聚合、PIE 启动语义、EQS 有效项计数、夹具 Pawn 等修复 |
| **M4 — 黄金路径端到端** | ✅ 2026-10-30 | `inspect_references`（引用/内容域策略，资产+规格两种模式）、`pawnDisplacement`/`expectedMinPawnDisplacement`、`Tools/mcp_m4_goldenpath.py`：catalog → 规格 → 资产 → 引用 → PIE，含坏规格→修复回路与可选导航图运动相位；工具 11 个 |
| **M5 — 原生节点库** | ✅ 2026-10-30 | `BTForgeRuntime` 内 7 个参数化原生 service/decorator/task（ComputeValue、Log、SetValue、UpdateDistance、FloatCompare、DistanceCheck、Chance）+ 规格别名（别名表即 catalog 权威源）；`Tools/mcp_m5_nodes.py` 在 PIE 里逐行为验证；工具 11 个 |
| **M6 — 蓝图节点生成（上半）** | ✅ 2026-10-30 | `create_node_blueprint`：数据-only 蓝图节点资产（编译出的 `_C` 类 + CDO 默认值，无事件图）、精确的父类校验、catalog 自动收录、PIE 验证 CDO 行为；`Tools/mcp_m6_bp.py`；工具 12 个 |
| **M6 — 蓝图节点生成（下半）** | ✅ 2026-10-30 | 受限事件图配方：`RecipeJson` 在 `create_node_blueprint` 里合成固定形状的 K2 图（AI 事件 → 控制器/pawn 上零参函数调用 → 任务自动收尾），创建期校验、PIE 端到端验证 |
| 蓝图配方 — 扩展面 | ⏳ 规划 | 常量参数连线、装饰器条件图（函数返回值）、项目类 Cast |

### 当前版本验证矩阵（v2.6.1）

| 环境 | 结果 |
|---|---|
| UE 5.8，自研 server（`:8090`） | BT 基础套件 PASS；M1/M2 套件 **35/35 PASS**；M3 运行套件 **34/34 PASS**；M4 黄金路径 **PASS**；M5 节点库套件 **PASS**；M6 蓝图生成套件 **PASS** |
| UE 5.8，官方 MCP server（`:8000`） | M1/M2 套件 **35/35 PASS**；M3 运行套件 **34/34 PASS**；M4 黄金路径 **PASS**；M5 + M6 套件 **PASS**；12 个工具在官方 toolset 中注册成功 |
| UE 5.5，空白工程 | 零源码改动编译通过；M1/M2 套件 **35/35 PASS**；M3 运行套件 **34/34 PASS**；M4 黄金路径 **PASS**；M5 + M6 套件 **PASS** |
| UE 5.4 / 5.6 / 5.7 | 未实测（API 按稳定性选型、无版本条件编译） |

---

## 已知限制

- **EQS context 是按类引用**（`TSubclassOf<UEnvQueryContext>`）。要给原生 context 按 query 配置参数（如 `BlackboardKey`），需要带类默认值的蓝图子类；该子类生成器仍是后续工作（M6 的蓝图生成只覆盖 BT 节点资产）。
- **EQS 参数数组暂不可写**（如 `Composite` generator 的子 generator）；非空数组会被明确拒绝。
- **蓝图节点类能创建、但配方面很窄**：`create_node_blueprint` 生成数据-only 节点 + `call_function` 受限配方；自由事件图（常量参数连线、装饰器条件图、项目类 Cast）仍属后续工作。
- **运行层验证的边界**（M3 已实现）：`start_verify` 需要一个开着编辑器的进程，且一次只跑一个会话；已有 PIE/SIE 在运行时会被拒绝（BTForge 只关自己开的会话）。`bareGameMode` 下没给 `mapPath` 会退到引擎模板图（项目图如 Lyra 在裸 GameMode 下会断言）。`fixturePawn` 默认给一个 `ADefaultPawn`，但导航类任务仍需目标图带 NavMesh（否则 `MoveTo` 只会报"0 items/failed to find path"）。EQS 验证是同步一次性 step 循环，不覆盖异步 generator。
- **引用策略的判定边界**（M4 已实现）：`inspect_references` 对引擎内置模块用一份**明示的编辑器模块清单**判定（内置模块没有运行时可读的宿主类型），插件模块与**项目模块**则读各自的描述符（`Type=Editor` / `TargetAllowList` 不含 `Game`）；另外用 cooker 自己的 `IsEditorOnlyObject` 兜底。`/Engine` 内容只给 `warn`。单次扫描上限 600 条——超出时报告 `summary.truncated` 并把 verdict 降级，不再静默截断。
- **项目图上的 pass 可能被项目自身日志拖成 false**：`no_log_errors` 会把 PIE 期间项目 GameFeature/蓝图报的错误算进来（实测 Lyra 的 GCNL_Spawning 脚本错误）。M4 黄金路径因此在项目图上只断言"运动证据"（`pawn_moved`），并把项目噪声如实报告出来，而不是过滤掉。
- **`inspect_behavior_tree` 只回标量参数**：结构体里的对象引用（如 `EQSRequest.QueryTemplate`）由 `inspect_references` 以精确 JSON 路径报告。
- **引擎 Set-key 任务家族是 5.6+ 才有的**：冒烟套件不能无条件引用它（M5 套件先探 catalog、没有就跳过对应 interop 控制项）。
- **能力目录里蓝图节点没有参数 schema**（只有原生类有），避免逐个加载蓝图 CDO。
- **未挂载的 GameFeature/插件内容可能不在目录里**（AssetRegistry 只能看到已挂载内容；工具输出里有 note 提示）。
- **5.4/5.6/5.7 未实测**：开发机只有 5.5 与 5.8。

---

## 许可证

[MIT](LICENSE) — 可用于商业与个人项目。
