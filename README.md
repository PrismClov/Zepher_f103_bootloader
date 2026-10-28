# STM32F103CBT6 USB A/B Bootloader

基于 Zephyr 4.4.2 的 STM32F103CBT6 USB HID IAP 双槽 Bootloader。Windows 使用系统 HID 驱动，不需要 USB 串口、WinUSB 或 dfu-util。

## 已实板验证

- SWD 烧录与整片校验
- USB HID 正常运行和重新枚举
- Slot A 升级
- 应用运行中从 A 升级到 B
- 应用运行中从 B 升级回 A
- 写入 B 槽 26% 时强制中断，复位后 A 槽仍正常启动
- 双元数据页交替提交及 CRC 校验
- Windows 图形化上位机
- pending 试运行、启动计数与自动回滚（已实板验证）
- 坏镜像自动回滚：破坏候选镜像 CRC 后复位，Bootloader 拒绝切换并回滚
- 试运行次数耗尽自动回滚（J-Link 构造 `boot_attempts=3` 记录实板验证）
- 无回退槽防变砖：首次烧录无旧固件时，试运行耗尽后自动转永久生效

## A/B 分区

STM32F103CBT6 的 128 KiB Flash 分配如下：

| 区域 | 起始地址 | 结束地址 | 大小 |
|---|---|---:|---:|
| Bootloader | `0x08000000` | `0x08007FFF` | 32 KiB |
| Slot A | `0x08008000` | `0x08013BFF` | 47 KiB |
| Slot B | `0x08013C00` | `0x0801F7FF` | 47 KiB |
| Metadata A | `0x0801F800` | `0x0801FBFF` | 1 KiB |
| Metadata B | `0x0801FC00` | `0x0801FFFF` | 1 KiB |

分区定义在 [app.overlay](app.overlay)。Bootloader 占用前 32 KiB，USB 升级永远不会擦除该区域。

两个应用槽的链接地址不同，因此需要两份固件：

- `app_template_slot_a.bin`：链接到 `0x08008000`
- `app_template_slot_b.bin`：链接到 `0x08013C00`

两份镜像来自同一套源码，只是设备树代码分区不同。每个槽最大 47 KiB。

## 升级过程（六步 A/B 流程）

Bootloader 从两份元数据页中选择序号最大且 CRC 正确的记录，按下面的流程处理升级：

| 步骤 | 动作 | 说明 |
|---|---|---|
| 1. 写入 B 区 | 应用层接收升级包，按块写入非活动槽 | 活动槽固件与提交记录保持不变 |
| 2. 提交 pending | 完整下载、CRC 校验和 Flash 回读通过后，最后一步才提交 `pending` 元数据 | `record_crc` 最后编程，提交中掉电时旧记录仍是权威记录 |
| 3. 镜像校验 | 复位后先读元数据，再校验候选镜像的向量表和 CRC32 | 校验失败不切换，继续使用旧槽 |
| 4. 断点交换 | 本设计的“交换”是元数据中的活动槽指针切换，每一步都持久化到 Flash | 掉电后下次启动可继续处理，无需按扇区搬运 |
| 5. TEST 启动 | 以 TEST 状态启动新固件，每次启动前把启动计数持久化到元数据 | 计数达到上限仍无确认则回滚 |
| 6. 确认 / 回滚 | 应用自检通过后调用 `f103_bootloader_confirm_image()` 写入 `image_ok`；未确认自动切回 `known_good_slot` | 崩溃、看门狗复位或迟迟不确认都会回滚 |

试运行规则：

- 新固件最多试启动 `F103_TEST_BOOT_MAX_ATTEMPTS`（3）次。
- 应用调用 `f103_bootloader_confirm_image()` 清除 pending 标志后永久生效；示例应用在 `runtime_usb_init()` 成功后确认，产品中应替换为真实自检（时钟、存储、外设、通信、看门狗）通过后再调用。
- 连续 3 次启动都没有确认时，Bootloader 自动把活动槽切回 `known_good_slot` 并清除 pending。
- 首次烧录（设备上没有旧固件可回退）时，试运行次数用尽后自动转为永久生效，避免变砖。
- 存在 pending 记录时再次升级会覆盖未确认的测试镜像，而不是回滚槽中的旧固件。

如果写入目标槽时掉电、拔线或上位机退出，当前活动槽仍保持有效。下次复位会继续启动之前的应用。

> 元数据格式已升级为 v2（magic `"2BMT"`，新增 `known_good_slot`、`boot_attempts`、`flags` 字段）。旧 v1 格式记录会被判为无效；升级 Bootloader 后第一次启动会停留在 USB 窗口等待重新下载一份固件。

## 常驻 USB 升级接口

示例应用包含 [runtime_usb.c](app_template/src/runtime_usb.c)，运行时会持续枚举为：

```text
F103 USB Application
VID:PID = 1209:F103
```

Windows 上位机连接应用后发送 `ENTER_BOOTLOADER`。应用主动断开 D+、复位到 Bootloader，升级工具随后自动选择非活动槽并继续下载。无需按键、拔插或手动 Reset。

Bootloader 运行时显示为：

```text
F103 USB IAP Bootloader
```

## 构建

首次初始化：

```powershell
.\setup.ps1
```

构建 Bootloader、Slot A 和 Slot B：

```powershell
.\build.ps1
```

输出：

```text
build\bootloader\zephyr\zephyr.bin
build\app_slot_a\zephyr\zephyr.bin
build\app_slot_b\zephyr\zephyr.bin
dist\f103_usb_bootloader.bin
dist\app_template_slot_a.bin
dist\app_template_slot_b.bin
```

Slot A 使用 [app_template/app.overlay](app_template/app.overlay)，Slot B 使用 [app_template/slot_b.overlay](app_template/slot_b.overlay)。

## 图形化上位机

运行：

```text
dist\F103_USB_Updater.exe
```

上位机会自动选择发布目录中的 A/B 固件对，并显示：

- 当前 USB 设备角色
- A、B 固件路径
- 两个镜像的大小和 CRC32
- 实时写入进度
- 当前 Bootloader 选择的写入槽
- 校验结果和运行日志

点击“开始升级”即可。应用运行中也可以直接升级。

## 命令行升级

```powershell
dist\f103_uploader.exe dist\app_template_slot_a.bin `
  --slot-b dist\app_template_slot_b.bin
```

工具会先查询当前活动槽，再自动选择正确的 A 或 B 镜像。只提供一个槽的镜像时，只有目标槽匹配才能升级。

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
| `extra[4:8]` | 下一次目标槽基地址 |
| `extra[8:12]` | 单槽最大尺寸 |
| `extra[12]` | 设备角色：Bootloader/Application |
| `extra[13]` | 活动槽编号 |
| `extra[14]` | 目标槽编号 |
| `extra[15]` | 是否存在有效活动槽 |
| `extra[16]` | pending 试运行标志 |
| `extra[17]` | 已试启动次数 |
| `extra[18]` | 最大试运行次数 |
| `extra[19]` | 回滚槽（known_good）编号 |

命令包括 `PING`、`BEGIN`、`DATA`、`END`、`RUN` 和 `ENTER_BOOTLOADER`。

## 注意事项

- `0x1209:0xF103` 仅用于原型验证，正式产品应换成自己合法持有的 USB VID/PID。
- 当前没有固件签名，CRC32 只能保证完整性，不提供安全启动能力。
- 单槽最大 47 KiB，示例应用约 41.7 KiB，业务继续增加时需关注剩余空间。
- Zephyr 固定为 4.4.2。Zephyr 4.5 移除 STM32F1 所需的旧 USB Device 栈前必须重新评估驱动方案。
