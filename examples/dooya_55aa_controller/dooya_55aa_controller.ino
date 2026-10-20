// Dooya 55 AA blind controller - every feature of this repository in one
// sketch (ESP8266).
//
//   - sends open / close / stop / set position / query frames
//   - polls the position once per second while idle
//   - decodes query replies and the unsolicited status reports
//   - prints every frame in hex (toggleable with 'd') and decodes the ones
//     that carry information
//
// Wiring (motor 6-pin cable, TX and RX must be crossed)
//   motor RX -> D7 (GPIO13)   MCU TX
//   motor TX -> D6 (GPIO12)   MCU RX
//   GND      -> GND           common ground is mandatory
//   3.3 V    -> out           directly powers a small ESP8266 at the connector
//   KEY UP / DOWN             leave open (short to GND to trigger)
//
// Serial monitor: 115200 baud, type ? for the command list.
//
// Note on D6 = GPIO12 (MTDI): it is a boot strap pin. If the motor's TX line
// holds it high at power-up, some ESP8266 modules fail to boot (the chip then
// expects 1.8 V flash). If that happens, power the motor after the MCU or move
// the wire to another pin.
//
// Protocol details, CRC and test vectors: main README, sections 4-7.

#include <SoftwareSerial.h>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

#define MOTOR_RX            D6      // motor TX line (MCU receives)
#define MOTOR_TX            D7      // motor RX line (MCU transmits)

#define FRAME_GAP_MS        6       // 3.5 character times at 9600 baud, rounded up
#define POLL_INTERVAL_MS    1000    // position query period
#define POLL_HOLDOFF_MS     300     // let a fresh command's report arrive first
#define STATE_SETTLE_MS     2500    // position unchanged this long -> stopped

#define RX_BUF_SIZE         64      // largest frame is 15 bytes
#define CMD_LINE_SIZE       16

// States are the protocol's own status byte values; 0 means "not known yet".
#define ST_UNKNOWN          0x00
#define ST_OPENING          0x01
#define ST_CLOSING          0x02
#define ST_STOPPED          0x03

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

SoftwareSerial MotorSerial(MOTOR_RX, MOTOR_TX);

static const uint8_t OPEN_BODY[]  = { 0x55, 0xAA, 0x05, 0x01, 0x01, 0x01 };
static const uint8_t CLOSE_BODY[] = { 0x55, 0xAA, 0x05, 0x01, 0x01, 0x02 };
static const uint8_t STOP_BODY[]  = { 0x55, 0xAA, 0x05, 0x01, 0x01, 0x03 };
static const uint8_t QUERY_BODY[] = { 0x55, 0xAA, 0x04, 0x02, 0x02 };

static uint8_t  RxBuf[RX_BUF_SIZE];
static size_t   RxLen = 0;
static uint32_t RxLastByteMs = 0;

static char     CmdLine[CMD_LINE_SIZE];
static size_t   CmdLen = 0;

static int      LastPosition = -1;          // -1 = unknown
static int      LastReplyPosition = -1;     // last position already printed
static uint32_t LastPositionChangeMs = 0;
static uint8_t  CurrentState = ST_UNKNOWN;
static uint32_t NextPollMs = 0;

static bool     DumpFrames = true;          // hex dump of every frame

// ---------------------------------------------------------------------------
// CRC-16 and framing
// ---------------------------------------------------------------------------

// Dooya 55AA CRC-16: poly 0x8005, init 0xFFFF, no reflection (MSB first),
// no final xor, stored high byte first. Same routine as crc/dooya_crc.c.
uint16_t DooyaCrc16(const uint8_t *pBuf, size_t len)
{
	uint16_t crc = 0xFFFF;

	for (size_t i = 0; i < len; i++)
	{
		crc ^= (uint16_t)pBuf[i] << 8;

		for (uint8_t bit = 0; bit < 8; bit++)
		{
			if (crc & 0x8000)
			{
				crc = (uint16_t)((crc << 1) ^ 0x8005);
			}
			else
			{
				crc = (uint16_t)(crc << 1);
			}
		}
	}

	return crc;
}

// Index of the first 0x55 0xAA start pair, or len when there is none.
size_t FindFrameStart(const uint8_t *pBuf, size_t len)
{
	for (size_t i = 0; (i + 1) < len; i++)
	{
		if ((pBuf[i] == 0x55) && (pBuf[i + 1] == 0xAA))
		{
			return i;
		}
	}

	return len;
}

void PrintHexLine(const char *pTag, const uint8_t *pBuf, size_t len)
{
	Serial.printf("%s [%uB]", pTag, (unsigned)len);

	for (size_t i = 0; i < len; i++)
	{
		Serial.printf(" %02X", pBuf[i]);
	}

	Serial.println();
}

const char *StateName(uint8_t state)
{
	switch (state)
	{
		case ST_OPENING: return "opening";
		case ST_CLOSING: return "closing";
		case ST_STOPPED: return "stopped";
		default:         return "unknown";
	}
}

// ---------------------------------------------------------------------------
// State and position bookkeeping
// ---------------------------------------------------------------------------

// Print only on a real transition: the position is polled every second, the
// same state would otherwise repeat forever.
void ApplyState(uint8_t state, const char *pSource)
{
	if (state == CurrentState)
	{
		return;
	}

	Serial.printf("ST state: %s -> %s (%s)\n",
		StateName(CurrentState), StateName(state), pSource);

	CurrentState = state;
}

// Query replies carry no state byte, so the direction is inferred from the
// position trend. A status report overrides it whenever one arrives.
void ApplyPosition(int position, const char *pSource)
{
	uint32_t now = millis();

	if (LastPosition < 0)
	{
		LastPosition = position;
		LastPositionChangeMs = now;
		return;
	}

	if (position != LastPosition)
	{
		ApplyState((position > LastPosition) ? ST_OPENING : ST_CLOSING, pSource);

		LastPosition = position;
		LastPositionChangeMs = now;
	}
	else if (((CurrentState == ST_OPENING) || (CurrentState == ST_CLOSING)) &&
		((now - LastPositionChangeMs) > STATE_SETTLE_MS))
	{
		ApplyState(ST_STOPPED, "position settled");
	}
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------

// Append the CRC (high byte first), transmit, then leave the line quiet.
void SendBody(const uint8_t *pBody, size_t bodyLen, const char *pWhat)
{
	uint8_t frame[16];
	uint16_t crc;

	if ((bodyLen + 2) > sizeof(frame))
	{
		return;
	}

	memcpy(frame, pBody, bodyLen);

	crc = DooyaCrc16(frame, bodyLen);
	frame[bodyLen] = (uint8_t)(crc >> 8);
	frame[bodyLen + 1] = (uint8_t)(crc & 0xFF);

	MotorSerial.write(frame, bodyLen + 2);

	if (DumpFrames)
	{
		PrintHexLine("TX", frame, bodyLen + 2);
	}

	if (pWhat != NULL)
	{
		Serial.printf("       -> %s\n", pWhat);
	}

	delay(FRAME_GAP_MS);

	// Do not poll straight after a command: the motor answers with a report of
	// its own and the two replies would interleave.
	NextPollMs = millis() + POLL_HOLDOFF_MS;
}

void SendPosition(int percent)
{
	uint8_t body[7];
	char what[24];

	if (percent < 0)
	{
		percent = 0;
	}

	if (percent > 100)
	{
		percent = 100;
	}

	body[0] = 0x55;
	body[1] = 0xAA;
	body[2] = 0x06;                     // length field
	body[3] = 0x01;                     // control
	body[4] = 0x02;                     // set position
	body[5] = (uint8_t)percent;
	body[6] = 0x67;                     // angle parameter, constant on roller blinds

	snprintf(what, sizeof(what), "set position %d%%", percent);
	SendBody(body, sizeof(body), what);
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------

void HandleFrame(const uint8_t *pBuf, size_t len)
{
	size_t start = FindFrameStart(pBuf, len);

	if (DumpFrames)
	{
		PrintHexLine("RX", pBuf, len);
	}

	if ((start + 3) > len)
	{
		Serial.println("       -> no 55 AA start pair, dropped");
		return;
	}

	size_t frameLen = (size_t)pBuf[start + 2] + 3;

	if ((start + frameLen) > len)
	{
		Serial.printf("       -> truncated: length field says %u, got %u\n",
			(unsigned)frameLen, (unsigned)(len - start));
		return;
	}

	uint16_t recv = ((uint16_t)pBuf[start + frameLen - 2] << 8) | pBuf[start + frameLen - 1];
	uint16_t calc = DooyaCrc16(pBuf + start, frameLen - 2);

	if (recv != calc)
	{
		Serial.printf("       -> crc mismatch: recv=0x%04X calc=0x%04X\n", recv, calc);
		return;
	}

	uint8_t func = pBuf[start + 3];

	if ((func == 0x02) && (frameLen == 8))
	{
		int position = pBuf[start + 5];

		if (position != LastReplyPosition)
		{
			Serial.printf("       -> reply, position %d%%\n", position);
			LastReplyPosition = position;
		}

		ApplyPosition(position, "reply trend");
	}
	else if ((func == 0x03) && (frameLen >= 9))
	{
		uint8_t state = pBuf[start + 5];
		int position = pBuf[start + 6];
		uint8_t angle = pBuf[start + 7];

		if (state > ST_STOPPED)
		{
			state = ST_UNKNOWN;
		}

		Serial.printf("       -> report, %s, position %d%%, angle 0x%02X\n",
			StateName(state), position, angle);

		LastPosition = position;
		LastPositionChangeMs = millis();
		ApplyState(state, "status report");
	}
	else
	{
		Serial.printf("       -> function 0x%02X, %u bytes, not decoded\n",
			func, (unsigned)frameLen);
	}
}

// Collect bytes until the line has been quiet for FRAME_GAP_MS, then hand the
// whole burst over as one frame.
void MotorTask()
{
	while (MotorSerial.available())
	{
		uint8_t byte = (uint8_t)MotorSerial.read();

		if (RxLen < sizeof(RxBuf))
		{
			RxBuf[RxLen++] = byte;
		}

		RxLastByteMs = millis();
	}

	if ((RxLen != 0) && ((millis() - RxLastByteMs) > FRAME_GAP_MS))
	{
		HandleFrame(RxBuf, RxLen);
		RxLen = 0;
	}
}

// ---------------------------------------------------------------------------
// Command console
// ---------------------------------------------------------------------------

void PrintHelp()
{
	Serial.println("o      open / up");
	Serial.println("c      close / down");
	Serial.println("s      stop");
	Serial.println("p50    move to 50 %  (p0 ... p100)");
	Serial.println("q      query the position now");
	Serial.println("d      toggle the hex frame dump");
	Serial.println("?      this help");
}

void HandleCommand(const char *pLine)
{
	switch (pLine[0])
	{
		case 'o':
		case 'O':
			SendBody(OPEN_BODY, sizeof(OPEN_BODY), "open");
			break;

		case 'c':
		case 'C':
			SendBody(CLOSE_BODY, sizeof(CLOSE_BODY), "close");
			break;

		case 's':
		case 'S':
			SendBody(STOP_BODY, sizeof(STOP_BODY), "stop");
			break;

		case 'q':
		case 'Q':
			SendBody(QUERY_BODY, sizeof(QUERY_BODY), "query position");
			break;

		case 'p':
		case 'P':
			if (pLine[1] == '\0')
			{
				PrintHelp();
			}
			else
			{
				SendPosition(atoi(pLine + 1));
			}
			break;

		case 'd':
		case 'D':
			DumpFrames = !DumpFrames;
			Serial.printf("       -> hex dump %s\n", DumpFrames ? "on" : "off");
			break;

		default:
			PrintHelp();
			break;
	}
}

// ---------------------------------------------------------------------------

void setup()
{
	Serial.begin(115200);
	MotorSerial.begin(9600);

	Serial.println();
	Serial.println("Dooya 55AA controller - 9600 8N1, polling every second");
	Serial.println();
	PrintHelp();
	Serial.println();

	NextPollMs = millis() + POLL_INTERVAL_MS;
}

void loop()
{
	MotorTask();

	while (Serial.available())
	{
		char c = (char)Serial.read();

		if ((c == '\r') || (c == '\n'))
		{
			if (CmdLen != 0)
			{
				CmdLine[CmdLen] = '\0';
				HandleCommand(CmdLine);
				CmdLen = 0;
			}
		}
		else if (CmdLen < (sizeof(CmdLine) - 1))
		{
			CmdLine[CmdLen++] = c;
		}
	}

	if ((int32_t)(millis() - NextPollMs) >= 0)
	{
		NextPollMs = millis() + POLL_INTERVAL_MS;
		SendBody(QUERY_BODY, sizeof(QUERY_BODY), NULL);
	}
}
