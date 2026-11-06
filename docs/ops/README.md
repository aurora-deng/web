# 运行维护分类

|用途|入口|
|---|---|
|日常启动/停止与隔离预览|[runtime](runtime/README.md)|
|工具链与环境搭建|[setup](setup/README.md)|
|依赖复用、审批与卸载|[dependencies](dependencies/README.md)|
|测试、版本核对与回退|[validation](validation/README.md)|
|按症状排错|[troubleshooting](troubleshooting/README.md)|
|早期收口/停机设计|[closeout](closeout/README.md)|
|历史版本与上线记录|[变更流水](changelog.md)|

先区分日常18090/18443与隔离18094/18444；目录整理及GitHub提交不代表已替换Linux日常服务。新依赖仍需批准，公共业务库不随新功能反复创建。
