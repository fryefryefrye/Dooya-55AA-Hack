// Dooya 485 blind controller - the official "Dooya Unified Serial Protocol"
// (杜亚统一串口协议) over RS485, in one sketch (ESP8266).
//
// Dooya publishes an official document for this protocol. Motors that expose
// an RS485 interface directly - open-close curtain and roller blind models -
// accept these frames without any translator in between.
//
//   - sends open / close / stop / set position / read position frames
//   - polls the position once per second while idle
//   - decodes the read replies (percent; 0xFF = stroke not set)
//   - prints every frame in hex (toggleable with 'd') and decodes the ones
//     that carry information
//
// Frame layout: 55 | ID_L ID_H | function | data... | CRC_L CRC_H
//   function high nibble = channel (0x0..0xE selects one, 0xF = all channels,
//                          group control, only valid for control commands)
//   function low nibble  = command (0x01 read / 0x02 write / 0x03 control)
//   FE FE = factory-default single-device address; 00 00 = broadcast (a
//           broadcasted frame is never answered). Two motors on one bus must
//           be given different addresses first.
//   CRC-16/MODBUS (poly 0x8005, reflected, init 0xFFFF), low byte first,
//   covering from the start byte to the last data byte.
//
// Frames on the wire (CRC is computed at run time):
//   open         55 FE FE 03 01 B9 24
//   close        55 FE FE 03 02 F9 25
//   pause        55 FE FE 03 03 38 E5
//   set position 55 FE FE 03 04 <percent> .. ..
//   read         55 FE FE 01 02 01 85 42
//   reply        55 FE FE 01 01 <percent> .. ..   (percent 0xFF = stroke not set)
//
// Wiring: ESP8266 -> RS485 transceiver (MAX485 class, auto-direction) -> motor A/B.
//   D6 (GPIO12) = transceiver RO (MCU RX)
//   D7 (GPIO13) = transceiver DI (MCU TX)
//   Bus: 9600 8N1. With a DE/RE-style transceiver, tie DE and RE together and
//   drive them from a spare pin (direction handling is not in this sketch).
//
// Serial monitor: 115200 baud, type ? for the command list.
//
// Official protocol reference: Dooya "对外接口通用485协议" (总则 / 开合帘 /
// 百叶帘及卷帘). CRC details: main README and crc/.

#include <SoftwareSerial.h>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

#define MOTOR_RX            D6      // transceiver RO (MCU receives)
#define MOTOR_TX            D7      // transceiver DI (MCU transmits)

#define FRAME_GAP_MS        6       // 3.5 character times at 9600 baud, frame end
#define CMD_INTERVAL_MS     100     // minimum gap between two commands
#define POLL_INTERVAL_MS    1000    // position query period
#define POLL_HOLDOFF_MS     300     // pause after a command before polling again
#define STATE_SETTLE_MS     2500    // position unchanged this long -> stopped

#define RX_BUF_SIZE         32      // a read reply is 8 bytes
#define CMD_LINE_SIZE       16

// States used for the status line; 0 means "not known yet".
#define ST_UNKNOWN          0x00
#define ST_OPENING          0x01
#define ST_CLOSING          0x02
#define ST_STOPPED          0x03

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

SoftwareSerial MotorSerial(MOTOR_RX, MOTOR_TX);

// Command bodies without the trailing CRC (appended at send time).
static const uint8_t OPEN_BODY[]     = { 0x55, 0xFE, 0xFE, 0x03, 0x01 };
static const uint8_t CLOSE_BODY[]    = { 0x55, 0xFE, 0xFE, 0x03, 0x02 };
static const uint8_t STOP_BODY[]     = { 0x55, 0xFE, 0xFE, 0x03, 0x03 };
static const uint8_t POSITION_BODY[] = { 0x55, 0xFE, 0xFE, 0x03, 0x04, 0x00 }; // [5] = percent
static const uint8_t QUERY_BODY[]    = { 0x55, 0xFE, 0xFE, 0x01, 0x02, 0x01 }; // read register 0x02

static bool     HexDumpOn = true;
static uint32_t LastCmdMillis = 0;

static uint8_t  RxBuf[RX_BUF_SIZE];
static uint8_t  RxLen = 0;
static uint32_t LastRxByteMillis = 0;

static int      LastPosition = -1;      // -1 = unknown
static int      LastReplyPosition = -1; // last reply position already printed
static bool     StrokeWarned = false;   // warned about 0xFF (no stroke set)
static uint8_t  State = ST_UNKNOWN;
static uint32_t LastMoveMillis = 0;

// ---------------------------------------------------------------------------
// CRC-16/MODBUS: poly 0x8005 reflected (0xA001), init 0xFFFF, no final XOR.
// Same result as the table-driven version in the reference firmware.
// ---------------------------------------------------------------------------

static uint16_t DooyaCrc16(const uint8_t *data, uint8_t len)
{
	uint16_t crc = 0xFFFFu;

	for (uint8_t i = 0; i < len; i++)
	{
		crc ^= data[i];

		for (uint8_t bit = 0; bit < 8; bit++)
		{
			if (crc & 0x0001u)
			{
				crc = (uint16_t)((crc >> 1) ^ 0xA001u);
			}
			else
			{
				crc = (uint16_t)(crc >> 1);
			}
		}
	}

	return crc;
}

// ---------------------------------------------------------------------------
// Hex output
// ---------------------------------------------------------------------------

static void PrintHexLine(const char *pTag, const uint8_t *pData, uint8_t len)
{
	Serial.print(pTag);
	Serial.print(" [");
	Serial.print(len);
	Serial.print("B]");

	for (uint8_t i = 0; i < len; i++)
	{
		Serial.printf(" %02X", pData[i]);
	}

	Serial.print("\n");
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------

static void SendBody(const uint8_t *pBody, uint8_t len, const char *pWhat)
{
	uint8_t frame[16];

	if ((size_t)(len + 2) > sizeof(frame))
	{
		return;
	}

	// Two commands too close together can be glued into one frame by the motor.
	uint32_t sinceLast = millis() - LastCmdMillis;
	if (sinceLast < CMD_INTERVAL_MS)
	{
		delay(CMD_INTERVAL_MS - sinceLast);
	}

	memcpy(frame, pBody, len);
	uint16_t crc = DooyaCrc16(frame, len);
	frame[len] = (uint8_t)(crc & 0xFFu);        // CRC low byte first
	frame[len + 1] = (uint8_t)(crc >> 8);

	MotorSerial.write(frame, len + 2);
	LastCmdMillis = millis();

	if (HexDumpOn)
	{
		PrintHexLine("TX", frame, len + 2);
	}

	if (pWhat != NULL)
	{
		Serial.printf("       -> %s\n", pWhat);
	}
}

// ---------------------------------------------------------------------------
// Receive path
// ---------------------------------------------------------------------------

static void ApplyPosition(int position)
{
	if (LastPosition == -1)
	{
		// First valid value: judge by the percent alone, no direction yet.
		State = ST_STOPPED;
	}
	else if (position > LastPosition)
	{
		State = ST_OPENING;
	}
	else if (position < LastPosition)
	{
		State = ST_CLOSING;
	}
	else if ((State == ST_OPENING || State == ST_CLOSING) &&
	         (millis() - LastMoveMillis) > STATE_SETTLE_MS)
	{
		State = ST_STOPPED;
	}

	if (position != LastPosition)
	{
		LastMoveMillis = millis();
	}

	// Print only on a real state transition, not on every poll.
	static const char *pNames[] = { "unknown", "opening", "closing", "stopped" };
	static uint8_t LastPrinted = ST_UNKNOWN;
	if (State != LastPrinted)
	{
		Serial.printf("ST state: %s\n", pNames[State]);
		LastPrinted = State;
	}

	LastPosition = position;
}

static void HandleFrame(const uint8_t *pBuf, uint8_t len)
{
	if (pBuf[0] != 0x55)
	{
		Serial.printf("       -> no 55 start byte, dropped\n");
		return;
	}

	if (len != 8)
	{
		Serial.printf("       -> unexpected length %d, dropped\n", len);
		return;
	}

	if (pBuf[1] != 0xFE || pBuf[2] != 0xFE)
	{
		Serial.printf("       -> reply from address %02X %02X, ignored\n", pBuf[1], pBuf[2]);
		return;
	}

	uint16_t recvCrc = (uint16_t)(pBuf[6] | ((uint16_t)pBuf[7] << 8));  // low byte first
	uint16_t calcCrc = DooyaCrc16(pBuf, 6);
	if (recvCrc != calcCrc)
	{
		Serial.printf("       -> crc mismatch: recv=0x%04X calc=0x%04X\n", recvCrc, calcCrc);
		return;
	}

	// Read reply: 55 | FE FE | 01 | 01 | percent | CRC_L CRC_H
	if (pBuf[3] != 0x01 || pBuf[4] != 0x01)
	{
		Serial.printf("       -> not a read reply (function 0x%02X), dropped\n", pBuf[3]);
		return;
	}

	int position = pBuf[5];

	if (position == 0xFF)
	{
		// 0xFF = stroke not set: the percent is meaningless and the motor
		// refuses percentage commands until the stroke has been learned.
		if (!StrokeWarned)
		{
			Serial.printf("       -> stroke not set (0xFF): run learn mode on the motor\n");
			StrokeWarned = true;
		}
		LastReplyPosition = -1;
		return;
	}

	if (position != LastReplyPosition)
	{
		Serial.printf("       -> reply, position %d%%\n", position);
		LastReplyPosition = position;
	}

	ApplyPosition(position);
}

static void MotorTask(void)
{
	while (MotorSerial.available() > 0)
	{
		uint8_t byteIn = (uint8_t)MotorSerial.read();

		if (RxLen >= RX_BUF_SIZE)
		{
			// Overlong frame: drop and resync instead of overrunning the buffer.
			RxLen = 0;
		}

		RxBuf[RxLen++] = byteIn;
		LastRxByteMillis = millis();
	}

	// Frames are delimited by line silence: 3.5 character times at 9600 baud.
	if ((RxLen != 0) && ((millis() - LastRxByteMillis) > FRAME_GAP_MS))
	{
		if (HexDumpOn)
		{
			PrintHexLine("RX", RxBuf, RxLen);
		}

		HandleFrame(RxBuf, RxLen);
		RxLen = 0;
	}
}

// ---------------------------------------------------------------------------
// Serial console
// ---------------------------------------------------------------------------

static void PrintHelp(void)
{
	Serial.printf("o      open / up\n");
	Serial.printf("c      close / down\n");
	Serial.printf("s      stop\n");
	Serial.printf("p50    move to 50%%  (p0 ... p100)\n");
	Serial.printf("q      query the position now\n");
	Serial.printf("d      toggle the hex frame dump\n");
	Serial.printf("?      this help\n");
}

static void SendPosition(uint8_t percent)
{
	uint8_t body[6];
	memcpy(body, POSITION_BODY, sizeof(POSITION_BODY));
	body[5] = percent;
	SendBody(body, sizeof(body), NULL);
	Serial.printf("       -> set position %u%%\n", percent);
}

static void HandleCommand(const char *pCmd)
{
	if (pCmd[0] == 0)
	{
		return;
	}
	else if (strcmp(pCmd, "?") == 0)
	{
		PrintHelp();
	}
	else if (strcmp(pCmd, "o") == 0)
	{
		SendBody(OPEN_BODY, sizeof(OPEN_BODY), "open");
	}
	else if (strcmp(pCmd, "c") == 0)
	{
		SendBody(CLOSE_BODY, sizeof(CLOSE_BODY), "close");
	}
	else if (strcmp(pCmd, "s") == 0)
	{
		SendBody(STOP_BODY, sizeof(STOP_BODY), "stop");
	}
	else if (strcmp(pCmd, "q") == 0)
	{
		SendBody(QUERY_BODY, sizeof(QUERY_BODY), "query position");
	}
	else if (strcmp(pCmd, "d") == 0)
	{
		HexDumpOn = !HexDumpOn;
		Serial.printf("       -> hex dump %s\n", HexDumpOn ? "on" : "off");
	}
	else if (pCmd[0] == 'p')
	{
		int percent = atoi(pCmd + 1);
		if (percent < 0)   percent = 0;
		if (percent > 100) percent = 100;
		SendPosition((uint8_t)percent);
	}
	else
	{
		Serial.printf("unknown command: %s (? for help)\n", pCmd);
	}
}

static void ConsoleTask(void)
{
	static char cmdLine[CMD_LINE_SIZE];
	static uint8_t cmdLen = 0;

	while (Serial.available() > 0)
	{
		char ch = (char)Serial.read();

		if (ch == '\r' || ch == '\n')
		{
			if (cmdLen != 0)
			{
				cmdLine[cmdLen] = 0;
				Serial.printf("> %s\n", cmdLine);
				HandleCommand(cmdLine);
				cmdLen = 0;
			}
		}
		else if (cmdLen < CMD_LINE_SIZE - 1)
		{
			cmdLine[cmdLen++] = ch;
		}
	}
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------

static uint32_t LastPollMillis = 0;

void setup()
{
	Serial.begin(115200);
	Serial.printf("\nDooya 485 controller - 9600 8N1 on the bus, polling every second\n\n");
	PrintHelp();
	Serial.print("\n");

	MotorSerial.begin(9600);
	LastCmdMillis = millis();
}

void loop()
{
	MotorTask();
	ConsoleTask();

	// Poll the position every second while idle. A motor never reports on its
	// own on this protocol, so polling is the only way to know the percent.
	if ((millis() - LastPollMillis) > POLL_INTERVAL_MS &&
	    (millis() - LastCmdMillis) > POLL_HOLDOFF_MS)
	{
		LastPollMillis = millis();
		SendBody(QUERY_BODY, sizeof(QUERY_BODY), NULL);
	}
}
