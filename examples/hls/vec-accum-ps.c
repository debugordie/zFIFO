#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "zfifo.h"

int main(){
  int fd = open("/dev/zfifo0", O_RDWR | O_SYNC);
  if (fd<0) {
    printf("Can't open /dev/zfifo0!\n");
    return -1;
  }

  unsigned size = 10;
  int send[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
  int recv[2] = { 0 };
  
  zf_send(fd, (char*)send, sizeof(int)*size);
  zf_recv(fd, (char*)recv, sizeof(int)*2);
  
  printf("Length: %d, Sum: %d\n", recv[0], recv[1]);
  
  close(fd);
  return 0;
}
