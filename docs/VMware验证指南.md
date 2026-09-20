# 在 VMware 上验证 VimtuOS 64 位（安装 + 启动）

这份文档记录**怎么在 VMware Workstation 里跑完整个安装流程并自动验收**，
以及踩到的三个 VMware 特有的坑。QEMU 侧的验收见 `tests/install_flow_test.py`。

## 0. 一条命令

```powershell
# 需要 VMware Workstation（自带 vmrun.exe）+ 已跑过 build64.sh 生成 vimtu64-64.img
cd C:\Users\fanqi\Desktop\VimtuOS\Vimtu64
python tests\vmware_install_test.py
```

它会自己做这些事（全自动，不用点鼠标）：

1. 在 `C:\Users\fanqi\Desktop\新建文件夹\v64-install-test\` 下建测试 VM：
   * `installer.img`（= `vimtu64-64.img` 的副本，16266 扇区）
   * `target.img`（16MB 空目标盘）
   * `installer.vmdk` / `target.vmdk`（monolithicFlat 描述符，指向上面两个 raw 文件）
2. `vmrun start … nogui` 启动安装 VM（BIOS 固件；IDE0:0 = 安装介质，IDE0:1 = 目标盘）
3. 等串口日志出现"磁盘枚举完成"，然后**从 COM2 串口通道**送入按键：
   回车×4 → `n`（新建）→ 回车（安装系统）
4. 断言串口：`[PART] 新建分区表 OK` / `[INSTALL] 开始安装` / `[INSTALL] 完成：已写 8073 扇区` /
   `[SETUP] 安装完成 100%` / `[SETUP] 自动重启`
5. 解析 `target.img`：MBR 55AA + P1(`0xEF`, 活动, 9+8000) + P2(`0x07`, 8009..盘尾)，
   LBA1..8 = `loader64.bin` 逐字节、LBA9.. = `kernel64_os.bin` 逐字节
6. 用 `vimtu64-installed-boot.vmx`（只挂 `target.vmdk`）单独启动，断言走 `[OS]` 路径

实测结果：**24 项断言全 PASS**。

## 1. 坑 1：`guestOS` 必须是 64 位类型，否则 CPUID 长模式位被屏蔽

VMware 对 `guestOS = "other"`（32 位类型）会**屏蔽 CPUID.80000001h:EDX 的 LM 位**，
我们的引导链在进长模式前会检查这一位，检查失败会停机 —— 表现是"屏幕全黑、串口没动静"。

修法：`guestOS = "other-64"`（或任何 `-64` 结尾 / 已知 64 位系统类型）。
测试脚本生成的 vmx 已经写好这一项；**用户自己建的 64 位 VM 也要改成 64 位类型**。

## 2. 坑 2：PAE 位必须读 `CPUID.01h:EDX`（Intel 不在 80000001h 报 PAE）

原来引导链把 PAE 读在 `CPUID.80000001h:EDX bit6`：AMD 会报，**Intel 不报**
（本机 Intel i3-6100 实测 `80000001h:EDX=0x2C100800`，bit6=0）。

于是：
* QEMU 的默认 CPU `qemu64` 是 AMD 风格 → 一直"碰巧通过"（这就是长期没发现的原因）
* VMware 直通 Intel 主机 CPU → 判定"不支持长模式"→ 静默 `hlt`

修法：PAE 改读 `CPUID.01h:EDX bit6`，LM 仍读 `80000001h:EDX bit29`；
并且失败时把原因和 CPUID 实际数值打到**串口**（`boot/loader64.asm` 的 `lm32*` 标记 +
`lm64_fatal`），不再有"屏幕没反应、日志什么都没有"这种盲区。

## 3. 坑 3：VMware 自带 VNC 是只读的，pipe 串口也建不起来

为了自动送按键，先后试过：

| 方案 | 结果 |
|---|---|
| monitor/键盘注入 | VMware 没有 `sendkey`；`vmrun` 也不提供发键接口 |
| 抢占 VMware 窗口 + SendKeys | 需要前台窗口与焦点，脚本不可靠 |
| VMware 自带 VNC（`RemoteDisplay.vnc.*`） | **只读**：连上能看到画面（1024x768），发 KeyEvent 客人收不到；`FramebufferUpdateRequest` 也不响应（只在画面变化时推帧） |
| `vmrun captureScreen` | 需要 VMware Tools（`VixVM_LoginInGuest`），自定义 OS 用不了 |
| 串口 `fileType = "pipe"` | VMware 建管道失败（`Unable to create the server-side instance ... named pipe`），全路径/短名都失败 |
| **串口 `fileType = "network"`（TCP，`mode = "server"`）** | ✅ 可用：VMware 监听，主机连上去写字节 → 进客人 COM2 接收缓冲 |

所以最终方案是**给安装程序加 COM2 按键通道**（`kernel/setup64_keys.h`），
VMware 侧用 network 模式串口，QEMU 也可以 `-serial pipe:`/socket 用同一条路径，
真机上换一根串口线同样成立。

```ini
serial0.present = "TRUE"
serial0.fileType = "file"          ; 客人日志（我们靠它做断言）
serial0.fileName = "…\serial-install.log"
serial1.present = "TRUE"
serial1.fileType = "network"
serial1.fileName = "tcp://127.0.0.1:4557"
serial1.mode = "server"
```

## 4. 手工观察（不用脚本）

```powershell
$vmrun = "C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe"
$vmx   = "C:\Users\fanqi\Desktop\新建文件夹\v64-install-test\vimtu64-install-test.vmx"
python tests\vmware_make_vm.py           # 生成/刷新磁盘与 vmx
& $vmrun -T ws start $vmx                # 带窗口启动，可以直接用鼠标键盘操作
Get-Content "C:\Users\fanqi\Desktop\新建文件夹\v64-install-test\serial-install.log" -Tail 20
& $vmrun -T ws stop $vmx hard
```

带窗口启动后就是一台"真机"：BIOS 自检 → 我们的引导链 → 安装向导，鼠标键盘都能用。

## 5. 用户自己那台 VM（`VimtuOS 64-bit.vmx`）

那份配置跑的是**32 位** VimtuOS（3MB vmdk）。要装 64 位版有两个前提：

1. `guestOS` 改成 64 位类型（见坑 1）
2. 磁盘容量要够：安装介质 16266 扇区（7.9MB）、目标盘建议 ≥ 64MB（装好的盘要建 48MB 的 FAT32 ESP；
   16MB 这类小盘只写老 MBR 布局（BIOS-only），会打 `[INSTALL] esp skipped (disk too small)`）；
   也可以直接用本目录生成的 `installer.vmdk` / `target.vmdk` 挂上去
