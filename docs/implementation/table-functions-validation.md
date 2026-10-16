# 表函数迁移验证记录

日期：2026-10-17。状态：验收进行中，PR 保持 draft。基线为 `a8afc89e9`；首个完整切换版本为 `1da34c7b0`，后续修复构建为 `8c040e5d7`。两个 release 构建均通过，依赖和 Cargo 使用现有本地缓存。

## 新实例对比

基线和切换实例均使用 `memory_budget=2G`、`cpu_count=4`、`datafile_size=512M`、`log_disk_size=2G`，在独立空目录启动。按迁移清单的全部 143 个旧 ID 查询，而非按名称前缀估算。

| 指标 | 基线 | 完整切换后 |
| --- | ---: | ---: |
| 旧 VT 的 `__all_table` 行 | 143 | 0 |
| 旧 VT 的 `__all_column` 行 | 1664 | 0 |
| 旧 VT 的 `__all_table_history` 行 | 143 | 0 |
| 旧 VT 的 `__all_column_history` 行 | 1664 | 0 |
| 旧 VT 的 `__all_ddl_operation` 行 | 143 | 0 |
| 五类元数据合计 | 3757 | 0 |
| 可绑定的旧 VT 名称 | 143 | 0 |
| 系统视图 | 139 | 139 |
| schema 分配器 `mem_used` 快照 | 1,240,016 B | 1,019,904 B |
| schema 分配器 `mem_total` 快照 | 2,079,744 B | 2,079,744 B |
| SSTable 实际已用宏块 | 8 MiB | 8 MiB |
| SSTable 文件预分配 | 512 MiB | 512 MiB |
| redo 文件 | 64 MiB | 64 MiB |

schema 已用内存快照减少 220,112 B，预留内存没有变化；管理器数量和启动查询会影响快照，不将其当作精确的进程 RSS 净收益。LocalStore 数据文件为 311,296 B 与 307,200 B，相差一个页，包含真实本地系统数据，不能归因于 VT 元数据消除。日志文件不用于计算收益。已消除持久化记录不等于在当前小实例中减少了 SSTable 宏块或预分配文件尺寸。

新注册信息也有成本：当前构建 `sizeof(TableFunctionDescriptor)=120`、`sizeof(TableFunctionColumn)=48`，143 个固定函数及 1664 个列描述共 97,032 B，另有定位数组、静态初始化 guard 和字符串。生成定义中去重后的字符串字面量共 27,559 B，链接器还可能与程序其他字符串合并；这不是额外堆分配量。四个通用入口另有自己的少量定义。注册表持有程序生命周期内的定义指针，不创建持久化函数或表 schema。

## 已执行的功能检查

- 143 个函数逐一 `SELECT * ... LIMIT 0`：列名、顺序、协议类型与旧表一致；逐一 `DESCRIBE TABLE FUNCTION name`：列名和数量与 SELECT 一致。
- 143 个函数逐一有限扫描：137 项成功。其余六项在两端返回相同错误：三个基线内部错误、未创建的过程和触发器、无效的 `tx_data(0)` 定位参数。不存在迁移后新增错误。
- 139 个视图逐一 `LIMIT 1`：两端均 138 项成功。`V$OB_PX_TARGET_MONITOR` 在基线和新版本均返回 4016。
- `SHOW TABLE FUNCTIONS` 返回 147 个签名；固定输出和调用描述、UNNEST、GENERATOR 及 JSON_TABLE 的基础查询已运行。
- 实际构建的 `sql_tests` 中 9 项 PL 编码和 5 项集合扫描测试全部通过，包含标量编码缓冲区、空/NULL/删除槽、记录成员、嵌套值、zip、投影顺序、重扫及提前关闭。
- 四连接共 1500 次操作验证 GENERATOR 状态隔离、LocalStore 有限扫描、系统配置有限扫描、关联 LEFT JOIN 重扫及无效参数失败路径。长 GENERATOR 查询可取消，返回 1317 后连接继续执行成功。取消响应约 2.5 ms，仅代表该次本地测量。
- SHOW、DDL、普通用户权限和对象可见性已执行回归；系统信息函数受保护，公共 INFORMATION_SCHEMA 提供方保留对象过滤。普通用户 SHOW PROCESSLIST 可见其他会话是基线既有行为，单独记录，不能误报为迁移通过了更严格的会话隔离要求。

实现中发现并修复的问题包括参数容器未初始化、PL 标量转换缺少 datum 缓冲区、PL typed NULL 集合/记录映射；嵌套 SQL 数组 LOB 标记和普通算术参数语法的修复正在集成。对应回归保存在现有单测和 mysqltest 体系中。

## 剩余验收

完整 mysqltest 结果、结构参数常量折叠及缓存限制、同名重载、协议输出路径、真实数据重启保持，以及最终代码审查仍需完成；当前记录不代表这些项目已经通过。

原始本地证据位于实施笔记目录：`baseline-instance.json`、`after-instance.json`、`provider-validation.json`、`cutover-show-permissions.json`、`lifecycle-validation.json`、`combined-unit-2.log` 和两次完整构建日志。迁移清单和测试源随 PR 提交；验收命令和最终结果会随实现继续更新。
