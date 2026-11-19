# coupling

耦合器、场映射和 preCICE 适配器。

自研耦合层负责本仓库内部各求解器之间的场交换；preCICE 适配器用于与外部求解器对接。两种路径共享同一套场映射接口。

## 热温度传给结构求解

`map_thermal_to_mechanic` 接受同一套控制体的胞心温度与体积，返回逐控制体的
`dT[i] = T_i - T_ref`。温度、参考温度与温差单位均为 K，体积为 m³；
`temperature_unit` 只接受 `"K"`（默认值）或 `""`（也按 K），不做单位换算。
调用方必须保证热场、体积和结构输入的控制体身份及顺序一致；接口只能检查长度和数值，
不做跨网格插值或重排。输入 span 的存储须在调用期间有效。

```cpp
#include <array>
#include <chipcae/coupling/thermal_mechanic_map.hpp>

const std::array temperatures{300.0, 350.0};
const std::array volumes{1.0, 3.0};
chipcae::coupling::ThermalToMechanicInput input;
input.T_cell = temperatures;
input.volume = volumes;
input.T_ref = 300.0;
auto status = chipcae::coupling::map_thermal_to_mechanic(input);
if (status.code.code() == chipcae::ErrorCode::NONE) {
    // status.value.dT == {0, 50}; weighted T == 337.5; weighted dT == 37.5.
    // Pass status.value.dT to the structural solver in the same cell order.
}

// Uniform mode uses the same per-cell arithmetic and volume-weighted checks.
input.T_cell = {};
input.T_uniform = 400.0;
input.T_ref = 293.15;
status = chipcae::coupling::map_thermal_to_mechanic(input);
```

thermal 模式不设置 `T_uniform`，必须提供与体积等长、非空且有限的 `T_cell`；
均匀模式设置有限的 `T_uniform` 并保持 `T_cell` 为空。两种模式均要求有限的 `T_ref`，
以及非空、逐项有限且正、累加和有限且正的体积。

校验按体积、参考温度、单位、模式互斥、温度来源、温度算术的顺序进行。
缺失 thermal 温度返回已有的 `INPUT_REFERENCE_MISSING`（12），其他校验失败返回
`INPUT_INVALID`（14）。成功消息为空；失败消息是英文短句，`value.dT` 为空且两个平均值为 0，
不返回部分结果。输入校验通过状态返回，不主动抛异常；内存分配失败沿用标准库异常，
接口未声明 `noexcept`。

两个平均值分别按 `Σ T_i V_i / Σ V_i` 和 `Σ dT[i] V_i / Σ V_i` 计算。
每个运算独立舍入到 double，按输入索引顺序累加，不融合乘加、不使用更宽累加或均匀模式捷径。
随后逐步计算 `delta = volume_weighted_T - T_ref`、
`residual = volume_weighted_dT - delta`、`tolerance = 1e-14 * max(abs(delta), 1.0)`，
要求所有中间值有限且 `abs(residual) <= tolerance`。
温度算术溢出或有限舍入导致一致性不达标均以 `invalid temperature arithmetic` 失败；
下溢得到有限值本身允许，但仍须通过一致性检查。函数耗时 O(n)，输出空间 O(n)，无全局状态。
