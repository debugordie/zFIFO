#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>

#include "zfifo.h"

int zf_send(int fd, char* data, unsigned long len){
  zfifo_io io;

  io.data = data;
  io.len = len;

  return ioctl(fd, IOCTL_SEND, &io);
}

int zf_recv(int fd, char* data, unsigned long len){
  zfifo_io io;

  io.data = data;
  io.len = len;

  return ioctl(fd, IOCTL_RECV, &io);
}

int zf_reset(int fd){
  return ioctl(fd, IOCTL_RESET, 0);
}
