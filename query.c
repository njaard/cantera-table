/*
    Inverted index query processor
    Copyright (C) 2013    Morten Hustveit
    Copyright (C) 2013    eVenture Capital Partners II

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <string.h>

#include "ca-table.h"

static size_t
CA_partition (struct ca_offset_score *data, size_t count, size_t pivot_index)
{
  size_t i, store_index;
  struct ca_offset_score pivot, tmp;

  store_index = 0;

  pivot = data[pivot_index];

  data[pivot_index] = data[count - 1];
  data[count - 1] = pivot;

  for (i = 0; i < count - 1; ++i)
    {
      if (data[i].score > pivot.score)
        {
          tmp = data[store_index];
          data[store_index] = data[i];
          data[i] = tmp;

          ++store_index;
        }
    }

  data[count - 1] = data[store_index];
  data[store_index] = pivot;

  return store_index;
}

void
ca_quicksort (struct ca_offset_score *data, size_t count)
{
  size_t pivot_index;

  while (count >= 2)
    {
      pivot_index = count / 2;

      pivot_index = CA_partition (data, count, pivot_index);

      ca_quicksort (data, pivot_index);

      data += pivot_index + 1;
      count -= pivot_index + 1;
    }
}

static size_t
CA_subtract_offsets (struct ca_offset_score *lhs, size_t lhs_count,
                     const struct ca_offset_score *rhs, size_t rhs_count)
{
  struct ca_offset_score *output, *o;
  const struct ca_offset_score *lhs_end, *rhs_end;

  output = o = lhs;

  lhs_end = lhs + lhs_count;
  rhs_end = rhs + rhs_count;

  while (lhs != lhs_end && rhs != rhs_end)
    {
      if (lhs->offset == rhs->offset)
        {
          ++lhs;
          ++rhs;

          continue;
        }

      if (lhs->offset < rhs->offset)
        *o++ = *lhs++;
      else
        ++rhs;
    }

  while (lhs != lhs_end)
    *o++ = *lhs++;

  return o - output;
}

static size_t
CA_intersect_offsets (struct ca_offset_score *lhs, size_t lhs_count,
                      const struct ca_offset_score *rhs, size_t rhs_count)
{
  struct ca_offset_score *output, *o;
  const struct ca_offset_score *lhs_end, *rhs_end;

  output = o = lhs;

  lhs_end = lhs + lhs_count;
  rhs_end = rhs + rhs_count;

  while (lhs != lhs_end && rhs != rhs_end)
    {
      if (lhs->offset == rhs->offset)
        {
          *o++ = *lhs;

          ++lhs;
          ++rhs;

          continue;
        }

      if (lhs->offset < rhs->offset)
        ++lhs;
      else
        ++rhs;
    }

  return o - output;
}

static size_t
CA_filter_offsets (struct ca_offset_score *offsets, size_t count,
                   int operator, float operand)
{
  size_t i, result = 0;

  for (i = 0; i < count; ++i)
    {
      switch (operator)
        {
        case '<':

          if (offsets[i].score < operand)
            offsets[result++] = offsets[i];

          break;

        case '>':

          if (offsets[i].score > operand)
            offsets[result++] = offsets[i];

          break;

        default:

          fprintf (stderr, "%c\n", operator);
          assert (!"unknown operator");
        }
    }

  return result;
}

int
ca_schema_query (struct ca_schema *schema, const char *query,
                 const char *index_table_name,
                 const char *summary_table_name,
                 ssize_t limit)
{
  struct ca_table *index_table;
  struct ca_table_declaration *index_declaration;

  struct ca_table *summary_table;
  struct ca_table_declaration *summary_declaration;

  struct iovec data_iov[2];
  const uint8_t *data;

  struct ca_offset_score *offsets = NULL;
  uint32_t i, offset_count = 0;

  char *query_buf = NULL, *token, *ch;

  ssize_t ret;
  int result = -1;

  ca_clear_error ();

  if (!(query_buf = ca_strdup (query)))
    goto done;

  if (!(index_table = ca_schema_table (schema, index_table_name, &index_declaration)))
    goto done;

  if (!(summary_table = ca_schema_table (schema, summary_table_name, &summary_declaration)))
    goto done;

  if (index_declaration->field_count != 2)
    {
      ca_set_error ("Incorrect field count in index table");

      goto done;
    }

  if (summary_declaration->field_count != 3)
    {
      ca_set_error ("Incorrect field count in summary table");

      goto done;
    }

  if (index_declaration->fields[0].type != CA_TEXT)
    {
      ca_set_error ("First field in index table must be text");

      goto done;
    }

  if (index_declaration->fields[1].type != CA_OFFSET_SCORE)
    {
      ca_set_error ("Second field in index table must be OFFSET_SCORE, is %s", ca_type_to_string (index_declaration->fields[1].type));

      goto done;
    }

  if (summary_declaration->fields[1].type != CA_TIMESTAMPTZ)
    {
      ca_set_error ("Second field in summary table must be TIMESTAMP WITH TIME ZONE");

      goto done;
    }

  if (summary_declaration->fields[2].type != CA_TEXT)
    {
      ca_set_error ("Second field in summary table must be TEXT");

      goto done;
    }

  for (token = strtok (query_buf, " \t"); token; token = strtok (NULL, " \t"))
    {
      struct ca_offset_score *token_offsets;
      uint32_t token_offset_count;
      int invert_rank = 0, subtract = 0;

      int operator = 0;
      float operand = 0;

      if (*token == '-')
        ++token, subtract = 1;

      if (*token == '~')
        ++token, invert_rank = 1;

      if (NULL != (ch = strchr (token, '>'))
          || NULL != (ch = strchr (token, '<')))
        {
          operator = *ch;
          operand = strtod (ch + 1, NULL);
          *ch = 0;
        }

      if (1 != (ret = ca_table_seek_to_key (index_table, token)))
        {
          if (!ret) /* "Not found" is not an error */
            result = 0;

          goto done;
        }

      if (2 != (ret = ca_table_read_row (index_table, data_iov, 2)))
        {
          if (ret >= 0)
            ca_set_error ("ca_table_read_row unexpectedly returned %d", (int) ret);

          goto done;
        }

      data = data_iov[1].iov_base;

      if (-1 == ca_parse_offset_score (&data, &token_offsets, &token_offset_count))
        goto done;

      if (operator)
        {
          token_offset_count =
            CA_filter_offsets (token_offsets, token_offset_count, operator, operand);
        }

      if (invert_rank)
        {
          for (i = 0; i < token_offset_count; ++i)
            token_offsets[i].score = -token_offsets[i].score;
        }

      if (!offsets)
        {
          offsets = token_offsets;
          offset_count = token_offset_count;
        }
      else
        {
          if (subtract)
            {
              offset_count = CA_subtract_offsets (offsets, offset_count,
                                                  token_offsets, token_offset_count);
            }
          else
            {
              offset_count = CA_intersect_offsets (offsets, offset_count,
                                                   token_offsets, token_offset_count);
            }

          free (token_offsets);
        }
    }

  if (offset_count < limit)
    limit = offset_count;

  ca_quicksort (offsets, offset_count);

  /* XXX: Fetch documents in phsyical order */

  putchar ('[');

  for (i = 0; i < limit; ++i)
    {
      if (i)
        printf (",\n");

      if (-1 == ca_table_seek (summary_table, offsets[i].offset, SEEK_SET))
        goto done;

      if (2 != (ret = ca_table_read_row (summary_table, data_iov, 2)))
        {
          if (ret >= 0)
            ca_set_error ("ca_table_read_row unexpectedly returned %d.  Is the index stale?", (int) ret);

          goto done;
        }

      printf ("%s", (const char *) data_iov[1].iov_base + 8);
    }

  printf ("]\n");

  result = 0;

done:

  free (offsets);
  free (query_buf);

  return result;
}