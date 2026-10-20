#include "dooya_crc.h"

uint16_t dooya_crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xFFFFu;
	size_t i;
	uint8_t bit;

	if (data == NULL)
	{
		return crc;
	}

	for (i = 0; i < len; i++)
	{
		crc ^= (uint16_t)data[i] << 8;

		for (bit = 0; bit < 8; bit++)
		{
			if (crc & 0x8000u)
			{
				crc = (uint16_t)((crc << 1) ^ 0x8005u);
			}
			else
			{
				crc = (uint16_t)(crc << 1);
			}
		}
	}

	return crc;
}

size_t dooya_frame_finish(uint8_t *buf, size_t bodyLen, size_t bufSize)
{
	uint16_t crc;

	if ((buf == NULL) || (bufSize < (bodyLen + 2)))
	{
		return 0;
	}

	crc = dooya_crc16(buf, bodyLen);

	/* High byte first - this is where Modbus-shaped code usually goes wrong */
	buf[bodyLen] = (uint8_t)(crc >> 8);
	buf[bodyLen + 1] = (uint8_t)(crc & 0xFFu);

	return bodyLen + 2;
}
