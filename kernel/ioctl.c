// Linux ARM64 terminal ioctl ABI over Darwin's termios and ioctl numbers.
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>

struct linux_termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t line, cc[19];
};
_Static_assert( sizeof(struct linux_termios) == 36, "Linux ARM64 termios size" );

struct bit { uint32_t linux; tcflag_t darwin; };
#define BIT(l, d) { l, d }
static const struct bit input[] = {
    BIT(0x001, IGNBRK), BIT(0x002, BRKINT), BIT(0x004, IGNPAR), BIT(0x008, PARMRK),
    BIT(0x010, INPCK), BIT(0x020, ISTRIP), BIT(0x040, INLCR), BIT(0x080, IGNCR),
    BIT(0x100, ICRNL), BIT(0x400, IXON), BIT(0x800, IXANY), BIT(0x1000, IXOFF),
    BIT(0x2000, IMAXBEL), BIT(0x4000, IUTF8),
};
static const struct bit output[] = {
    BIT(0x001, OPOST), BIT(0x004, ONLCR), BIT(0x008, OCRNL),
    BIT(0x010, ONOCR), BIT(0x020, ONLRET),
};
static const struct bit control[] = {
    BIT(0x040, CSTOPB), BIT(0x080, CREAD), BIT(0x100, PARENB),
    BIT(0x200, PARODD), BIT(0x400, HUPCL), BIT(0x800, CLOCAL),
};
static const struct bit local[] = {
    BIT(0x001, ISIG), BIT(0x002, ICANON), BIT(0x008, ECHO), BIT(0x010, ECHOE),
    BIT(0x020, ECHOK), BIT(0x040, ECHONL), BIT(0x080, NOFLSH), BIT(0x100, TOSTOP),
    BIT(0x200, ECHOCTL), BIT(0x400, ECHOPRT), BIT(0x800, ECHOKE), BIT(0x8000, IEXTEN),
};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t from_host( tcflag_t bits, const struct bit *map, size_t n )
{
    uint32_t out = 0;
    for (size_t i = 0; i < n; i++) if (bits & map[i].darwin) out |= map[i].linux;
    return out;
}

static tcflag_t to_host( uint32_t bits, tcflag_t old, const struct bit *map, size_t n )
{
    for (size_t i = 0; i < n; i++)
        old = (old & ~map[i].darwin) | ((bits & map[i].linux) ? map[i].darwin : 0);
    return old;
}

static const struct { uint32_t linux; speed_t host; } speeds[] = {
    {0, B0}, {1, B50}, {2, B75}, {3, B110}, {4, B134}, {5, B150},
    {6, B200}, {7, B300}, {8, B600}, {9, B1200}, {10, B1800},
    {11, B2400}, {12, B4800}, {13, B9600}, {14, B19200}, {15, B38400},
    {0x1001, B57600}, {0x1002, B115200}, {0x1003, B230400},
};
static const int cc_index[19] = {
    VINTR, VQUIT, VERASE, VKILL, VEOF, VTIME, VMIN, -1, VSTART, VSTOP,
    VSUSP, VEOL, VREPRINT, VDISCARD, VWERASE, VLNEXT, VEOL2, -1, -1,
};

static int tcgets_linux( int fd, struct linux_termios *out )
{
    struct termios host;
    if (tcgetattr( fd, &host )) return -1;
    memset( out, 0, sizeof *out );
    out->iflag = from_host( host.c_iflag, input, COUNT(input) );
    out->oflag = from_host( host.c_oflag, output, COUNT(output) );
    out->cflag = from_host( host.c_cflag, control, COUNT(control) );
    out->lflag = from_host( host.c_lflag, local, COUNT(local) );
    out->cflag |= (host.c_cflag & CSIZE) == CS5 ? 0 :
                  (host.c_cflag & CSIZE) == CS6 ? 0x10 :
                  (host.c_cflag & CSIZE) == CS7 ? 0x20 : 0x30;
    for (size_t i = 0; i < COUNT(speeds); i++)
        if (cfgetospeed( &host ) == speeds[i].host) { out->cflag |= speeds[i].linux; break; }
    for (size_t i = 0; i < COUNT(cc_index); i++)
        if (cc_index[i] >= 0) out->cc[i] = host.c_cc[cc_index[i]];
    return 0;
}

static int tcsets_linux( int fd, int action, const struct linux_termios *in )
{
    struct termios host;
    if (tcgetattr( fd, &host )) return -1;
    host.c_iflag = to_host( in->iflag, host.c_iflag, input, COUNT(input) );
    host.c_oflag = to_host( in->oflag, host.c_oflag, output, COUNT(output) );
    host.c_cflag = to_host( in->cflag, host.c_cflag, control, COUNT(control) );
    host.c_lflag = to_host( in->lflag, host.c_lflag, local, COUNT(local) );
    static const tcflag_t sizes[] = { CS5, CS6, CS7, CS8 };
    host.c_cflag = (host.c_cflag & ~CSIZE) | sizes[(in->cflag >> 4) & 3];
    uint32_t speed = in->cflag & 0x100f;
    for (size_t i = 0; i < COUNT(speeds); i++)
    {
        if (speed != speeds[i].linux) continue;
        cfsetispeed( &host, speeds[i].host );
        cfsetospeed( &host, speeds[i].host );
        break;
    }
    for (size_t i = 0; i < COUNT(cc_index); i++)
        if (cc_index[i] >= 0) host.c_cc[cc_index[i]] = in->cc[i];
    return tcsetattr( fd, action, &host );
}

static long ioctl_linux( long fd, long request, long arg )
{
    switch (request)
    {
    case 0x5401: return tcgets_linux( (int)fd, (struct linux_termios *)arg );
    case 0x5402: return tcsets_linux( (int)fd, TCSANOW, (const struct linux_termios *)arg );
    case 0x5403: return tcsets_linux( (int)fd, TCSADRAIN, (const struct linux_termios *)arg );
    case 0x5404: return tcsets_linux( (int)fd, TCSAFLUSH, (const struct linux_termios *)arg );
    case 0x540f: return ioctl( (int)fd, TIOCGPGRP, (void *)arg );
    case 0x5410: return ioctl( (int)fd, TIOCSPGRP, (void *)arg );
    case 0x5413: return ioctl( (int)fd, TIOCGWINSZ, (void *)arg );
    case 0x5414: return ioctl( (int)fd, TIOCSWINSZ, (void *)arg );
    case 0x541b: return ioctl( (int)fd, FIONREAD, (void *)arg );
    case 0x5421: return ioctl( (int)fd, FIONBIO, (void *)arg );
    default: errno = ENOTTY; return -1;
    }
}

long inv_ioctl( long fd, long request, long arg )
{
    long r = ioctl_linux( fd, request, arg );
    // Darwin answers terminal requests on a non-terminal like /dev/null with ENODEV; Linux, and
    // programs testing "is this a tty", expect ENOTTY
    if (r == -1 && errno == ENODEV && (request & ~0xffL) == 0x5400) errno = ENOTTY;
    return r;
}
