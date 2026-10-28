#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifndef FIXTURE_VERSION
#define FIXTURE_VERSION 1
#endif

int
main (int argc, char **argv)
{
  struct termios settings;
  char input;
  if (argc > 2 && (strcmp (argv[1], "--wrap") == 0 || strcmp (argv[1], "--ambiguous") == 0 || (argc > 3 && strcmp (argv[1], "--mixed") == 0)))
    {
      int count = strcmp (argv[1], "--wrap") == 0 ? 1 : 2;
      for (int i = 0; i < count; i++)
        {
          pid_t pid = fork ();
          if (pid < 0)
            return 1;
          if (pid == 0)
            {
              const char *program = i == 1 && strcmp (argv[1], "--mixed") == 0 ? argv[3] : argv[2];
              execl (program, program, "--hold", NULL);
              _exit (127);
            }
        }
      while (wait (NULL) > 0)
        ;
      return 0;
    }
  if (tcgetattr (STDIN_FILENO, &settings) != 0)
    return 1;
  cfmakeraw (&settings);
  if (tcsetattr (STDIN_FILENO, TCSANOW, &settings) != 0)
    return 1;
  printf ("\033]0;codex - monitor fixture - Terminal\aREADY:%d\r\n", FIXTURE_VERSION);
  fflush (stdout);
  while (read (STDIN_FILENO, &input, 1) == 1)
    {
      if (input == 'e')
        {
          execl (argv[0], argv[0], "--hold", NULL);
          return 127;
        }
      if (input == 'q')
        break;
    }
  return 0;
}
