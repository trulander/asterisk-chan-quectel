/*
    tty.c
*/

#include <fcntl.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h> /* struct termios tcgetattr() tcsetattr()  */

#include "ast_config.h"

#include <asterisk/app.h>
#include <asterisk/logger.h>

#include "tty.h"

#include "at_read.h"
#include "ringbuffer.h"

void tty_close_lck(const char* dev, int fd, int exclusive, int flck)
{
    if (flck) {
        if (flock(fd, LOCK_UN | LOCK_NB) < 0) {
            const int errno_save = errno;
            ast_log(LOG_WARNING, "[TTY] Unable to unlock %s: %s\n", dev, strerror(errno_save));
        }
    }

    if (exclusive) {
        if (ioctl(fd, TIOCNXCL) < 0) {
            const int errno_save = errno;
            if (errno_save != ENOTTY && errno_save != EINVAL) {
                ast_log(LOG_WARNING, "[TTY] Unable to disable exlusive mode for %s: %s\n", dev, strerror(errno_save));
            }
        }
    }

    close(fd);
}

void tty_close(const char* dev, int fd)
{
    if (fd < 0) {
        return;
    }
    tty_close_lck(dev, fd, 1, 1);
}

int tty_open(const char* dev, int typ)
{
    struct termios term_attr;

    const int fd = open(dev, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        const int errno_save = errno;
        tty_close_lck(dev, fd, 0, 0);
        ast_log(LOG_WARNING, "[TTY] Unable to open %s: %s\n", dev, strerror(errno_save));
        return -1;
    }

    int locking_status = 0;
    int exclusive = 0;
    if (ioctl(fd, TIOCGEXCL, &locking_status) < 0) {
        const int errno_save = errno;
        if (errno_save != ENOTTY && errno_save != EINVAL) {
            tty_close_lck(dev, fd, 0, 0);
            ast_log(LOG_WARNING, "[TTY] Unable to get locking status for %s: %s\n", dev, strerror(errno_save));
            return -1;
        }
        ast_log(LOG_NOTICE, "[TTY] %s does not support TIOCGEXCL; using flock fallback\n", dev);
    } else if (locking_status) {
        tty_close_lck(dev, fd, 0, 0);
        ast_verb(1, "Device %s locked.\n", dev);
        return -1;
    }

    if (ioctl(fd, TIOCEXCL) < 0) {
        const int errno_save = errno;
        if (errno_save != ENOTTY && errno_save != EINVAL) {
            tty_close_lck(dev, fd, 0, 0);
            ast_log(LOG_WARNING, "[TTY] Unable to put %s into exclusive mode: %s\n", dev, strerror(errno_save));
            return -1;
        }
        ast_log(LOG_NOTICE, "[TTY] %s does not support TIOCEXCL; flock remains the exclusive lock\n", dev);
    } else {
        exclusive = 1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        const int errno_save = errno;
        tty_close_lck(dev, fd, exclusive, 0);
        ast_log(LOG_WARNING, "[TTY] Unable to flock %s: %s\n", dev, strerror(errno_save));
        return -1;
    }

    const int flags = fcntl(fd, F_GETFD);
    if (flags == -1 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        const int errno_save = errno;
        tty_close_lck(dev, fd, exclusive, 1);
        ast_log(LOG_WARNING, "[TTY] fcntl(F_GETFD/F_SETFD) failed for %s: %s\n", dev, strerror(errno_save));
        return -1;
    }

    if (tcgetattr(fd, &term_attr)) {
        const int errno_save = errno;
        tty_close_lck(dev, fd, exclusive, 1);
        ast_log(LOG_WARNING, "[TTY] tcgetattr() failed for %s: %s\n", dev, strerror(errno_save));
        return -1;
    }

    switch (typ) {
        case 2:
            term_attr.c_cflag = B115200 | CS8 | CREAD | CLOCAL;
            break;

        case 1:
            term_attr.c_cflag = B115200 | CS8 | CREAD | CRTSCTS | CLOCAL;
            break;

        default:
            term_attr.c_cflag = B115200 | CS8 | CREAD | CRTSCTS;
            break;
    }

    term_attr.c_iflag     = 0;
    term_attr.c_oflag     = 0;
    term_attr.c_lflag     = 0;
    term_attr.c_cc[VMIN]  = 1;
    term_attr.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSAFLUSH, &term_attr)) {
        ast_log(LOG_WARNING, "[TTY] tcsetattr(TCSAFLUSH) failed for %s: %s\n", dev, strerror(errno));
    }

    return fd;
}

/*!
 * Get status of the quectel. It might happen that the device disappears
 * (e.g. due to a USB unplug).
 *
 * \return 0 if device seems ok, non-0 if it seems not available
 */

int tty_status(int fd, int* err)
{
    struct termios t;

    if (fd < 0) {
        if (err) {
            *err = EINVAL;
        }
        return -1;
    }

    const int res = tcgetattr(fd, &t);
    if (res) {
        if (err) {
            *err = errno;
        }
    }
    return res;
}
