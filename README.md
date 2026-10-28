# STM32F103CBT6 USB A/B Bootloader（MCUboot 风格）

基于 Zephyr 4.4.2 的 STM32F103CBT6 USB HID IAP 双槽 Bootloader。Windows 使用系统 HID 驱动，不需要 USB 串口、WinUSB 或 dfu-util。

固件始终下载到 B 区，复位后按扇区交换到 A 区并以 TEST 状态试运行；应用自检通过后写 `image_ok` 确认，连续 3 次启动未确认则自动交换回滚。整个过程掉电安全，可从交换日志断点续传。

## 已实板验证

- SWD 烧录与整片校验
- USB HID 正常运行和重新枚举
- 完整升级：写 B 区 → 提交 pending → 校验 → 按扇区交换 → TEST 启动 → 应用确认
- 交换中断后复位续传（已验证扇区边界和扇区中途两种中断点）
- 试运行次数耗尽自动回滚（交换回旧镜像，内容逐字节验证）
- 候选镜像校验失败拒绝交换、继续运行旧固件
- 无回退槽防变砖：首次烧录时试运行耗尽自动转永久生效
- Windows 图形化上位机（单镜像）

## A/B 分区

STM32F103CBT6 的 128 KiB Flash 分配如下：

| 区域 | 起始地址 | 结束地址 | 大小 |
|---|---|---:|---:|
| Bootloader | `0x08000000` | `0x08007FFF` | 32 KiB |
| Slot A（运行槽） | `0x08008000` | `0x080137FF` | 46 KiB |
| Scratch（交换暂存） | `0x08013800` | `0x08013BFF` | 1 KiB |
| Slot B（下载槽） | `0x08013C00` | `0x0801F3FF` | 46 KiB |
| Swap Log（交换日志） | `0x0801F400` | `0x0801F7FF` | 1 KiB |
| Metadata A | `0x0801F800` | `0x0801FBFF` | 1 KiB |
| Metadata B | `0x0801FC00` | `0x0801FFFF` | 1 KiB |

分区定义在 [app.overlay](app.overlay)。**应用只构建一份，统一链接到 Slot A（`0x08008000`）**，下载时作为数据写入 B 区，交换后从 A 区运行。

## 升级过程（六步 A/B 流程）

| 步骤 | 动作 | 说明 |
|---|---|---|
| 1. 写入 B 区 | 应用层接收升级包，按块写入下载槽 B | A 区固件与元数据保持不变 |
| 2. 提交 pending | 完整下载、CRC 校验和 Flash 回读通过后，最后一步才提交 `pending\|SWAP` 元数据 | `record_crc` 最后编程，提交中掉电时旧记录仍是权威记录 |
| 3. 镜像校验 | 复位后先读元数据，再校验候选镜像的向量表和 CRC32 | 校验失败不交换，继续运行 A 区旧固件 |
| 4. 按扇区交换 | B→A 逐扇区搬运（借 scratch 中转），每步追加写交换日志 | 掉电后从日志断点续传，不需要整片重来 |
| 5. TEST 启动 | 交换完成后从 A 区启动新固件，每次启动前持久化启动计数 | 计数达到上限仍无确认则回滚 |
| 6. 确认 / 回滚 | 应用自检通过后调用 `f103_bootloader_confirm_image()` 写入 `image_ok`；未确认则交换回滚 | 崩溃、看门狗复位或迟迟不确认都会回滚 |

试运行规则：

- 新固件最多试启动 `F103_TEST_BOOT_MAX_ATTEMPTS`（3）次。
- 应用调用 `f103_bootloader_confirm_image()` 清除 pending 标志后永久生效；示例应用在 `runtime_usb_init()` 成功后确认，产品中应替换为真实自检（时钟、存储、外设、通信、看门狗）通过后再调用。
- 连续 3 次启动都没有确认时，Bootloader 把 A/B 再交换一次，旧固件回到 A 区并继续运行。
- 若候选镜像校验失败，直接放弃交换请求，继续启动旧固件，B 区内容等待下次下载覆盖。

### 交换日志与掉电安全

- 每个 1 KiB 扇区交换分 3 个安全点：`scratch ← A[k]`、`A[k] ← B[k]`、`B[k] ← scratch`，每完成一步在交换日志页追加一条 16 位记录。
- F1 的 Flash 半字只能从 `0xFFFF` 编程一次，因此日志采用**追加式**写入（从不重复编程同一半字）。
- 复位后按日志重放：扇区中途中断时，利用仍有效的 scratch 与未破坏的对方扇区幂等恢复，再从下一个扇区继续。
- 交换完成后才提交元数据（描述符互换、`known_good` 翻转、清除 SWAP 标志）。
- 扇区交换通过寄存器级 FLASH 操作完成（`raw_erase_page` / `raw_flash_write`），规避通用驱动在重锁阶段的间歇性 `-EIO`。

> 元数据格式为 v3（magic `"3BMT"`）。旧 v1/v2 记录会被判为无效；升级 Bootloader 后第一次启动会停留在 USB 窗口等待重新下载固件。

## 常驻 USB 升级接口

示例应用包含 [runtime_usb.c](app_template/src/runtime_usb.c)，运行时会持续枚举为：

```text
F103 USB Application
VID:PID = 1209:F103
```

Windows 上位机连接应用后发送 `ENTER_BOOTLOADER`。应用主动断开 D+、复位到 Bootloader，升级工具随后自动上传并交换。无需按键、拔插或手动 Reset。

Bootloader 运行时显示为：

```text
F103 USB IAP Bootloader
```

## 构建

首次初始化：

```powershell
.\setup.ps1
```

构建 Bootloader 和应用（单镜像）：

```powershell
.\build.ps1
```

输出：

```text
build\bootloader\zephyr\zephyr.bin
build\app\zephyr\zephyr.bin
dist\f103_usb_bootloader.bin
dist\app_template.bin
```

## 固件版本

应用镜像内嵌固定布局的版本块（`common/f103_fw_version.h`），版本号在
[app_template/src/app_version.h](app_template/src/app_version.h) 中维护：

```c
#define APP_FW_VERSION_MAJOR 1
#define APP_FW_VERSION_MINOR 0
#define APP_FW_VERSION_PATCH 0
#define APP_FW_VERSION_BUILD 1
```

- 应用运行中：PING 直接上报自身版本（`extra[24:30]`）。
- Bootloader 运行中：扫描 A 区镜像中的版本块并上报，因此升级前后都能看到当前设备固件版本。
- 首次烧录（还没有元数据）时 Bootloader 无法定位 A 区镜像，版本显示为 unknown，完成一次升级后即正常。

## 图形化上位机

运行：

```text
dist\F103_USB_Updater.exe
```

上位机会自动选择发布目录中的 `app_template.bin`，并显示：

- 设备当前固件版本（应用运行中直接读取；Bootloader 下扫描 A 区镜像）
- 待升级文件的版本、大小和 CRC32
- 实时写入进度、校验结果和运行日志

点击“开始升级”即可；应用运行中也可以直接升级（会自动切换到 Bootloader）。

## 命令行升级

```powershell
dist\f103_uploader.exe dist\app_template.bin
```

只需一份镜像：上位机会校验其链接地址必须为 Slot A，并固定写入 B 区。

## 协议

HID 使用 Vendor Usage Page `0xFF00` 和 64 字节中断端点：

- Report ID `1`：设备到主机
- Report ID `2`：主机到设备
- 请求：`A5 5A cmd seq offset_le32 len data[54]`
- 响应：`status cmd seq value_le32 extra[56]`

`PING` 返回：

| 字段 | 内容 |
|---|---|
| `extra[0:4]` | 当前活动槽基地址 |
| `extra[4:8]` | 下一次目标槽基地址（固定为 Slot B） |
| `extra[8:12]` | 单槽最大尺寸 |
| `extra[12]` | 设备角色：Bootloader/Application |
| `extra[13]` | 活动槽编号（固定 0 = A） |
| `extra[14]` | 目标槽编号（固定 1 = B） |
| `extra[15]` | 是否存在有效元数据 |
| `extra[16]` | pending 试运行标志 |
| `extra[17]` | 已试启动次数 |
| `extra[18]` | 最大试运行次数 |
| `extra[19]` | 回退槽（known_good）编号 |
| `extra[20]` | 交换进行中标志 |
| `extra[21]` | 已交换扇区数 |
| `extra[22]` | 总扇区数 |

命令包括 `PING`、`BEGIN`、`DATA`、`END`、`RUN` 和 `ENTER_BOOTLOADER`。

## 安全、量产与验证文档

- [SECURITY.md](SECURITY.md) — 威胁模型、无签名风险、RDP/WRP 与调试口策略
- [PRODUCTION.md](PRODUCTION.md) — 发布构建、VID/PID、代码签名、量产烧录、Bootloader 自更新/恢复、Flash 寿命、许可证清单
- [docs/FAULT_INJECTION.md](docs/FAULT_INJECTION.md) — 防掉电/故障注入测试矩阵与实板结果
- `build-release.ps1` — 注入合法 VID/PID 并可选签名上位机
- `tools/test/protocol_negative.py` — 协议负向测试（10 项）

## 注意事项

- `0x1209:0xF103` 仅用于原型验证，正式产品应换成自己合法持有的 USB VID/PID。
- 当前没有固件签名，CRC32 只能保证完整性，不提供安全启动能力。
- 单槽最大 46 KiB，示例应用约 43.5 KiB；业务继续增加时需关注剩余空间。
- 交换一次约需数秒（46 扇区 × 擦除/编程）。
- Zephyr 固定为 4.4.2。Zephyr 4.5 移除 STM32F1 所需的旧 USB Device 栈前必须重新评估驱动方案。
