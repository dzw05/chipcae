# D02 → D12 事件适配契约

架构师增补 2 为准：**CAELOG1 + CRC-32/IEEE，event_log_format=1 是正式线格式**。
读取器遇到 event_log_format≠1 必须明确拒绝“不支持”，不可猜测或静默降级。
`mock_solver.require_format/read_log` 覆盖此拒绝。

## 正式记录

与 `io/include/chipcae/io/event_log.hpp` frozen v1 注释一致，每条记录：

|字节|含义|
|---|---|
|0..7|ASCII CAELOG1 后接 NUL，每条记录都有 magic|
|8..11|小端 uint32 body 字节长度，上限 64 MiB（单记录，不是分段）|
|12..15|小端 uint32 CRC-32/IEEE，覆盖 body|
|16..|小端 uint64 seq、int64 wall_ns、uint8 kind，再按 proto 字段顺序写值|

CRC 反射多项式 0xEDB88320，初/终 XOR=0xFFFFFFFF；Python zlib.crc32 与其一致。
数值小端；字符串 uint32 字节长度+UTF-8；重复字段 uint32 元素数；Plugin 为
key/version 两字符串。不是 protobuf 二进制 wire。无 CRC32C/分段验收项。

现有 C++ 支持 kind 15 Hello、10 Progress、11 Residual、20 LogLine、21 State、
22 OutputAvailable。D02 Python codec 仅实现后五类，未知 kind 明确拒绝；
D12 正式 adapter 仍须实现 Hello 全字段，并检查其中 event_log_format。
同一事件序号在所有输出中一致；正式日志因 mock-only 事件可有序号间隙。
读取 inclusive seq，validated offset 仅在完整 CRC 正确且 payload 解码成功后推进。
半 header/body、超长、CRC 错误均停在尾部，不越过坏记录继续解析；magic 错误报错。
C++ writer 重开会先将坏尾备份 `.corrupt-<offset>[-n]` 再截断，flush 无 fsync；
Python fixture reader 不执行恢复或修改文件。

## Mock 边界

`mock-events.jsonl` 是明确的测试通道，不是新的 event_log_format，也不能拼入
正式 CAELOG1。它覆盖 State、Progress、Residual、ResidualBatch、LogLine、
OutputAvailable、ControlAck、KernelExit（oneof key `exit`）。当前 C++ variant
缺后三种中的 ResidualBatch/ControlAck/KernelExit，D02 不擅自定义其二进制编码。
JSONL 使用 proto 字段拼写，uint64/int64 序号为字符串；State 值为 proto 枚举数字。
mock 的 LogLine.level=1 仅作本地 info 数值，不宣称真实内核等级映射已冻结。
OutputAvailable 只针对 manifest 已登记且 complete 的 step；运行目录无 manifest
时不伪造输出可用事件。

`events/{valid,half,bad-crc}.caelog` 由同一确定性记录构成，固定 wall_ns。
fixture.json 给出最后有效记录边界；坏 CRC fixture 翻转第二记录末字节，
半记录 fixture 截去第二记录末 7 字节。测试独立按 struct/zlib 验证首记录。
这些为可重复 mock fixtures；B3 现有 C++ writer golden 尚待外部交付，
不冒称已完成跨语言 golden 联验。

## 控制文件

`<run_dir>/control.jsonl`；mock 不覆盖已有命令。每行 ControlCommand proto3 JSON，
例如 `{"cmd_seq":"23","client_request_id":"stop-demo","action":"STOP"}`。
接受 proto 原字段名及标准 camelCase。cmd_seq 为正 uint64 字符串，生产者从 1
严格递增；mock 接受缺失序号（例如从 23 开始）以测回执关联。
缺文件无命令；无换行尾行等待且不报警。非法完整行警告、跳过字节，
不推进已处理 seq；seq≤已处理 seq 忽略；后续合法行继续处理。
窗口完成边界才处理 STOP，ControlAck 的 cmd_seq/client_request_id 原样匹配，
status=APPLIED，effective_step/time 为最后完成窗口；KernelExit.exit_code=0、
reason=user_stop、State=COMPLETED。其他合法动作在 mock 中回 REJECTED。
真实 C3 控制文件及内核联验属于外部交付，本卡仅 mock 验证。
