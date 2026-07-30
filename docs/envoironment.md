# CentOS Stream 9 C\+\+开发测试环境部署指南

# 一、环境部署文档 `ENV.md`

## 项目编译 \& 测试环境说明

### 1\. 基础系统

操作系统：**CentOS Stream 9 x86\_64**

> ⚠️ 不兼容 CentOS7 / RockyLinux8、Ubuntu；不同发行版包名、仓库策略存在差异

### 2\. 环境组件清单

| 组件  | 用途  | 备注  |
| --- | --- | --- |
| gcc\-c\+\+ | GNU C\+\+ 编译器 | 主力编译工具 |
| clang / clang\+\+ | LLVM C\+\+ 编译器 | 备选编译器，用于多编译器兼容性测试 |
| cmake \+ make | C/C\+\+ 构建系统 | 项目编译必备 |
| GoogleTest\(GTest\+GMock\) | C\+\+ 单元测试框架 | CentOS Stream9 无官方 RPM 包，源码编译安装静态库 |
| Python3\.9 \+ pip3 | 自动化集成测试环境 | 黑盒接口测试 |
| requests \(Python 库\) | HTTP 接口请求库 | 编写 web 服务自动化测试脚本 |
| git | 版本控制 | 拉取源码、第三方库 |
| gdb | C\+\+ 程序调试器 | 程序崩溃调试 |

### 3\. 已知系统坑点

1. CentOS Stream9 官方软件源**不提供 gtest\-devel**，必须源码编译安装 GoogleTest；
  
2. MySQL 官方仓库 GPG 密钥校验容易失败，所有 dnf 安装命令附带`--nogpgcheck`绕过校验；
  
3. **禁止执行 ****`sudo dnf update -y`**，避免全系统包升级引发 ABI 兼容、环境变动风险；
  
4. 本环境 GTest 采用静态库编译安装，CMake `find_package(GTest)` 可直接识别。
  

### 4\. 环境校验命令（部署完成后自检）

```bash
# C++编译工具
g++ --version
clang++ --version
cmake --version

# 单元测试框架
ls /usr/include/gtest/gtest.h
ls /usr/lib64/libgtest*.a

# Python测试环境
python3 --version
python3 -c "import requests; print(requests.__version__)"

# 辅助工具
git --version
gdb --version
```

### 5\. 开发建议

- Python 仅用于简单接口测试，依赖极少，**无需虚拟环境**；
  
- 单元测试程序推荐静态链接 GTest，发布产物无需附带动态库；
  
- 如需数据库开发，额外安装：`sudo dnf install --nogpgcheck mysql-community-devel`
  

---

# 二、一键部署脚本 `install_env.sh`

```bash
#!/bin/bash
set -e

echo "==================== 开始部署 CentOS Stream9 开发测试环境 ===================="

# 1. 安装系统编译工具、调试工具
sudo dnf install -y --nogpgcheck \
    cmake make \
    gcc-c++ clang clang-tools-extra \
    python3 python3-pip \
    git gdb

# 2. 安装Python测试依赖 requests
pip3 install requests

# 3. 源码编译安装 GoogleTest（CentOS Stream9无官方rpm包）
echo "==================== 编译安装 GoogleTest ===================="
rm -rf ./googletest
git clone https://github.com/google/googletest.git
cd googletest

cmake \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_BUILD_TYPE=Release \
    -B build

cmake --build build -j$(nproc)
sudo cmake --install build

# 清理源码
cd ..
rm -rf ./googletest

echo "==================== 环境安装完成，执行自检命令验证 ===================="
echo "【自检命令】"
echo "g++ --version"
echo "clang++ --version"
echo "cmake --version"
echo "ls /usr/include/gtest/gtest.h"
echo "ls /usr/lib64/libgtest*.a"
echo "python3 -c \"import requests; print(requests.__version__)\""
```

## 使用方式

1. 保存脚本：`vim install_env.sh`，粘贴全部内容
  
2. 添加执行权限：
  

```bash
chmod +x install_env.sh
```

3. 一键运行：

```bash
./install_env.sh
```

## 补充说明

1. 如果网络较差，googletest 克隆缓慢，可以替换国内镜像地址：
  `git clone shturl.cc/4rtpqWDZETXMCikWgwGWJ1zZXQ4XzY`
  
2. `set -e`：任意命令出错脚本直接终止，方便排查部署故障；
  
3. 所有 dnf 命令携带`--nogpgcheck`，规避 MySQL 仓库 GPG 校验失败阻塞安装；