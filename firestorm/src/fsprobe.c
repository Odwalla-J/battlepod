/* fsprobe: where is a busy MW4.exe thread spending its time?
 *
 * Pauses the thread for a moment, reads its instruction pointer and the stack
 * words that point into MW4.exe's code, lets it go, and repeats - a poor
 * man's profiler for a game that has stopped responding, with nothing to
 * attach and nothing to restart.
 *
 * usage: fsprobe <thread id> [samples]
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define CODE_LO 0x00401000u
#define CODE_HI 0x006E2000u

int main(int argc, char **argv)
{
	DWORD tid, i, n = 40;
	HANDLE th, ph;
	DWORD pid = 0;

	if (argc < 2) { fprintf(stderr, "usage: fsprobe <thread id> [samples]\n"); return 1; }
	tid = strtoul(argv[1], NULL, 0);
	if (argc > 2) n = strtoul(argv[2], NULL, 0);
	th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
	if (!th) { fprintf(stderr, "fsprobe: cannot open thread %lu (%lu)\n", tid, GetLastError()); return 1; }
	pid = GetProcessIdOfThread(th);
	ph = OpenProcess(PROCESS_VM_READ, FALSE, pid);
	if (!ph) { fprintf(stderr, "fsprobe: cannot open process %lu (%lu)\n", pid, GetLastError()); return 1; }

	for (i = 0; i < n; i++) {
		CONTEXT c;
		DWORD stack[512];
		SIZE_T got = 0;
		int k, shown = 0;

		SuspendThread(th);
		c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
		GetThreadContext(th, &c);
		ReadProcessMemory(ph, (void *)c.Esp, stack, sizeof stack, &got);
		ResumeThread(th);

		printf("eip %08lX  stack:", c.Eip);
		for (k = 0; k < (int)(got / 4) && shown < 10; k++)
			if (stack[k] >= CODE_LO && stack[k] < CODE_HI) { printf(" %08lX", stack[k]); shown++; }
		printf("\n");
		if (i == 0) {
			/* File paths on the stack: what a stuck loop is trying to open. */
			const char *b = (const char *)stack;
			int j, start = -1;
			for (j = 0; j < (int)got; j++) {
				int pr = b[j] >= 0x20 && b[j] < 0x7F;
				if (pr && start < 0) start = j;
				if (!pr) {
					if (start >= 0 && j - start >= 6 && (memchr(b + start, '\\', j - start) ||
									  memchr(b + start, ':', j - start)))
						printf("  string at esp+%03X: %.*s\n", start, j - start, b + start);
					start = -1;
				}
			}
		}
		Sleep(50);
	}
	return 0;
}
