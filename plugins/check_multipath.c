/*
 * License: GPLv3+
 * Copyright (c) 2013 Davide Madrisan <davide.madrisan@gmail.com>
 *
 * A Nagios plugin to check multipath topology
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "common.h"
#include "messages.h"
#include "progname.h"
#include "progversion.h"

static bool verbose = false;
static const char *multipathd_socket = MULTIPATHD_SOCKET;

static const char *program_copyright =
  "Copyright (C) 2013 Davide Madrisan <" PACKAGE_BUGREPORT ">\n";

static struct option const longopts[] = {
  {(char *) "verbose", no_argument, NULL, 'v'},
  {(char *) "help", no_argument, NULL, GETOPT_HELP_CHAR},
  {(char *) "version", no_argument, NULL, GETOPT_VERSION_CHAR},
  {NULL, 0, NULL, 0}
};

static _Noreturn void
usage (FILE * out)
{
  fprintf (out, "%s (" PACKAGE_NAME ") v%s\n", program_name, program_version);
  fputs ("This plugin checks the multipath topology status.\n", out);
  fputs (program_copyright, out);
  fputs (USAGE_HEADER, out);
  fprintf (out, "  %s [OPTION]...\n", program_name);
  fputs (USAGE_OPTIONS, out);
  fputs ("  -v, --verbose   show details for command-line debugging "
	 "(Nagios may truncate output)\n", out);
  fputs (USAGE_HELP, out);
  fputs (USAGE_VERSION, out);
  fputs (USAGE_EXAMPLES, out);
  fprintf (out, "  %s\n", program_name);

  exit (out == stderr ? STATE_UNKNOWN : STATE_OK);
}

static _Noreturn void
print_version (void)
{
  printf ("%s (" PACKAGE_NAME ") v%s\n", program_name, program_version);
  fputs (program_copyright, stdout);
  fputs (GPLv3_DISCLAIMER, stdout);

  exit (STATE_OK);
}

static size_t
write_all (int fd, const void *buf, size_t len)
{
  size_t total = 0;

  while (len)
    {
      ssize_t n = write (fd, buf, len);
      if (n < 0)
	{
	  if ((errno == EINTR) || (errno == EAGAIN))
	    continue;
	  return total;
	}
      if (!n)
	return total;

      buf = n + (char *) buf;
      len -= n;
      total += n;
    }

  return total;
}

static size_t
read_all (int fd, void *buf, size_t len)
{
  size_t total = 0;

  while (len)
    {
      ssize_t n = read (fd, buf, len);
      if (n < 0)
	{
	  if ((errno == EINTR) || (errno == EAGAIN))
	    continue;
	  return total;
	}
      if (!n)
	return total;

      buf = n + (char *) buf;
      len -= n;
      total += n;
    }

  return total;
}

static void
multipathd_query (const char *query, char *buf, size_t bufsize)
{
  int sock;
  struct sockaddr_un sun;
  size_t len = strlen (query) + 1;

  if ((sock = socket (PF_UNIX, SOCK_STREAM, 0)) < 0)
    plugin_error (STATE_UNKNOWN, errno, "cannot create unix stream socket");

  sun.sun_family = AF_UNIX;
  strncpy (sun.sun_path, multipathd_socket, sizeof (sun.sun_path));
  sun.sun_path[sizeof (sun.sun_path) - 1] = 0;

  if (connect (sock, (struct sockaddr *) &sun, sizeof (sun)) < 0)
    plugin_error (STATE_UNKNOWN, errno, "cannot connect to %s",
		  multipathd_socket);

  if (write_all (sock, &len, sizeof (len)) != sizeof (len))
    plugin_error (STATE_UNKNOWN, 0, "failed to send message to multipathd");

  if (write_all (sock, query, len) != len)
    plugin_error (STATE_UNKNOWN, 0, "failed to send message to multipathd");

  if (read_all (sock, &len, sizeof (len)) != sizeof (len))
    plugin_error (STATE_UNKNOWN, 0,
		  "failed to receive message from multipathd");

  if (len > bufsize)
    plugin_error (STATE_UNKNOWN, 0, "reply from multipathd too long");

  if (read_all (sock, buf, len) != len)
    plugin_error (STATE_UNKNOWN, 0,
		  "failed to receive message from multipathd");

  close (sock);
}

static int
check_for_faulty_paths (char *buf)
{
  char *str1, *token, *saveptr1;
  char *dm_st_ok_pattern = "[ \t]+[?active]?[ \t]*[?ready]?[ \t]+";
  int rc, row, faulty_paths = 0;
  regex_t regex;

  if ((rc = regcomp (&regex, dm_st_ok_pattern, REG_EXTENDED | REG_NOSUB)))
    {
      regerror (rc, &regex, buf, sizeof (buf));
      plugin_error (STATE_UNKNOWN, 0, "regcomp() failed: %s", buf);
    }

  /* data format:
   *       hcil    dev dev_t  pri dm_st   chk_st  next_check
   *   ex: 4:0:0:0 sdb 8:16   10  [active][ready] XXX....... 7/20
   * -or-
   *       hcil    dev dev_t pri dm_st  chk_st dev_st  next_check
   *   ex: 6:0:0:0 sdf 8:80  1   active ready  running XXXX...... 9/20  */

  for (row = 1, str1 = buf;; row++, str1 = NULL)
    {
      token = strtok_r (str1, "\n", &saveptr1);
      if (token == NULL)
	break;
      if (verbose)
	printf ("%s\n", token);
      if (row > 1 && regexec (&regex, token, (size_t) 0, NULL, 0))
	{
	  faulty_paths++;
	  if (verbose)
	    printf (" \\ faulty path detected!\n");
	}
    }

  return faulty_paths;
}

int
main (int argc, char **argv)
{
  int c, faulty_paths;
  static char buffer[2048];

  set_program_name (argv[0]);

  while ((c = getopt_long (argc, argv, "v" GETOPT_HELP_VERSION_STRING,
			   longopts, NULL)) != -1)
    {
      switch (c)
	{
	default:
	  usage (stderr);
	case 'v':
	  verbose = true;
	  break;

	case_GETOPT_HELP_CHAR
        case_GETOPT_VERSION_CHAR

	}
    }

  if (getuid () != 0)
    plugin_error (STATE_UNKNOWN, 0, "need to be root");

  multipathd_query ("show paths", buffer, sizeof (buffer));
  faulty_paths = check_for_faulty_paths (buffer);

  if (faulty_paths > 0)
    {
      printf ("%s %s: found %d faulty path(s)\n", program_name_short,
	      state_text (STATE_CRITICAL), faulty_paths);
      return STATE_CRITICAL;
    }

  printf ("%s %s\n", program_name_short, state_text (STATE_OK));
  return STATE_OK;
}
