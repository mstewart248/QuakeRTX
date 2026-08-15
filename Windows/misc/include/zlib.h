/*
	Minimal zlib.h shim for the MSVC build.

	Quake/ice/ice_main.c includes <zlib.h> solely for crc32(), which it uses to
	compute the STUN FINGERPRINT attribute (RFC 5389 s15.5). Nothing else in the
	tree references zlib, so vendoring the full library would be disproportionate.

	This provides the standard IEEE 802.3 CRC-32 that zlib's crc32() computes,
	with a matching signature. If real zlib is ever added to the build, delete
	this file -- the include path will pick up the genuine header instead.
*/

#ifndef QS_MINIMAL_ZLIB_SHIM_H
#define QS_MINIMAL_ZLIB_SHIM_H

static unsigned long crc32 (unsigned long crc, const unsigned char *buf, unsigned int len)
{
	crc = crc ^ 0xFFFFFFFFUL;

	while (len--)
	{
		int k;
		crc ^= (unsigned long) *buf++;
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0UL);
	}

	return (crc ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL;
}

#endif /* QS_MINIMAL_ZLIB_SHIM_H */
