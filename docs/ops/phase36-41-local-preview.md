# Phase36–41本地成果如何复现

本轮没有推送GitHub，也没有替换日常18090服务。日常服务仍使用原账号库；新功能的测试使用既有隔离库与18094/18444。不要把两套实例的账号误认为同一份数据。

## 1. Linux测试服务

```bash
cd /home/pikachu/function3-src
# 仅补齐既有隔离库中的新增迁移，不创建新的业务库。
python3 scripts/function3/validation/isolated_postgres.py migrate --database function3_runtime_test

# 推荐的独立推理路径。复用已有模型、证书和单模型驻留预算；不下载依赖。
python3 scripts/function3/validation/run_server.py start --runtime-certificate --ai-worker --tts
```

- HTTP18094、HTTPS18444；学习用主程序gRPC隔离到本机50064。
- 独立ONNX Worker只监听本机50062，使用TLS和私有Token；不在此端口开放未认证的网络访问。
- AI账号必须绑定 `onnx-grpc` 版本。集成测试创建的是隔离库内的专用夹具；正式账号应走模型注册、质量门禁和绑定流程，不要直接篡改活跃版本的runtime。
- 教学进程内模式可用 `--ai` 替代 `--ai-worker`，二者互斥，避免在这台7.5GiB虚拟机同时驻留两份模型。
- 没有已有模型或证书时脚本报错，不自行下载或更换证书。模型首次加载需要时间；短请求热态测量不能代表长RAG上下文耗时。

## 2. Windows本机入口

先用现有MinGW构建，原来的中文目录Ninja兼容问题由显式构建脚本规避：

```powershell
cd 'E:\虚拟机高级web开发\github-test6.3-staging'
python -X utf8 scripts/client/build_windows.py --boost-root 'E:\虚拟机高级web开发\deps\boost-1.92.0'
```

本轮Windows直接访问虚拟机18444端口受现有网络设置限制，验收复用了SSH转发，没有修改防火墙。需要时在独立终端运行：

```powershell
ssh -N -L 127.0.0.1:18444:127.0.0.1:18444 pikachu@192.168.239.135
```

然后启动本机入口，`--data-dir`应由本人选择一个新的专用目录：

```powershell
.\build-client\release\nebula_companion.exe `
  --server 192.168.239.135 --tls-port 18444 --connect-host 127.0.0.1 `
  --certificate-sha256 '<本人已核实并同意的当前证书SHA256>' `
  --frontend 'E:\虚拟机高级web开发\github-test6.3-staging\frontend\phase11' `
  --data-dir '<本人选择的私人资料专用绝对路径>' `
  --python 'C:\Users\24851\AppData\Local\Programs\Python\Python312\python.exe' `
  --parser 'E:\虚拟机高级web开发\github-test6.3-staging\scripts\client\documents\extract_document.py'
```

入口同时监听127.0.0.1和::1，URL仍是localhost:18091；启动终端给出一次性打开链接。只接受本机Host/Origin/会话，不作为任意URL代理。SSH转发只改变TCP连接地址，TLS仍验证192.168.239.135、有效期、用途及已确认指纹，不安装系统根证书。

浏览器注册/登录后，在“私人助手→设置”选择当前启用的ONNX运行方式与AI账号。好友式页面和侧栏的会话历史分开，资料与长期记忆共享。群AI继续走Linux共享会话，不读取私人资料。直接浏览Linux网页可用共享业务，私人功能需要本机入口。

## 3. 语音与停止

个人语音设置位于私人助手“设置”；站点公告设置位于管理中心“公告音色”。个人Key只在Windows DPAPI中保存，站点Key来自Linux私有文件。没有Key仍能使用服务器Kokoro；Windows不会新增大型TTS模型。真实云语音和自然度验收限制见[Phase40](../architecture/business/phase40.md)。

本机入口按Ctrl+C退出；转发终端也按Ctrl+C关闭。Linux测试服务与独立Worker分别停止：

```bash
python3 scripts/function3/validation/run_server.py stop
python3 scripts/function3/validation/onnx_worker.py stop
```

脚本只控制匹配路径与启动标识的测试进程。日常18090服务不受这些停止命令控制。本轮验收结束会关闭测试实例和临时转发；再次预览时按本文手动启动即可。

完整结果：[Phase41](../architecture/business/phase41.md)；架构：[总图](../architecture/business/phase36-41-end-to-end.md)；代码：[组件索引](../architecture/business/phase36-41-code-index.md)。

Phase42沿用上述启动方式、端口和数据库。重新构建后普通短请求默认复用已验证的TLS连接；神树前端与Linux服务应一同使用本地新版本，才能使用批量榜单读取。日常18090仍未切换，结果见[Phase42](../architecture/business/phase42.md)。
