# ADR-0007：STEP 导入采用 OCCT

- 状态：已接受
- 日期：2026-11-16

## 背景

STEP 导入需要成熟的几何内核。决策来源：owner 决定。

## 决策

STEP 导入使用 Open CASCADE（OCCT），遵循 LGPL，仅允许动态链接。

## 后果

发行时必须保留动态链接边界并履行 LGPL 义务。
