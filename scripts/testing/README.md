# 本地 CMake / CTest 入口

`run_tests.sh` 从自身位置推导项目根目录，在独立的 `build-tests` 中配置、编译并运行 CTest。`BUILD_DIR` 和额外 CMake 参数仍按原接口传递。它不是源码检查器，也不会安装依赖；完整业务测试需要事先配置隔离数据库，不能默认连接正式实例。

项目根目录示例：`bash scripts/testing/run_tests.sh -DWEBSERVER_ENABLE_ASAN=ON`。原 `bash scripts/run_tests.sh` 保留兼容转发。

[脚本总览](../README.md)
