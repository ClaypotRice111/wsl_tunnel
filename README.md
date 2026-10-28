# WSL Tunnel

Windows 10/11 x64 原生 C++ / Win32 可视化软件 + WSL/Linux Python 命令行工具。

## Windows 使用

双击 **dist/WSLTunnel-Native.exe**。主界面只有端口转发、USB 直通和顶部的 WSL 发行版选择。启动自动检测，也可手动刷新，显示 WSL 1/2 及运行状态。

- Windows → WSL：使用 netsh portproxy；目标 auto 自动检测 WSL IPv4。
- WSL → Windows：使用所选发行版内的 socat；auto 根据 NAT/mirrored 检测 Windows 地址，无法可靠检测时需手填 IPv4。
- USB：安装 usbipd-win 后，刷新设备、填写 BUSID、首次共享、接入 WSL。ESP32 复位后重新枚举可能需要再次接入。

修改 Windows portproxy、USB 首次共享/取消共享需要右键 EXE 以管理员身份运行。WSL 反向转发需要 socat（Ubuntu/Debian：sudo apt install socat）。软件不会自动安装依赖、修改防火墙。仅支持 TCP/IPv4，默认监听 127.0.0.1；目标服务必须监听可达地址。

原生界面采用 PerMonitorV2，收到 WM_DPICHANGED 后重建字体与布局。命令在后台线程执行。Windows 界面无需 .NET、PowerShell 界面运行时或 Python。

## 规则与生命周期

规则保存在 %LOCALAPPDATA%/WSLTunnel/native-rules.ini。关闭界面不会停止转发；Windows portproxy 持久保留，WSL socat 随发行版关闭退出。停止 socat 监听后，已有连接可能持续到客户端断开。WSL IP 改变后需停止并重新启动规则。

“操作记录”不是实时健康状态，点击“实际状态”核对。避免多个实例同时修改配置。旧版用户 JSON 规则保留在用户目录，不自动导入原生配置；旧系统端口代理也不会被清理，遇到冲突请先核对并通过 Windows netsh 管理。系统配置和用户规则不属于旧程序清理范围。

USB 设备接入后不能同时供 Windows 独占使用；USB/IP 设备在 WSL 2 发行版间共享，不保证某个发行版独占。不承诺所有设备零延迟或无损兼容。

## WSL/Linux 命令行

Python 3.8+ 标准库，无需 Linux 桌面环境。

```sh
cd /mnt/d/library/wsl_tunnel
sh start.sh doctor
sh start.sh --help
sh start.sh usb list
sh start.sh usb attach --busid 1-2
sh start.sh serial
sh start.sh usb detach --busid 1-2
sh start.sh forward --port 8080 --target 192.168.1.10 --target-port 80
sh start.sh forward --port 11435 --target auto --target-port 11434
sh start.sh ollama
sh start.sh gpu
```

检测发行版 ID/ID_LIKE、WSL 内核与依赖能力，给出 Ubuntu/Debian、Fedora/RHEL、Arch、openSUSE、Alpine 等发行版提示；具体包可用性取决于版本和软件源。CLI 转发前台运行，Ctrl+C 停止，不写 GUI 规则文件。

WSL 默认调用 Windows usbipd.exe，需要启用 Windows interop。WSL sudo 不能提升 Windows 权限。原生 Linux 可使用 --backend linux 管理 usbip，需要内核模块、服务器端 usbipd 和相应权限；接收侧使用 vhci_hcd，导出侧使用 usbip_host。软件不自动加载模块或启动系统服务。

Ollama 优先使用 localhost:11434；必要时在 GUI 中手动配置 Windows 11435 → WSL 11434，确保服务监听可达地址。GPU 检测使用 CLI gpu 命令。WSL 使用 Windows NVIDIA 驱动提供加速，不是通用 PCIe 独占直通；不要在 WSL 安装 Linux NVIDIA 显示驱动。

## 构建与验证

准备官方 LLVM-MinGW x64 UCRT 工具链，见 native/README.md。运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Build.ps1
```

中间文件位于 .build，发布文件位于 dist。关闭正在运行的旧 EXE 后再构建覆盖。EXE 静态链接 C++ 运行库，仅依赖 Windows 系统 DLL；本地构建未签名。

EXE 参数 --self-test 检查解析/校验，--detect-test 查询实际 WSL 发行版，--smoke-test 在屏幕外验证原生 UI 以及 100%/150%/200% DPI 消息布局并生成预览。消息测试不代替真实跨屏拖动测试。Linux 测试：python3 -m unittest discover -s tests -p test_cli.py -v。USB 与网络完整端到端验证需要实际硬件和服务。
