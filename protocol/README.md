# protocol

桌面界面、作业管理层与求解内核的契约源文件，由架构组维护。

- `chipcae/run/v1`：`chipcae.run.v1` 内核事件与运行控制（契约 v0.3）。
- `chipcae/jobs/v1`：`chipcae.jobs.v1` 作业管理服务。
- 包名必须包含大版本号；已发布字段号不得复用；删除字段必须用 `reserved` 保留。
- `errors.yaml` 只追加：已发布 code/name 不得修改或删除，废弃使用 `deprecated: true`。新条目只能追加到末尾，code 严格单调递增。CI 对比基线检查兼容性，已有条目可添加元数据。
- 只有 `CHIPCAE_ENABLE_GRPC=ON` 时才进行 gRPC 代码生成；这些 proto 目前是协议源文件。

## Event log v1 二进制线格式

实现以 [`io/include/chipcae/io/event_log.hpp`](../io/include/chipcae/io/event_log.hpp) 的注释为准：

> Frozen v1 layout: each record is "CAELOG1\0", le32 body length (max 64 MiB), le32 CRC-32 (reflected polynomial 0xEDB88320, init/final XOR 0xFFFFFFFF, covering the body), then the body: le64 seq, le64 signed wall_ns, uint8 kind equal to the RunEvent oneof field number, then fields in proto field order. Numeric fields are little-endian, strings have le32 byte lengths, repeated fields have le32 element counts; Plugin contains key then version strings. Reads are inclusive by seq and stop at an incomplete or CRC-mismatched tail. The writer flushes without fsync: an accepted MVP tradeoff. A crash may lose flushed but non-durable records; reopening truncates a torn tail. A dropped tail is first copied to <path>.corrupt-<offset>, or <path>.corrupt-<offset>-<n> if that name already exists. The library does not rotate or delete these files; the operator is responsible for cleaning them up.

`tests/protocol/fixtures/event_log_v1.bin` 是 writer 生成的 golden 样例；旁边 JSON 描述有效前缀及故意损坏的尾部。

## 运行控制文件

提交运行时界面创建空的 `control.jsonl`；文件不存在时内核按无命令处理。运行目录中的 `control.jsonl` 是 `chipcae.run.v1.ControlCommand` 的 proto3 JSON 映射，每行一条命令。界面仅追加写，内核只读；`cmd_seq` 从 1 开始并严格递增，内核忽略已处理（`cmd_seq` 不大于最后处理值）的命令。内核在每个全局窗口边界检查一次；稳态在每次外迭代或每个物理场完成后检查。每条命令都在事件日志写入 `RunEvent.control_ack`，其 `effective_step` / `effective_time` 指向生效前最后完成的窗口。暂停期间持续写 `State=PAUSED`；`STOP` 结束时写 `KernelExit(reason="user_stop")`。末尾尚未写完的 JSON 行在出现换行符前不处理。

格式非法的完整行写入 `LogLine(level=WARN)` 后跳过，且不推进已处理序号；无换行的末尾半行等待后续追加。
