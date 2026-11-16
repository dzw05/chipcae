# protocol

桌面界面与求解内核之间的通信协议。由架构组维护。

- `chipcae/run/v1/*.proto`：作业启动与进度推送的 Protobuf 定义，包名为 `chipcae.run.v1`。
- `errors.yaml`：错误码表。启用 gRPC 后才会生成代码；未启用时本目录只保存协议源文件。
