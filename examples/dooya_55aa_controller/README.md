# Example - Dooya 55 AA Blind Controller

One sketch with everything this repository documents: send commands, poll the
position, decode the motor's replies and its unsolicited status reports, and
print the traffic in hex. No WiFi, no cloud — just the serial console, so it
works on a bare ESP8266 and is easy to graft onto a bigger firmware later.

## Hardware

| Item | Notes |
| :--- | :--- |
| ESP8266 (NodeMCU) | any ESP8266 board, 3.3 V logic |
| Motor 6-pin cable | 9600 8N1, 3.3 V level |

## Wiring

**TX and RX must be crossed.**

| Motor signal | ESP8266 pin |
| :--- | :--- |
| motor RX | D7 (GPIO13), MCU TX |
| motor TX | D6 (GPIO12), MCU RX |
| GND | GND |
| 3.3 V out | — directly powers a small ESP8266 hung on the connector |
| KEY UP / KEY DOWN | leave open |

`D6 = GPIO12` (MTDI) is a boot strap pin: if the motor's TX line holds it high
at power-up, some ESP8266 modules fail to boot. If that happens, power the
motor after the MCU or move the wire to another pin.

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

* Polls the position every second (`POLL_INTERVAL_MS`) while idle, so the
  current percentage is always known — the motor never reports while sitting
  still.
* Skips one poll right after a command (`POLL_HOLDOFF_MS`) so the command's own
  status report does not collide with a query reply.
* Decodes the status reports: `0x01` opening, `0x02` closing, `0x03` stopped.
* Prints a state line **only on a transition**. When only query replies are
  available (no report), the direction is inferred from the position trend and
  treated as stopped after `STATE_SETTLE_MS` without movement.

## Output

```
Dooya 55AA controller - 9600 8N1, polling every second

o      open / up
c      close / down
s      stop
p50    move to 50 %  (p0 ... p100)
q      query the position now
d      toggle the hex frame dump
?      this help

TX [7B] 55 AA 04 02 02 38 74
RX [8B] 55 AA 05 02 02 32 E0 3F
       -> reply, position 50%
> c
TX [8B] 55 AA 05 01 01 02 EA A3
       -> close
RX [15B] 55 AA 0C 03 01 02 00 0B 01 00 02 00 00 DB 3B
       -> report, closing, position 0%, angle 0x0B
ST state: unknown -> closing (status report)
> p100
TX [9B] 55 AA 06 01 02 64 67 74 9F
       -> set position 100%
> d
       -> hex dump off
```

Polling frames are printed in hex but stay silent in text, and a reply whose
position has not changed is not repeated — a long-running monitor therefore only
speaks up when something actually happens. `d` turns the hex dump off entirely.

Rejected frames are reported instead of silently dropped:

```
       -> crc mismatch: recv=0x2ABB calc=0x2ABC
       -> truncated: length field says 15, got 11
       -> no 55 AA start pair, dropped
```

## Configuration

All at the top of the sketch:

| Macro | Default | Meaning |
| :--- | :--- | :--- |
| `MOTOR_RX` / `MOTOR_TX` | `D6` / `D7` | motor TX / RX lines (pinout: `img/rj11.png` in the main README) |
| `FRAME_GAP_MS` | `6` | 3.5 character times at 9600 baud, frame end detection |
| `POLL_INTERVAL_MS` | `1000` | position query period |
| `POLL_HOLDOFF_MS` | `300` | pause after a command before polling again |
| `STATE_SETTLE_MS` | `2500` | no movement this long → "stopped" |
| `RX_BUF_SIZE` | `64` | receive buffer (largest frame is 15 bytes) |

## Inside the sketch

| Part | Functions |
| :--- | :--- |
| CRC and framing | `DooyaCrc16()`, `FindFrameStart()`, `PrintHexLine()` |
| Transmit | `SendBody()` (appends the CRC high byte first), `SendPosition()` |
| Receive | `MotorTask()` (6 ms silence → one frame), `HandleFrame()` (validate, decode `0x02` / `0x03`) |
| State | `ApplyPosition()`, `ApplyState()` — single place that decides what the blind is doing |
| Console | `HandleCommand()`, `PrintHelp()` |

The receive path never half-parses: start pair, length field and CRC are checked
before anything is interpreted.

## Build & Upload

- **Arduino IDE**: install the ESP8266 core, open `dooya_55aa_controller.ino`, select your board and upload.
- **PlatformIO**: `pio run -t upload` in this folder (see `platformio.ini`), then `pio device monitor`.

## 中文说明

一个工程包含本仓库的全部功能：发指令、轮询位置、解析电机应答与主动上报、把收发报文按十六进制
打印。不连 WiFi、不走云，只靠串口控制台，裸 ESP8266 就能跑，也方便以后并进更大的固件。

**接线**（TX / RX 必须交叉）：电机 RX → D7(GPIO13)，电机 TX → D6(GPIO12)，GND → GND；
3.3V 输出可直接给挂在水晶头上的小 ESP8266 供电；两个按键线悬空。
各线引脚定义见主 README 的 `img/rj11.png`。`D6 = GPIO12`（MTDI）是启动引导脚，
上电若被电机 TX 拉高，部分模块会启动失败；遇到就让电机晚上电，或换到别的脚。

**命令**（串口 115200，一行一条）：`o` 开、`c` 关、`s` 停、`p50` 到 50%（`p0`…`p100`）、
`q` 立即查位置、`d` 切换十六进制报文打印、`?` 帮助。

**自动行为**：

* 空闲时每秒查一次位置（`POLL_INTERVAL_MS`）——电机静止时不会主动上报，只能轮询。
* 发完命令后停一次轮询（`POLL_HOLDOFF_MS`），避免命令自带的上报和查询应答撞在一起。
* 解析主动上报：`0x01` 开、`0x02` 关、`0x03` 停。
* **只在状态变化时**打印一行状态。只有应答没有上报时，方向按位置趋势推断，
  `STATE_SETTLE_MS` 内没有位移就判为停止。

**输出策略**：轮询帧只打十六进制、不打文字；位置没变的应答不重复打印；所以长时间挂着只在真有事时说话。
`d` 可把十六进制整段关掉。校验失败的帧不会被静默丢弃，会打出
`crc mismatch` / `truncated` / `no 55 AA start pair` 三种结论之一。

**可调参数**：`MOTOR_RX`/`MOTOR_TX`、`FRAME_GAP_MS`(6)、`POLL_INTERVAL_MS`(1000)、
`POLL_HOLDOFF_MS`(300)、`STATE_SETTLE_MS`(2500)、`RX_BUF_SIZE`(64)，都在文件开头。
