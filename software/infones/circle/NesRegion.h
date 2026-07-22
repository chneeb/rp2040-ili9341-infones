//
//  NesRegion.h
//
//  Reading the TV region out of a 16 byte iNES header.
//
//  Shared by the emulator side, which uses it to decide the frame period and
//  the sound rate, and the ROM menu, which uses it to label the list. One copy
//  of the bit twiddling so the two can never disagree about a file.
//
//  Deliberately free of both Circle and InfoNES headers - it is handed a raw
//  16 bytes and knows nothing else.
//
#pragma once

enum TNesRegion
{
	NesRegionUnknown,	// iNES 1.0: the header does not reliably say
	NesRegionNTSC,
	NesRegionPAL,
	NesRegionMulti,
	NesRegionDendy
};

// pHeader must point at the first 16 bytes of the file.
//
// Only a NES 2.0 header is believed. iNES 1.0 does have a PAL bit, at byte 9
// bit 0, but practically every dump in existence leaves it clear whatever the
// game is - so reading it would confidently mislabel PAL ROMs as NTSC far more
// often than it would help. Unknown is the honest answer and the safe one:
// nothing downstream changes behaviour on it.
//
// NES 2.0 is byte 7 bits 2-3 == 2, and then byte 12 bits 0-1 carry the region.
static inline enum TNesRegion NesRegionFromHeader (const unsigned char *pHeader)
{
	if (((pHeader[7] >> 2) & 3) != 2)
	{
		return NesRegionUnknown;
	}

	switch (pHeader[12] & 3)
	{
	case 0:		return NesRegionNTSC;
	case 1:		return NesRegionPAL;
	case 2:		return NesRegionMulti;
	default:	return NesRegionDendy;
	}
}

// One character for the ROM list. '?' rather than 'N' for an iNES 1.0 header:
// calling an unknown region NTSC would hide exactly the case worth seeing,
// which is a PAL game the header never admitted to.
static inline char NesRegionChar (enum TNesRegion Region)
{
	switch (Region)
	{
	case NesRegionNTSC:	return 'N';
	case NesRegionPAL:	return 'P';
	case NesRegionMulti:	return 'M';
	case NesRegionDendy:	return 'D';
	default:		return '?';
	}
}
