#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <err.h>
#include <getopt.h>
#include <sys/mman.h>
#include <sysexits.h>

#include "ca-table.h"

static int print_version;
static int print_help;

static struct option long_options[] =
{
    { "version",        no_argument,       &print_version, 1 },
    { "help",           no_argument,       &print_help,    1 },
    { 0, 0, 0, 0 }
};

int
main (int argc, char **argv)
{
  struct ca_table *table;
  int i;

  while ((i = getopt_long (argc, argv, "", long_options, 0)) != -1)
    {
      switch (i)
        {
        case 0:

          break;

        case '?':

          errx (EX_USAGE, "Try '%s --help' for more information.", argv[0]);
        }
    }

  if (print_help)
    {
      printf ("Usage: %s [OPTION]... OUTPUT INPUT...\n"
             "\n"
             "      --help     display this help and exit\n"
             "      --version  display version information\n"
             "\n"
             "Report bugs to <morten.hustveit@gmail.com>\n",
             argv[0]);

      return EXIT_SUCCESS;
    }

  if (print_version)
    {
      fprintf (stdout, "%s\n", PACKAGE_STRING);

      return EXIT_SUCCESS;
    }

  if (optind + 1 > argc)
    errx (EX_USAGE, "Usage: %s [OPTION]... TABLE [KEY]", argv[0]);

  if (!(table = ca_table_open ("write-once", argv[optind++], O_RDONLY, 0)))
    errx (EX_NOINPUT, "%s", ca_last_error ());

  if (optind == argc)
    {
      struct iovec value;
      ssize_t ret;

      while (1 == (ret = ca_table_read_row (table, &value, 1)))
        printf ("%s\n", (const char *) value.iov_base);

      if (ret == -1)
        {
          fprintf (stderr, "Error: %s\n", ca_last_error ());

          return EXIT_FAILURE;
        }
    }
  else
    {
      const char *key = argv[optind++];
      struct iovec value[2];
      const uint8_t *begin, *end;
      ssize_t ret;

      if (-1 == (ret = ca_table_seek_to_key (table, key)))
        errx (EXIT_FAILURE, "Error: %s", ca_last_error());

      if (!ret)
        {
          fprintf (stderr, "'%s' not found\n", key);

          return EXIT_FAILURE;
        }

      if (2 != (ret = ca_table_read_row (table, value, 2)))
        {
          if (ret < 0)
            fprintf (stderr, "Error: %s\n", ca_last_error ());
          else if (!ret) /* Key both exists and does not exist? */
            fprintf (stderr, "Error: ca_table_read_row unexpectedly returned %d\n",
                     (int) ret);

          return EXIT_FAILURE;
        }

      begin = value[1].iov_base;
      end = begin + value[1].iov_len;

      while (begin != end)
        {
          uint64_t start_time;
          uint32_t i, interval, count;
          const float *sample_values;

          ca_parse_time_float4 (&begin,
                                &start_time, &interval,
                                &sample_values, &count);


          for (i = 0; i < count; ++i)
            {
              printf ("%llu\t%.7g\n",
                      (unsigned long long) (start_time + i * interval),
                      sample_values[i]);
            }
        }
    }

  ca_table_close (table);

  return EXIT_SUCCESS;
}
