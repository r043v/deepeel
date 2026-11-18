/* libdeepeel: the whole deepeel command line as a function.
 *
 * deepeel_main(argc, argv) behaves exactly like the `deepeel` executable run
 * with the same arguments (`deepeel ROM`, `deepeel md explore ROM ...`, ...) and
 * returns its exit status.  argv[0] is the program name.  The engine keeps
 * process-wide state (signal handlers, worker threads, globals): call it once per
 * process, or run it in a forked child as m0boy does. */
#ifndef DEEPEEL_H
#define DEEPEEL_H
#ifdef __cplusplus
extern "C" {
#endif
int deepeel_main(int argc, char **argv);
#ifdef __cplusplus
}
#endif
#endif
