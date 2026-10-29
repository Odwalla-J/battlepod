/* fsrun: start MW4.exe with the shim loaded before any of the game runs.
 *
 * MW4.exe loads ddraw.dll only when its graphics start, which is too late for
 * anything the game does first - reading c:\ctcl.ini, or a pod deciding to
 * quit. So this starts the game suspended, has it load ddraw.dll from its own
 * directory, and only then lets its main thread go. The shim does the rest.
 *
 * usage: fsrun [MW4 arguments...]                  the game
 *        fsrun -exe Launcher.exe [arguments...]   VWE's pod launcher, likewise
 * (fsrun.exe sits beside the program and the shim's ddraw.dll)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	char dir[MAX_PATH], exe[MAX_PATH], dll[MAX_PATH], cmd[4096], name[MAX_PATH] = "MW4.exe", *slash;
	const char *args = GetCommandLineA();
	STARTUPINFOA si = { sizeof si };
	PROCESS_INFORMATION pi;
	void *remote;
	HANDLE th;
	DWORD code;

	/* Our own arguments, without our own name. */
	if (*args == '"') { args = strchr(args + 1, '"'); args = args ? args + 1 : ""; }
	else { while (*args && *args != ' ') args++; }
	while (*args == ' ') args++;
	if (!strncmp(args, "-exe ", 5)) {
		int n = 0;
		for (args += 5; *args == ' '; args++) ;
		while (*args && *args != ' ' && n < MAX_PATH - 1) name[n++] = *args++;
		name[n] = 0;
		while (*args == ' ') args++;
	}

	GetModuleFileNameA(NULL, dir, sizeof dir);
	if ((slash = strrchr(dir, '\\'))) slash[1] = 0;
	snprintf(exe, sizeof exe, "%s%s", dir, name);
	snprintf(dll, sizeof dll, "%sddraw.dll", dir);
	snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, args);
	if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES || GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
		fprintf(stderr, "fsrun: put fsrun.exe beside %s and the shim's ddraw.dll\n", name);
		return 1;
	}

	if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, dir, &si, &pi)) {
		fprintf(stderr, "fsrun: cannot start %s (%lu)\n", exe, GetLastError());
		return 1;
	}
	remote = VirtualAllocEx(pi.hProcess, NULL, sizeof dll, MEM_COMMIT, PAGE_READWRITE);
	WriteProcessMemory(pi.hProcess, remote, dll, strlen(dll) + 1, NULL);
	/* kernel32 is at the same address in every process of a boot, so our
	 * LoadLibraryA is the game's. */
	th = CreateRemoteThread(pi.hProcess, NULL, 0,
				(LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"),
				remote, 0, NULL);
	if (!th) {
		fprintf(stderr, "fsrun: cannot load the shim (%lu)\n", GetLastError());
		TerminateProcess(pi.hProcess, 1);
		return 1;
	}
	WaitForSingleObject(th, INFINITE);
	GetExitCodeThread(th, &code);
	CloseHandle(th);
	VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
	if (!code) {
		fprintf(stderr, "fsrun: the shim did not load\n");
		TerminateProcess(pi.hProcess, 1);
		return 1;
	}
	ResumeThread(pi.hThread);
	printf("fsrun: %s running as process %lu\n", name, pi.dwProcessId);
	WaitForSingleObject(pi.hProcess, INFINITE);
	GetExitCodeProcess(pi.hProcess, &code);
	printf("fsrun: %s exited with %lu\n", name, code);
	return (int)code;
}
