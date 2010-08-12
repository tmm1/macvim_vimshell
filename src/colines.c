#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <termios.h>
#include <sys/types.h>
#include <pty.h>

void sigwinch(int arg)
{
	struct winsize ws;
	printf("window size is now: ");

	if(ioctl(0, TIOCGWINSZ, &ws)<0)
	{
		perror("ioctl");
		return;
	}
	printf("rows = %d, cols = %d\n", ws.ws_row, ws.ws_col);
}

int main()
{
	struct sigaction sa;
	sa.sa_handler=sigwinch;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags=0;
	if(sigaction(SIGWINCH, &sa, NULL)<0)
	{
		perror("sigaction");
		return -1;
	}
	sigwinch(0);
	while(1)
	{
		sleep(10);
	}
	return 0;
}
