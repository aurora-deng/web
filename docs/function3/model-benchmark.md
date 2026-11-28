# 离线合成模型对比

本工具只使用明确指定的本地 ONNX 模型和内置合成题，不连接数据库、不激活版本、不启动业务服务、不下载模型或依赖。执行虚拟机测试仍需得到授权。不能仅凭本工具成功就切换默认模型。

## 题集和评分边界

- 固定版本 `synthetic-quality-20261127-v1`，分类正面 / 负面 / 中性各 8 题，另有中文闲聊 3 题、知识问答 3 题。
- 这是**新建的隔离合成题集**，不声称复用了历史数据库中的 24 道题，也不是训练数据。
- 分类仅按去除首尾空白、转小写后的完整标签精确评分；含解释的回答不算正确。
- 闲聊和知识回答必须另行逐题检查正确性、完整性、中文表达和危险建议。工具始终输出 `activationAllowed=false`。
- `long` 题请求至多 512 个输出 token；模型若提前 EOS（结束标记），报告实际 token ID 数，不把“上限 512”写成“实测 512”。
- 复用各模型已有生成配置，没有为追求好成绩单独调参；重复结果可能受模型默认采样设置影响，不能当作严格确定性质量基准。

## 编译与调用

已具备项目 ONNX 依赖的 Linux 隔离构建：

```sh
cmake -S /path/to/source -B /path/to/build
cmake --build /path/to/build --target phase11_onnx_benchmark_contract phase11_onnx_model_benchmark -j 2
ctest --test-dir /path/to/build -R '^phase11_onnx_benchmark_contract$' --output-on-failure
python3 scripts/function3/validation/model_benchmark.py   --binary /path/to/build/phase11_onnx_model_benchmark   --model-root /path/to/approved-models --model-dir relative-model-directory   --label model-a --suite quality --repeats 3 --cpus 2,3   --output /path/to/new-evidence-directory
```

调用前应确认既有 CMake 配置，不能为了上述命令安装依赖或重写正式构建参数。`--suite long` 单独测长输出。输出目录必须不存在，且不能在模型根内。

只生成可审查题集、不加载模型：

```sh
python3 scripts/function3/validation/model_benchmark.py --write-fixture /path/to/new-cases.jsonl
```

## 计时、资源和安全边界

1. C++ 工具不预调用 `validateVersion`，首个请求才装模型。`modelLoadPerformed=true` 的计时包含 Provider 内目录摘要复核和模型 / tokenizer 建立。
2. 独立记录启动前摘要计算、请求总耗时、客户端首次文本片段、Provider 预填充到首 token、decode 和队列耗时。首次片段不必等于第一个 token ID，因为特殊 token 可能没有可显示文本。
3. 之后的请求复用同一 Provider / 模型。热请求数和客户端首片段中位数单列；各题型汇总仍可能包含第一条冷请求，应查看原始行分离分析。
4. 未清空操作系统页缓存。第一次摘要读取本身会预热文件缓存，所以只能称“进程冷加载”，不能称“磁盘冷启动”。
5. `peakRssKiB`、用户 / 内核 CPU 秒是整个测量子进程的统计，不是每题采样，也不证明对普通聊天没有干扰。
6. CPU 集合必须在当前允许范围内、至多 4 核；运行 nice=5；可用内存不足 3.5 GB 拒绝运行。两个模型应串行执行，避免互相影响。
7. 环境仅白名单继承 PATH、动态库路径和 locale/TZ；不枚举或继承数据库、代理、云凭据和遥测变量。
8. 每题超时后先取消并等待 30 秒；整个子进程另有超时上限。异步回调共享持有状态，迟到 token / 重复完成不能写入已销毁对象或覆盖首个终态；输出有 64 KiB 上限。
9. 原始合成回答保留在 `raw.jsonl`，诊断留在隔离 `stderr.log`；终端报告不打印环境、密钥、真实聊天或 Provider 内部异常路径。

## 证据

每次生成 `cases.jsonl`、`raw.jsonl`、`stderr.log`、`report.json`。报告包含题集 / 二进制摘要、模型树摘要、CPU 约束、真实 token 指标、失败分类和已知限制。不覆盖失败证据，不与其他 binary / fixture 的成绩混算。

源码入口：`tests/phase11/smoke/onnx_model_benchmark.cpp`；有界回调测量：`tests/phase11/smoke/OnnxBenchmark.h`；编排与报告：`scripts/function3/validation/model_benchmark.py`。离线测试分别为 `tests/phase11/unit/onnx_benchmark.cpp` 和 `tests/project/test_model_benchmark.py`。
