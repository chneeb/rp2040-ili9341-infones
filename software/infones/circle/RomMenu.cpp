//
//  RomMenu.cpp
//
#include "RomMenu.h"
#include "InputConfig.h"
#include "DisplayConfig.h"
#include "GamePi20.h"

#include <circle/util.h>
#include <fatfs/ff.h>
#include <circle/machineinfo.h>
#include <circle/string.h>

// InfoNES pad bits, as CKernel::ReadPad() reports them.
#define PAD_A		0x01
#define PAD_START	0x08
#define PAD_UP		0x10
#define PAD_DOWN	0x20

#define ROW_HEIGHT	16
#define TOP_MARGIN	28
#define LEFT_MARGIN	12

// Room kept clear at the bottom for the status line. C2DGraphics::DrawText
// draws nothing at all - not even clipped - if the glyph would run past the
// bottom edge, so the line has to fit with the font's underline included.
#define FOOTER_HEIGHT	20

#define COLOUR_BG	COLOR2D (0, 0, 40)
#define COLOUR_TEXT	COLOR2D (200, 200, 200)
#define COLOUR_CHOSEN	COLOR2D (255, 255, 255)
#define COLOUR_BAR	COLOR2D (0, 60, 160)
#define COLOUR_TITLE	COLOR2D (140, 190, 255)

CRomMenu::CRomMenu (CPanelDisplay *pDisplay)
:	m_pDisplay (pDisplay),
	m_Graphics (pDisplay),
	m_pDirectory (""),
	m_nCount (0),
	m_nSelected (0),
	m_nTop (0)
{
}

CRomMenu::~CRomMenu (void)
{
}

boolean CRomMenu::Initialize (void)
{
	return m_Graphics.Initialize ();
}

static boolean IsNesFile (const char *pName)
{
	size_t nLen = strlen (pName);
	if (nLen < 5)
	{
		return FALSE;
	}

	const char *pExt = pName + nLen - 4;

	return    pExt[0] == '.'
	       && (pExt[1] == 'n' || pExt[1] == 'N')
	       && (pExt[2] == 'e' || pExt[2] == 'E')
	       && (pExt[3] == 's' || pExt[3] == 'S');
}

// Read the region out of a ROM's header. Sixteen bytes per file, once, while
// the list is being built - not while drawing it.
static enum TNesRegion ReadRegion (const char *pDirectory, const char *pName)
{
	CString Path;
	Path.Format ("%s/%s", pDirectory, pName);

	FIL File;
	if (f_open (&File, Path, FA_READ) != FR_OK)
	{
		return NesRegionUnknown;
	}

	unsigned char Header[16];
	UINT nRead;
	boolean bOK =    f_read (&File, Header, sizeof Header, &nRead) == FR_OK
		      && nRead == sizeof Header;

	f_close (&File);

	return bOK ? NesRegionFromHeader (Header) : NesRegionUnknown;
}

unsigned CRomMenu::Scan (const char *pDirectory)
{
	m_pDirectory = pDirectory;
	m_nCount = 0;

	DIR Dir;
	if (f_opendir (&Dir, pDirectory) != FR_OK)
	{
		return 0;
	}

	FILINFO Info;
	while (m_nCount < MaxRoms)
	{
		if (f_readdir (&Dir, &Info) != FR_OK || Info.fname[0] == '\0')
		{
			break;			// end of directory
		}

		if (Info.fattrib & AM_DIR)
		{
			continue;
		}

		if (!IsNesFile (Info.fname))
		{
			continue;
		}

		strncpy (m_Names[m_nCount], Info.fname, MaxNameLength - 1);
		m_Names[m_nCount][MaxNameLength - 1] = '\0';
		m_Regions[m_nCount] = ReadRegion (pDirectory, Info.fname);
		m_nCount++;
	}

	f_closedir (&Dir);

	return m_nCount;
}

void CRomMenu::PushDisplay (void)
{
	// The panel copy is skipped in HDMI-only mode; the C2DGraphics buffer is
	// still drawn in memory, so the HDMI mirror below shows the menu regardless.
	if (GamePi20_PanelActive ())
	{
		m_Graphics.UpdateDisplay ();
	}

	// Mirror the same off-screen buffer to HDMI (no-op if not built / no
	// monitor). The buffer is the panel's RGB565 big-endian format, the same as
	// the emulator frame, so PresentHDMI's byte-swap applies unchanged.
	GamePi20_PresentHDMI ((const unsigned short *) m_Graphics.GetBuffer (),
			      m_Graphics.GetWidth (), m_Graphics.GetHeight ());
}

void CRomMenu::Draw (void)
{
	unsigned nWidth = m_Graphics.GetWidth ();
	unsigned nHeight = m_Graphics.GetHeight ();

	m_Graphics.ClearScreen (COLOUR_BG);

	m_Graphics.DrawText (nWidth / 2, 8, COLOUR_TITLE, "Select a game",
			     C2DGraphics::AlignCenter);


	// How many rows fit below the title.
	unsigned nRows = (nHeight - TOP_MARGIN - FOOTER_HEIGHT) / ROW_HEIGHT;

	// Keep the selection on screen.
	if (m_nSelected < m_nTop)
	{
		m_nTop = m_nSelected;
	}
	else if (m_nSelected >= m_nTop + nRows)
	{
		m_nTop = m_nSelected - nRows + 1;
	}

	for (unsigned i = 0; i < nRows && m_nTop + i < m_nCount; i++)
	{
		unsigned nIndex = m_nTop + i;
		unsigned nY = TOP_MARGIN + i * ROW_HEIGHT;

		if (nIndex == m_nSelected)
		{
			m_Graphics.DrawRect (0, nY - 2, nWidth, ROW_HEIGHT, COLOUR_BAR);
		}

		// Region first, in a fixed width column, so the names still line
		// up and the eye can run down the letters on their own.
		CString Row;
		Row.Format ("%c  %s", NesRegionChar (m_Regions[nIndex]),
			    (const char *) m_Names[nIndex]);

		m_Graphics.DrawText (LEFT_MARGIN, nY,
				     nIndex == m_nSelected ? COLOUR_CHOSEN : COLOUR_TEXT,
				     Row);
	}

	// What the panel is actually being clocked at. Circle divides the measured
	// core clock and truncates, so the rate that comes out depends on what the
	// firmware did with core_freq - which is worth being able to read rather
	// than assume.
	// The core clock is shown because it moves on its own; the bus rate is the
	// target, because CKernel::WaitForNextFrame() re-aims the divisor once a
	// second to hold it there whatever the core is doing.
	unsigned nCore = CMachineInfo::Get ()->GetClockRate (CLOCK_ID_CORE);
	unsigned nSPI = CPanelDisplay::TargetClock ();

	CString Info;
#if !SOUND_ENABLED
	Info.Format ("core %u  spi %u  %upx  %ufps  sound off",
		     nCore / 1000000, nSPI / 1000000, NES_OUT_WIDTH,
		     GamePi20_GetMeasuredFPS ());
#else
	if (GamePi20_IsMuted ())
	{
		Info.Format ("core %u  spi %u  %upx  %ufps  muted",
			     nCore / 1000000, nSPI / 1000000, NES_OUT_WIDTH,
			     GamePi20_GetMeasuredFPS ());
	}
	else
	{
		Info.Format ("core %u  spi %u  %upx  %ufps  vol %u",
			     nCore / 1000000, nSPI / 1000000, NES_OUT_WIDTH,
			     GamePi20_GetMeasuredFPS (), GamePi20_GetVolumeLevel ());
	}
#endif

	m_Graphics.DrawText (nWidth / 2, nHeight - FOOTER_HEIGHT, COLOUR_TEXT, Info,
			     C2DGraphics::AlignCenter);

	PushDisplay ();
}

const char *CRomMenu::Run (unsigned (*pReadPad) (void), void (*pWaitFrame) (void))
{
	if (m_nCount == 0)
	{
		m_Graphics.ClearScreen (COLOUR_BG);
		m_Graphics.DrawText (m_Graphics.GetWidth () / 2,
				     m_Graphics.GetHeight () / 2 - 8, COLOUR_TEXT,
				     "No .nes files in /nes",
				     C2DGraphics::AlignCenter);
		PushDisplay ();

		return nullptr;
	}

	// Act on the press, and require a release before acting again: this runs
	// once a frame, so a held button would otherwise run through the whole
	// list in well under a second.
	unsigned nPrevPad = pReadPad ();

	for (;;)
	{
		Draw ();

		// The status line shows these, so a change has to force a redraw:
		// the inner loop otherwise only redraws when the selection moves.
		unsigned nShownVolume = GamePi20_GetVolumeLevel ();
		int nShownMute = GamePi20_IsMuted ();

		for (;;)
		{
			pWaitFrame ();

			unsigned nPad = pReadPad ();

			if (   GamePi20_GetVolumeLevel () != nShownVolume
			    || GamePi20_IsMuted () != nShownMute)
			{
				nPrevPad = nPad;
				break;
			}
			unsigned nPressed = nPad & ~nPrevPad;
			nPrevPad = nPad;

			if (nPressed & PAD_UP)
			{
				if (m_nSelected > 0)
				{
					m_nSelected--;
				}
				break;
			}

			if (nPressed & PAD_DOWN)
			{
				if (m_nSelected + 1 < m_nCount)
				{
					m_nSelected++;
				}
				break;
			}

			if (nPressed & (PAD_A | PAD_START))
			{
				// FatFs wants the drive prefix on the path.
				strncpy (m_Path, m_pDirectory, sizeof m_Path - 1);
				m_Path[sizeof m_Path - 1] = '\0';

				size_t nLen = strlen (m_Path);
				if (nLen > 0 && m_Path[nLen - 1] != '/' && m_Path[nLen - 1] != ':')
				{
					strncat (m_Path, "/", sizeof m_Path - strlen (m_Path) - 1);
				}

				strncat (m_Path, m_Names[m_nSelected],
					 sizeof m_Path - strlen (m_Path) - 1);

				// Black out the whole panel on the way out. The
				// emulator only ever writes the 256 wide strip in
				// the middle, so without this the menu's background
				// would stay in the pillarbox bars either side.
				m_Graphics.ClearScreen (COLOR2D (0, 0, 0));
				PushDisplay ();

				return m_Path;
			}
		}
	}
}
