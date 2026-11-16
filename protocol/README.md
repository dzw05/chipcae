# protocol

桌面界面、作业管理层与求解内核的契约源文件，由架构组维护。

- `chipcae/run/v1`：`chipcae.run.v1` 内核事件与运行控制（契约 v0.3）。
- `chipcae/jobs/v1`：`chipcae.jobs.v1` 作业管理服务。
- 包名必须包含大版本号；已发布字段号不得复用；删除字段必须用 `reserved` 保留。
- `errors.yaml` 只追加：已发布 code/name 不得修改或删除，废弃使用 `deprecated: true`。新条目只能追加到末尾，code 严格单调递增。CI 对比基线检查兼容性，已有条目可添加元数据。
- 只有 `CHIPCAE_ENABLE_GRPC=ON` 时才进行 gRPC 代码生成；这些 proto 目前是协议源文件。
