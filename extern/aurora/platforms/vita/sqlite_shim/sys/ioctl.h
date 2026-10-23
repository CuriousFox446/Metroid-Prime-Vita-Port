#pragma once
/* PS Vita newlib has no <sys/ioctl.h>. SQLite's unix VFS includes it but only
   calls ioctl on Linux (F2FS atomic writes), so an empty header is enough. */
