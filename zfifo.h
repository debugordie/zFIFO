#ifndef _ZFIFO_H_
#define _ZFIFO_H_

#include <linux/ioctl.h>

typedef struct {
  unsigned long len;
  char * data;
} zfifo_io;

#define ZFIFO_MAGIC 'Z'

#define IOCTL_SEND _IOW(ZFIFO_MAGIC, 1, zfifo_io *)
#define IOCTL_RECV _IOR(ZFIFO_MAGIC, 2, zfifo_io *)
#define IOCTL_RESET _IOW(ZFIFO_MAGIC, 2, int)

#ifndef _ZFIFO_DRIVER_
int zf_send(int fd, char* data, unsigned long len);
int zf_recv(int fd, char* data, unsigned long len);
int zf_reset(int fd);
#endif

#endif
