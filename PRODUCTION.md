# 量产与发布流程

本文档给出从源码到出货的工程流程，以及当前设计在量产条件下的边界。

## 1. 版本与构建

- 固件版本：`app_template/src/app_version.h`（major/minor/patch/build）。
- 版本块会嵌入应用镜像，上位机与 Bootloader 都可读取（见 README「固件版本」）。
- 发布构建：

```powershell
.\build-release.ps1 -Vid 0x1234 -Pid 0x5678
```

脚本会以 `EXTRA_CONF_FILE` 注入合法 VID/PID，构建 Bootloader 与应用，输出到 `dist\release\`。
若提供 `-SignTool`、`-CertPath`、`-CertPassword`，会对上位机 exe 做 SHA-256 代码签名。

> `0x1209:0xF103` 是原型 PID，不可用于商业出货。正式产品必须使用自有 VID/PID。

## 2. 首次烧录（工厂）

1. 使用 SWD 烧录 `f103_usb_bootloader.hex`（0x08000000，32 KiB）。
2. 烧录当前发布应用 `app_template.hex`（0x08008000）。
3. 至少执行一次完整 USB 升级并确认（生成有效元数据），然后记录：
   - A 区镜像 CRC、版本号
   - 元数据 seq
4. 如需序列号/校准数据，建议放在应用区的独立参数页，不要复用元数据页与交换日志页。
5. 依据产品策略配置 RDP/WRP（见 SECURITY.md）。

**不建议**把“首次烧录后 trial 次数耗尽自动转永久”作为正式首件流程；正式产线应保证出厂前完成一次 `image_ok` 确认。

## 3. Bootloader 自身更新与恢复

32 KiB 单副本 Bootloader **无法**在片内做 A/B 双 Bootloader。当前可用的恢复路径：

| 路径 | 前置条件 | 说明 |
|---|---|---|
| SWD | 调试焊盘/排针可用 | 最可靠，工厂与售后均可使用 |
| STM32 ROM DFU | `BOOT0=1` 复位 + USB 连接 | 使用 STM32CubeProgrammer（0483:DF11）烧录 Bootloader+应用 |
| `ENTER_ROM_BOOTLOADER` 命令 | 设备仍能运行 Bootloader | 跳入 ROM 引导程序；本板该引导程序默认等待 USART1，USB DFU 仍需 BOOT0 配置 |

建议量产板：
- 保留 `BOOT0` 测试点或按键（可跳线拉高）；
- 保留 SWD 焊盘（不长期焊接排针）；
- 在结构件上标注恢复方式。

后续产品化建议：换用 ≥64 KiB Bootloader 分区的 MCU，实现双 Bootloader + 签名验证。

## 4. 读保护与调试口

见 [SECURITY.md](SECURITY.md#4-读保护--调试口)。要点：
- 量产启用 RDP Level 1 + Bootloader 区 WRP。
- 解除 RDP 需要整片擦除，务必在产线流程中明确。
- 开发板保持未保护状态。

## 5. Flash 寿命估算

F103CBT6 每页标称 10k 次擦除。单次升级的擦除分布：

| 区域 | 每次升级擦除次数 | 估算寿命 |
|---|---|---|
| A/B 槽页 | 约 3 次/页（scratch + A + B） | ~3000 次升级 |
| 元数据页 | 2–4 次/升级 | ~2500–5000 次升级 |
| 交换日志页 | 1 次/次交换 | ~10000 次交换 |
| 应用下载页（B） | 1–2 次/升级 | ~5000 次升级 |

以上为最保守估算，实际受擦写策略、温度与批次影响。高频升级场景（每日多次）建议：
- 降低日志/元数据写频率（当前已按需写入）；
- 在文档中限制保修内升级次数；
- 或换用支持更高擦写寿命的 MCU。

## 6. 元数据规则（实现约定）

- 两份元数据页交替写入，`record_crc` 最后编程。
- 启动时选择 **序号最大且通过 magic/CRC/字段校验** 的记录。
- 任一副本损坏时自动回退到另一份有效副本（已实板验证）。
- 写入失败时保留旧记录；Bootloader 停留在 USB 窗口等待重试或人工恢复。
- 元数据格式为 v3（`3BMT`）；v1/v2 旧记录会被判为无效。

## 7. 升级与回滚的掉电安全

- 交换日志为追加式（每步写新的 16 位记录），单次交换最多 138 条 / 512 容量。
- 每次发起交换前整页擦除日志（此时 A/B 均完整）。
- 交换中断后复位自动续传；已实板验证扇区边界、扇区中途（rem=1/2）两类中断点。
- 回滚交换使用同一机制，已单独验证回滚中途掉电。
- 详细矩阵与结果见 [docs/FAULT_INJECTION.md](docs/FAULT_INJECTION.md)。

## 8. 测试与验收

建议把以下项目纳入出厂/回归测试：

1. 完整升级 + 确认（版本号、CRC、USB 重新枚举）。
2. `tools/test/protocol_negative.py`（协议负向测试）。
3. 电源跌落：交换过程随机断电 20 次以上，验证每次恢复后 A 区镜像 CRC 一致。
4. 连续升级循环 ≥100 次（Flash 寿命抽样）。
5. 双副本不一致、日志中途损坏、scratch 异常。
6. USB 主机兼容性（Windows 10/11、不同 Hub）。
7. 温度/EMC/ESD 由第三方实验室完成。

## 9. 第三方组件与许可

| 组件 | 许可 | 说明 |
|---|---|---|
| Zephyr RTOS 4.4.2 | Apache-2.0 | 分发需保留 LICENSE/NOTICE |
| STM32Cube HAL/LL | BSD-3-Clause（ST 条款） | 随 Zephyr module 引入 |
| CMSIS | Apache-2.0 | ARM |
| hidapi（上位机） | BSD-3-Clause | Python 依赖 |
| PyInstaller（打包） | GPL + 例外 | 仅用于构建上位机 exe，需保留许可声明 |

发布镜像前请生成完整的 SBOM 与许可证清单；本表为当前仓库的基线。

## 10. 发布检查清单

- [ ] VID/PID 合法且已写入发布构建
- [ ] 应用版本号已更新，版本块可被上位机读取
- [ ] 完整升级 + 确认实板通过
- [ ] 协议负向测试通过
- [ ] 掉电/中断注入抽样通过
- [ ] 上位机 exe 已签名
- [ ] RDP/WRP 配置与恢复流程确认
- [ ] 许可证清单与 SBOM 归档
- [ ] 现场恢复说明随产品交付
