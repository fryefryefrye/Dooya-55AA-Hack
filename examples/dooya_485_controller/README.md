# Example - Dooya 485 (Unified Serial Protocol) Blind Controller

One sketch with everything needed to drive a Dooya motor over **RS485** using
the officially documented **Dooya Unified Serial Protocol (杜亚统一串口协议)**:
send commands, poll the position, decode the replies, print the traffic in hex.
No WiFi, no cloud — just the serial console, so it works on a bare ESP8266 and
is easy to graft onto a bigger firmware later.

Use this example when the motor **exposes an RS485 interface directly** —
open-close curtain and roller blind models commonly do. If your motor only has
the 6-pin TTL cable (a translator in between), use
[`dooya_55aa_controller`](../dooya_55aa_controller/) instead.

## Hardware

| Item | Notes |
| :--- | :--- |
| ESP8266 (NodeMCU) | any ESP8266 board, 3.3 V logic |
| RS485 transceiver | MAX485 class; an auto-direction type needs no DE/RE pin |
| Motor 485 terminals | A / B, 9600 8N1 |

## Wiring

| ESP8266 | RS485 transceiver |
| :--- | :--- |
| D6 (GPIO12) | RO (MCU RX) |
| D7 (GPIO13) | DI (MCU TX) |
| 3.3 V / GND | VCC / GND |
| — | A / B to the motor's A / B |

With a DE/RE-style transceiver, tie DE and RE together and drive them from a
spare pin — direction handling is not in this sketch. An auto-direction
transceiver just works.

## Commands

Serial monitor at **115200**, one command per line:

| Command | Action |
| :--- | :--- |
| `o` | open / up |
| `c` | close / down |
| `s` | stop |
| `p50` | move to 50 % (`p0` … `p100`) |
| `q` | query the position right now |
| `d` | toggle the hex frame dump |
| `?` | print the help text |

## What it does on its own

* Polls the position every second (`POLL_INTERVAL_MS`) while idle — the motor
  never reports on its own on this protocol.
* Skips one poll right after a command (`POLL_HOLDOFF_MS`).
* Decodes the read reply: `55 <addr_L> <addr_H> 01 01 <percent> .. ..`; percent
  `0xFF` means the stroke has not been set and the motor refuses percentage
  commands. The sample output below uses the factory-default address `FE FE`.
* Prints a state line **only on a transition**: opening / closing is inferred
  from the position trend, and the state becomes stopped after
  `STATE_SETTLE_MS` without movement.

## Output

```
Dooya 485 controller - 9600 8N1 on the bus, polling every second

o      open / up
c      close / down
s      stop
p50    move to 50 %  (p0 ... p100)
q      query the position now
d      toggle the hex frame dump
?      this help

TX [7B] 55 FE FE 01 02 01 85 42
RX [8B] 55 FE FE 01 01 32 C5 A7
       -> reply, position 50%
> c
TX [7B] 55 FE FE 03 02 F9 25
       -> close
TX [7B] 55 FE FE 01 02 01 85 42
RX [8B] 55 FE FE 01 01 1E C4 7A
       -> reply, position 30%
ST state: closing
> d
       -> hex dump off
```

Polling frames are printed in hex but stay silent in text, and a reply whose
position has not changed is not repeated — a long-running monitor only speaks
up when something actually happens.

Rejected frames are reported instead of silently dropped:

```
       -> crc mismatch: recv=0x24B9 calc=0x24BA
       -> unexpected length 6, dropped
       -> reply from address 01 00, ignored
       -> stroke not set (0xFF): run learn mode on the motor
```

## Configuration

All at the top of the sketch:

| Macro | Default | Meaning |
| :--- | :--- | :--- |
| `MOTOR_RX` / `MOTOR_TX` | `D6` / `D7` | transceiver RO / DI |
| `FRAME_GAP_MS` | `6` | 3.5 character times at 9600 baud, frame end detection |
| `CMD_INTERVAL_MS` | `100` | minimum gap between two commands |
| `POLL_INTERVAL_MS` | `1000` | position query period |
| `POLL_HOLDOFF_MS` | `300` | pause after a command before polling again |
| `STATE_SETTLE_MS` | `2500` | no movement this long → "stopped" |
| `RX_BUF_SIZE` | `32` | receive buffer (a read reply is 8 bytes) |

## Protocol notes

* Frame: `55 | ID_L ID_H | function | data... | CRC_L CRC_H`. The function
  byte's high nibble selects the channel (`0xF` = all channels, control only);
  the low nibble is the command (read / write / control).
* The two bytes after `55` are the device address, low byte first. `FE FE` is
  the factory-default single-device address, not part of the frame format; on a
  bus with several motors, change each motor's address first — a frame to
  `FE FE` reaches every unmodified motor, and `00 00` is broadcast (never
  answered).
* CRC is CRC-16/MODBUS (poly `0x8005` reflected, init `0xFFFF`), **low byte
  first** — see the main README and [`crc/`](../../crc/).
* Official reference: Dooya《对外接口通用485协议》总则及分册（开合帘 / 百叶帘及卷帘）.

## Inside the sketch

| Part | Functions |
| :--- | :--- |
| CRC | `DooyaCrc16()` — bitwise CRC-16/MODBUS, low byte sent first |
| Transmit | `SendBody()` (appends the CRC, keeps `CMD_INTERVAL_MS` between frames), `SendPosition()` |
| Receive | `MotorTask()` (6 ms silence → one frame), `HandleFrame()` (address, length, CRC, decode) |
| State | `ApplyPosition()` — trend-based direction with settle timeout |
| Console | `HandleCommand()`, `PrintHelp()` |

## Build & Upload

- **Arduino IDE**: install the ESP8266 core, open `dooya_485_controller.ino`, select your board and upload.
- **PlatformIO**: `pio run -t upload` in this folder (see `platformio.ini`), then `pio device monitor`.

## 中文说明

一个工程包含驱动杜亚 485 电机的全部功能：发指令、轮询位置、解析应答、把收发报文按
十六进制打印。跑的是**杜亚统一串口协议**（官方公开发布文档），适合电机**直接提供 485
接口**的情况——开合帘、卷帘型号常见。如果电机只有 6P TTL 线（中间有转换模块），
请用 [`dooya_55aa_controller`](../dooya_55aa_controller/)。

**接线**：ESP8266 经 RS485 收发器（MAX485 类，自动收发型免方向控制）接电机 A/B——
D6(GPIO12) → RO，D7(GPIO13) → DI，3.3V/GND → VCC/GND。若用 DE/RE 型收发器，
把 DE、RE 短接后接一个空闲脚自行控制方向（本例程未处理）。总线 9600 8N1。

**命令**（串口 115200，一行一条）：`o` 开、`c` 关、`s` 停、`p50` 到 50%（`p0`…`p100`）、
`q` 立即查位置、`d` 切换十六进制报文打印、`?` 帮助。

**自动行为**：空闲时每秒查一次位置（该协议电机不会主动上报）；发完命令后停一次轮询；
解析读应答 `55 <地址_L> <地址_H> 01 01 <百分比>`（下文示例输出用的是出厂默认地址
`FE FE`），百分比 `0xFF` 表示未设行程（电机不执行百分比命令）；
**只在状态变化时**打印一行状态，方向按位置趋势推断，`STATE_SETTLE_MS` 无位移判停。

**协议要点**：帧 `55 | ID_L ID_H | 功能码 | 数据 | CRC_L CRC_H`；功能码高 4 位选通道
（`0xF` 群发，仅控制命令有效），低 4 位是命令类型；`55` 之后的两字节是设备地址
（低字节在前），`FE FE` 只是**出厂默认的单机地址**、不属于帧格式的一部分，多机共总
先改地址，`00 00` 是广播（不应答）；CRC 为 CRC-16/MODBUS（poly `0x8005`
反射、初值 `0xFFFF`），**低字节在前**——细节见主 README 与 [`crc/`](../../crc/)。
官方参考：杜亚《对外接口通用485协议》总则及分册。
