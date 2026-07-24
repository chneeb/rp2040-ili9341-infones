//
//  RomMenu.h
//
//  Picks a .nes file off the SD card.
//
//  Drawn with C2DGraphics rather than by hand: ST7789DMADisplay is a CDisplay,
//  so C2DGraphics attaches straight to it, and that brings DrawText and a
//  built-in font with it. The emulator itself does not use C2DGraphics - it
//  hands its frame to the display directly - so the two never draw at once.
//
#pragma once

#include <circle/2dgraphics.h>
#include <circle/types.h>

#include "DisplayConfig.h"
#include "NesRegion.h"

class CRomMenu
{
public:
	static const unsigned MaxRoms = 128;
	static const unsigned MaxNameLength = 64;

	CRomMenu (CPanelDisplay *pDisplay);
	~CRomMenu (void);

	boolean Initialize (void);

	/// \brief Read the .nes files in pDirectory.
	/// \return Number of ROMs found
	unsigned Scan (const char *pDirectory);

	/// \brief Show the list and let the player choose.
	/// \param pPadState Called once per frame, returns InfoNES pad bits
	/// \return Full path of the chosen ROM, or nullptr if there was nothing
	const char *Run (unsigned (*pReadPad) (void), void (*pWaitFrame) (void));

private:
	void Draw (void);

	CPanelDisplay *m_pDisplay;
	C2DGraphics m_Graphics;

	char m_Names[MaxRoms][MaxNameLength];

	// One per entry, read from each file's header during Scan(). Shown as a
	// letter in the list so a PAL game is recognisable before launching it.
	enum TNesRegion m_Regions[MaxRoms];
	char m_Path[MaxNameLength * 2];
	const char *m_pDirectory;

	unsigned m_nCount;
	unsigned m_nSelected;
	unsigned m_nTop;		// first row on screen, for scrolling
};
