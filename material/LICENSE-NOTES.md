# License notes

`chipcae_material` is proprietary to the chipcae project. Third-party dependencies:

| Dependency | Use | License family |
| --- | --- | --- |
| C++20 standard library | always | permissive (implementation) |
| Kokkos | parallelism | Apache-2.0 / BSD-style |
| nanobind | optional Python bindings | BSD-3-Clause |
| Python / NumPy | optional bindings & tests | PSF / BSD |
| scipy | test reference only (future) | BSD-3-Clause |
| Boost.Math | test reference only (future) | Boost Software License |

No copyleft-only (GPL) dependency is used. The dependency allowlist in
`cmake/dependencies.allowlist` is unchanged.
