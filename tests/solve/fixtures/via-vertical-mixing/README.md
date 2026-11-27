# via-vertical-mixing 输入夹具（2026-11-23 简报冻结）

本目录冻结 solve-via-vertical-mixing 的输入，不是输出期望。实现卡把同一内容写入仓库 `tests/solve/fixtures/via-vertical-mixing/`，测试必须读该目录，不得在测试源里另写一套材料或几何。
oracle 闭式仍在测试内独立实现，只从这里读输入。

修订一增补（2026-11-23 20:20 Asia/Shanghai）：`levin_cavity`（Levin 空腔极限两档）、`opening_ereg`（E_reg/E_mask 两档）与 `robustness.soft_inclusion`（软夹杂 HS 上界）。测试必须读这些字段，不得在源里另写一套数。
