# 27 赛季机库通信桥接修改报告

## 1. 修改目标

本次修改将 C 型板机库改造成一条完整的“裁判系统模拟器 - 机库 - LicheeRV nano 上位机”通信链路：

```text
电脑裁判系统模拟器
        │ USB-TTL / USART1 / 115200 8N1
        ▼
Robomaster C 型板机库
        │ USART6 / 蓝牙透明串口 / 115200 8N1
        ▼
LicheeRV nano 无人机上位机
```

机库接收电脑发送的 RoboMaster 2027 协议帧，按 CRC8、CRC16 和命令码解析；完整帧通过 USART6 原样转发给上位机。每次成功解析/转发后，机库再通过 USART1 返回一帧诊断状态，模拟器可以实时显示机库是否已经收到、解析并转发。

## 2. 协议实现

协议依据用户提供的《RoboMaster 2027 机甲大师高校系列赛通信协议 V1.0.0（20261029）》实现，保留协议帧结构：

```text
A5 + data_length(LE16) + seq + CRC8
  + cmd_id(LE16) + data + CRC16(LE16)
```

当前模拟器和固件支持的裁判系统指令如下：

| 命令 | ID | 数据长度 | 用途 |
| --- | ---: | ---: | --- |
| 比赛状态 | `0x0001` | 11 B | 比赛阶段、阶段剩余时间、同步时间戳 |
| 飞镖选手端指令 | `0x020A` | 6 B | 飞镖舱门开启/关闭/运动中状态 |

`0x0001` 的比赛阶段按协议使用高 4 位，`game_progress = 4` 表示比赛中；`0x020A` 的舱门状态按协议使用 `0 = 已开启、1 = 已关闭、2 = 开启/关闭中`。

## 3. 固件侧修改

### 3.1 新增机库桥接层

新增文件：

- `modules/hangar_bridge/hangar_bridge.h`
- `modules/hangar_bridge/hangar_bridge.c`

桥接层负责：

1. 注册 USART1 作为 2027 裁判系统常规链路。
2. 注册 USART6 作为蓝牙透明发送链路。
3. 从 `rm_referee27` 的完整帧队列中取出已通过 CRC 的帧。
4. 将原始完整帧发送到 USART6，不改变命令码、数据和序号。
5. 在 USART1 返回机库状态诊断帧。
6. 过滤可能被 USB 转串口回显回来的诊断帧，避免诊断帧再次转发给无人机。
7. 状态帧发送采用非阻塞 DMA 就绪检查，不因等待上一帧发送而阻塞底盘任务。

### 3.2 扩展 2027 裁判协议模块

修改文件：

- `modules/refree_27/rm_referee27.h`
- `modules/refree_27/rm_referee27.c`

新增能力：

- 已完成帧环形队列，避免在 DMA/IDLE 回调中直接发送串口。
- `R27RefereePopFrame()`：任务上下文取出完整帧。
- `R27BuildFrame()`：按官方 CRC8/CRC16 封装完整协议帧。
- 保存完整原始帧，支持桥接层透明转发。
- 常规链路掉线判定调整为约 2.5 秒，适配 `0x0001` 的 1 Hz 发送频率。
- `0x0301` 机器人交互数据按变长数据解析，不再固定截断为旧协议长度。

### 3.3 诊断状态回传

机库状态使用官方 `0x0301` 机器人交互帧，定义工程内部子内容 ID `0x02F0`。状态数据以 `H27S` 作为魔数，包含：

- 最近处理的命令码和序号
- 最近数据长度
- 比赛阶段
- 飞镖舱门状态
- 已解析帧数
- 已转发帧数
- CRC/接收错误数
- 转发错误数
- 机库运行时间

该诊断子内容只用于电脑模拟器显示，不转发给 LicheeRV nano。这样不会改变官方 `0x0001`、`0x020A` 的语义。

### 3.4 应用层和串口配置

修改文件：

- `application/robot.c`
- `application/robot_task.h`
- `application/chassis/chassis.c`
- `Src/usart.c`
- `basic_framework.ioc`
- `Makefile`

主要调整：

- `RobotInit()` 中先初始化 `HangarBridgeInit(&huart1, &huart6)`，再初始化底盘应用，避免旧模块抢占串口。
- `RobotTask()` 每 5 ms 调用 `HangarBridgeTask()`。
- 移除旧裁判系统 UI 任务创建，防止旧协议模块注册 USART6。
- 底盘功率限制读取新 2027 结构 `RobotStatus.chassis_power_limit`，首帧到达前使用 80 W 安全默认值。
- USART1 改为协议规定的 115200 / 8N1。
- USART6 保持 115200 / 8N1，用于蓝牙透明链路。
- Makefile 加入 `rm_referee27.c`、`hangar_bridge.c` 及对应头文件目录。

## 4. 模拟裁判系统修改

模拟器目录：

`D:\Robomaster\YQ_Drone\Hangar\referee-simulator`

修改内容：

- `protocol.py`
  - 增加 `0x0301 / 0x02F0` 机库状态帧解析。
  - 校验 `H27S`、状态字段、最近命令、计数器和比赛/舱门状态。
- `server.py`
  - 接收状态帧后保存为 `hangar_status`。
  - `/api/status` 返回最新机库状态，断开串口时清除旧状态。
- `web/index.html`
  - 链路统计面板增加“机库解析状态”卡片。
  - 帮助说明更新为 USART1 115200 和 `0x0301 / 0x02F0` 回传方式。
- `web/app.js`
  - 实时展示状态、最近命令、已解析/已转发计数、错误计数、比赛阶段、舱门状态和回传时间。
- `web/style.css`
  - 增加机库状态卡片的待命、成功、错误和等待转发视觉状态。
- `tests/test_simulator.py`
  - 新增状态帧协议解析测试。
  - 新增回环链路状态帧进入 `/api/status` 的测试。

网页原有能力继续保留：串口列表刷新、真实串口/软件回环连接、单次发送、1 Hz/3 Hz 周期发送、倒计时、CRC 检查、协议帧预览、日志过滤和 JSONL 导出。

## 5. 验证结果

### 固件编译

使用工程内 ARM GCC 和 Make 完整构建成功：

```text
RAM:   85464 B / 128 KB  (65.20%)
FLASH: 86052 B / 1 MB    (8.21%)
```

生成文件：

- `build/basic_framework.elf`
- `build/basic_framework.hex`
- `build/basic_framework.bin`

### 模拟器测试

```text
python -m unittest discover -s tests
Ran 23 tests ... OK
```

覆盖官方 CRC、黄金帧、拆包/粘包、错误恢复、回环串口、周期发送、HTTP 接口、状态帧解析和状态回传显示数据。

浏览器 smoke 测试脚本仍保留在 `tests/browser_smoke.cjs`，但当前开发环境没有 `node` 命令，因此本次无法在本机执行该脚本。

## 6. 使用方法

1. 编译并烧录 `basic_framework` 固件。
2. USB-TTL 的 TX 接 C 型板 USART1 RX，USB-TTL 的 RX 接 USART1 TX，双方共地。
3. 串口工具选择 `115200 / 8N1`；模拟器中选择相同 COM 口和波特率。
4. 启动：

   ```powershell
   cd D:\Robomaster\YQ_Drone\Hangar\referee-simulator
   python server.py
   ```

5. 网页连接串口后点击“发送比赛开始”或“发送舱门开启”。
6. 机库成功处理后，网页“链路统计”中的“机库解析状态”应变为“已转发”，并显示最近命令、解析计数和转发计数。
7. C 型板 USART6 接蓝牙模块，LicheeRV nano 上位机按同一套 2027 帧格式解析收到的数据。

## 7. 当前限制和硬件联调注意事项

- 当前只在软件回环和静态固件构建层面验证，尚未连接实际 C 型板、USB-TTL、蓝牙模块和 LicheeRV nano 做电气联调。
- USB-TTL 电平必须与 C 型板 UART 电平兼容，TX/RX 必须交叉连接并共地。
- USART6 当前发送的是完整官方裁判协议帧；LicheeRV nano 上位机应先按 `0xA5`、长度、CRC8、CRC16 重组，再按 `0x0001`/`0x020A` 解析。
- `0x02F0` 是本工程诊断子内容，不应在无人机上位机执行；若蓝牙链路存在回传或回显，应在上位机过滤 `cmd_id = 0x0301` 且子内容 ID 为 `0x02F0` 的帧。
- 如果实际蓝牙模块使用其他波特率，只需同步修改 `Src/usart.c` 的 USART6 配置和模拟器/上位机串口设置；协议字段本身不需要改变。
