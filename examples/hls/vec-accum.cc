#include "hls_stream.h"

struct int_s{
  int data;
  bool last;
};

void vec_accum (hls::stream<int_s>& a,
                hls::stream<int_s>& b){
#pragma HLS INTERFACE axis port=a
#pragma HLS INTERFACE axis port=b

  int_s aa, bb;
  int sum=0, i=0;

  do {
    aa = a.read();
    int x = aa.data;
    sum += x;
    i ++;
  } while(aa.last == 0);

  bb.data=i;   bb.last=0;  b.write(bb);
  bb.data=sum; bb.last=1;  b.write(bb);
}
