/*
 * MISRC Common - File Utilities
 *
 * Shared file opening and handling utilities for CLI and GUI.
 */

#ifndef MISRC_FILE_UTILS_H
#define MISRC_FILE_UTILS_H

#include <stdio.h>
#include <stdbool.h>

/*-----------------------------------------------------------------------------
 * File Opening
 *-----------------------------------------------------------------------------*/

/* Open a file for binary writing with optional overwrite confirmation
 *
 * @param f             Pointer to FILE* to receive opened file handle
 * @param filename      Path to file, or "-" for stdout
 * @param overwrite     If true, overwrite without prompting
 * @param interactive   If true and file exists and !overwrite, prompt user
 * @return 0 on success, -1 if user declined overwrite, -2 if open failed
 *
 * If filename is "-", stdout is returned.
 * If interactive is false and file exists and !overwrite, returns -1.
 */
int file_open_write(FILE **f, const char *filename, bool overwrite, bool interactive);

/* Close a file if it's not stdout
 *
 * @param f             File handle to close
 *
 * Safe to call with NULL or stdout - does nothing in those cases.
 */
void file_close_if_not_stdout(FILE *f);

#endif /* MISRC_FILE_UTILS_H */
