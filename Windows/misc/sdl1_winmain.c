/*
	WinMain entry point for the SDL 1.2 build.

	SDL 1.2's SDL_main.h renames main() to SDL_main() on Windows and expects
	SDLmain.lib to supply the real WinMain. That prebuilt library is VC6-era
	and references __iob_func, which the modern UCRT removed, so linking it
	against the v143 toolset fails.

	Supplying WinMain ourselves is a few lines and avoids dragging in a CRT
	compatibility shim just to satisfy an obsolete import.
*/

#include <windows.h>
#include <stdlib.h>

extern int SDL_main (int argc, char *argv[]);

int WINAPI WinMain (HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	(void) hInstance;
	(void) hPrevInstance;
	(void) lpCmdLine;
	(void) nCmdShow;

	/* MSVC pre-parses the command line into these for us. */
	return SDL_main (__argc, __argv);
}
