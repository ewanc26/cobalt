/*
 * Entry point for the link check. Does nothing on purpose.
 *
 * The compile sweep is `-fsyntax-only`, which means it never resolves a
 * symbol — a function deleted during a refactor while callers remained
 * compiles perfectly and fails only at link time. That has already happened
 * once here, and the unit-test binary caught it; but the unit tests link only
 * the *without-Wolfram* configuration, so the halves of feed.c, notifications.c
 * and session.c that sit behind COBALT_HAS_WOLFRAM had no link coverage at all.
 *
 * This binary exists to give them some. Every object is passed to the linker
 * directly rather than through an archive, so all of them are included and
 * every undefined symbol has to resolve. Nothing is executed — running it would
 * mean talking to a PDS, which is not what a test does.
 */

int
main(void)
{
   return 0;
}
