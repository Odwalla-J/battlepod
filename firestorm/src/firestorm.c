/* Firestorm.exe: open the launcher of a packaged copy, double-clicked.
 *
 * `make package` lays a copy out as
 *
 *     Firestorm.exe     this
 *     python\           an embeddable Python, nothing installed
 *     launcher\         fslauncher.py and its page
 *     tools\            start-centre.ps1 and desk.py, for a test match
 *     game\             MW4 with the shim's ddraw.dll and fsrun.exe
 *
 * and this runs python\pythonw.exe launcher\fslauncher.py --game game, with
 * python\ first on PATH so the test match's scripts find the same Python.
 * Arguments are passed on to the launcher. Named "BattleTech 3.0.exe" (the
 * same program, copied), it opens the launcher on BattleTech 3.0's page.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static char dir[MAX_PATH];

static int missing(const char *what)
{
	char msg[MAX_PATH + 200];
	snprintf(msg, sizeof msg, "%s%s is missing.\n\nThis copy is incomplete: run make package again.", dir, what);
	MessageBoxA(NULL, msg, "Firestorm", MB_ICONERROR);
	return 1;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR args, int show)
{
	char py[MAX_PATH + 32], cmd[4096], path[32768], *slash;
	STARTUPINFOA si = { sizeof si };
	PROCESS_INFORMATION pi;
	DWORD n;

	GetModuleFileNameA(NULL, dir, sizeof dir);
	if ((slash = strrchr(dir, '\\'))) slash[1] = 0;
	snprintf(py, sizeof py, "%spython\\pythonw.exe", dir);
	if (GetFileAttributesA(py) == INVALID_FILE_ATTRIBUTES) return missing("python\\pythonw.exe");
	snprintf(cmd, sizeof cmd, "%slauncher\\fslauncher.py", dir);
	if (GetFileAttributesA(cmd) == INVALID_FILE_ATTRIBUTES) return missing("launcher\\fslauncher.py");
	snprintf(cmd, sizeof cmd, "%sgame\\MW4.exe", dir);
	if (GetFileAttributesA(cmd) == INVALID_FILE_ATTRIBUTES) return missing("game\\MW4.exe");

	n = snprintf(path, sizeof path, "%spython;", dir);
	GetEnvironmentVariableA("PATH", path + n, sizeof path - n);
	SetEnvironmentVariableA("PATH", path);

	{
		char self[MAX_PATH], *base;
		GetModuleFileNameA(NULL, self, sizeof self);
		base = strrchr(self, '\\') ? strrchr(self, '\\') + 1 : self;
		snprintf(cmd, sizeof cmd, "\"%s\" \"%slauncher\\fslauncher.py\" --game \"%sgame\"%s %s", py, dir, dir,
			 _strnicmp(base, "BattleTech", 10) ? "" : " --page bt3", args);
	}
	if (!CreateProcessA(py, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
		snprintf(cmd, sizeof cmd, "Could not start %s (error %lu).", py, GetLastError());
		MessageBoxA(NULL, cmd, "Firestorm", MB_ICONERROR);
		return 1;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return 0;
}
