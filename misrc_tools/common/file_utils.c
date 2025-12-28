/*
 * MISRC Common - File Utilities Implementation
 */

#include "file_utils.h"

#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#endif

int file_open_write(FILE **f, const char *filename, bool overwrite, bool interactive)
{
    if (strcmp(filename, "-") == 0) {
        *f = stdout;
        return 0;
    }

    if (access(filename, F_OK) == 0 && !overwrite) {
        if (interactive) {
            char ch = 0;
            fprintf(stderr, "File '%s' already exists. Overwrite? (y/n) ", filename);
            if (scanf(" %c", &ch) != 1) {
                return -1;
            }
            if (ch != 'y' && ch != 'Y') {
                return -1;
            }
        } else {
            /* Non-interactive mode, file exists, no overwrite permission */
            return -1;
        }
    }

    *f = fopen(filename, "wb");
    if (!(*f)) {
        fprintf(stderr, "Failed to open %s\n", filename);
        return -2;
    }

    return 0;
}

void file_close_if_not_stdout(FILE *f)
{
    if (f && f != stdout) {
        fclose(f);
    }
}
