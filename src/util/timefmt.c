#include "util/timefmt.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Read exactly `digits` decimal digits. Returns false on anything else, so a
 * malformed field fails the parse instead of silently reading a short value. */
static bool
take_digits(const char **cursor, int digits, int *out)
{
   int value = 0;
   for (int i = 0; i < digits; i++) {
      char c = (*cursor)[i];
      if (c < '0' || c > '9') {
         return false;
      }
      value = value * 10 + (c - '0');
   }
   *cursor += digits;
   *out = value;
   return true;
}

static bool
take_literal(const char **cursor, char expected)
{
   if (**cursor != expected) {
      return false;
   }
   (*cursor)++;
   return true;
}

/*
 * Days from 1970-01-01 to y-m-d, proleptic Gregorian.
 *
 * Howard Hinnant's days_from_civil. It shifts the year to start in March so
 * the leap day lands at the end of the cycle, which removes every special case
 * for February — no lookup tables and no branching on leap years.
 */
static int64_t
days_from_civil(int64_t y, unsigned m, unsigned d)
{
   y -= (m <= 2);
   const int64_t era = (y >= 0 ? y : y - 399) / 400;
   const unsigned yoe = (unsigned) (y - era * 400);                     /* [0, 399] */
   const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
   const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;       /* [0, 146096] */
   return era * 146097 + (int64_t) doe - 719468;
}

/* The inverse of days_from_civil, from the same source. */
static void
civil_from_days(int64_t z, int *out_year, unsigned *out_month, unsigned *out_day)
{
   z += 719468;
   const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
   const unsigned doe = (unsigned) (z - era * 146097);                  /* [0, 146096] */
   const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
   const int64_t y = (int64_t) yoe + era * 400;
   const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);        /* [0, 365] */
   const unsigned mp = (5 * doy + 2) / 153;                             /* [0, 11] */
   const unsigned d = doy - (153 * mp + 2) / 5 + 1;                     /* [1, 31] */
   const unsigned m = mp + (mp < 10 ? 3 : -9);                          /* [1, 12] */

   *out_year = (int) (y + (m <= 2));
   *out_month = m;
   *out_day = d;
}

bool
cobalt_time_format_rfc3339(int64_t epoch, char *out, size_t out_size)
{
   /* "YYYY-MM-DDTHH:MM:SSZ" is 20 characters plus a terminator. */
   if (!out || out_size < 21) {
      return false;
   }

   /* Floor division, so times before 1970 do not round the day towards zero
    * and land an hour into the wrong date. */
   int64_t days = epoch / 86400;
   int64_t rem = epoch % 86400;
   if (rem < 0) {
      rem += 86400;
      days -= 1;
   }

   int year;
   unsigned month, day;
   civil_from_days(days, &year, &month, &day);

   snprintf(out, out_size, "%04d-%02u-%02uT%02u:%02u:%02uZ", year, month, day,
            (unsigned) (rem / 3600), (unsigned) ((rem % 3600) / 60),
            (unsigned) (rem % 60));
   return true;
}

bool
cobalt_time_parse_rfc3339(const char *text, int64_t *out_epoch)
{
   if (!text || !out_epoch) {
      return false;
   }

   const char *c = text;
   int year, month, day, hour, minute, second;

   if (!take_digits(&c, 4, &year) || !take_literal(&c, '-') ||
       !take_digits(&c, 2, &month) || !take_literal(&c, '-') ||
       !take_digits(&c, 2, &day)) {
      return false;
   }

   /* ATProto uses 'T'; tolerate the lowercase form RFC 3339 also permits. */
   if (*c != 'T' && *c != 't') {
      return false;
   }
   c++;

   if (!take_digits(&c, 2, &hour) || !take_literal(&c, ':') ||
       !take_digits(&c, 2, &minute) || !take_literal(&c, ':') ||
       !take_digits(&c, 2, &second)) {
      return false;
   }

   /* Fractional seconds are present on most PDS output and carry no
    * information a feed needs, so they are skipped rather than parsed. */
   if (*c == '.') {
      c++;
      if (*c < '0' || *c > '9') {
         return false;
      }
      while (*c >= '0' && *c <= '9') {
         c++;
      }
   }

   /* UTC only — see the header. A numeric offset is rejected rather than
    * being read as if it were Zulu. */
   if (*c != 'Z' && *c != 'z') {
      return false;
   }
   c++;
   if (*c != '\0') {
      return false;
   }

   if (month < 1 || month > 12 || day < 1 || day > 31 ||
       hour > 23 || minute > 59 || second > 60) {  /* 60: leap second */
      return false;
   }

   const int64_t days = days_from_civil(year, (unsigned) month, (unsigned) day);
   *out_epoch = days * 86400 + hour * 3600 + minute * 60 + second;
   return true;
}

void
cobalt_time_relative(int64_t epoch, int64_t now, char *out, size_t out_size)
{
   if (!out || out_size == 0) {
      return;
   }

   int64_t age = now - epoch;

   /* A future timestamp means the console's clock is behind, not that the post
    * is from the future. Showing "now" is the least confusing outcome. */
   if (age < 0) {
      age = 0;
   }

   if (age < 60) {
      snprintf(out, out_size, "%ds", (int) age);
   } else if (age < 3600) {
      snprintf(out, out_size, "%dm", (int) (age / 60));
   } else if (age < 86400) {
      snprintf(out, out_size, "%dh", (int) (age / 3600));
   } else if (age < 7 * 86400) {
      snprintf(out, out_size, "%dd", (int) (age / 86400));
   } else if (age < 365 * 86400) {
      snprintf(out, out_size, "%dw", (int) (age / (7 * 86400)));
   } else {
      snprintf(out, out_size, "%dy", (int) (age / (365 * 86400)));
   }
}

int64_t
cobalt_time_now(void)
{
   const time_t now = time(NULL);
   /* (time_t) -1 means the clock is unavailable. Callers treat 0 as "no idea",
    * which makes every post read as "now" rather than as 1970. */
   return (now == (time_t) -1) ? 0 : (int64_t) now;
}
