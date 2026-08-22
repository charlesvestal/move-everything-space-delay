/* Proof that the ping-pong addition changes nothing when it is off.
 *
 * Renders a fixed program through the core and prints a checksum of the
 * result. scripts/docker-build.sh builds this twice — once against the
 * PRISTINE upstream core fetched from dusk-audio/dusk-audio-plugins, once
 * against ours — and fails the build if the two numbers differ.
 *
 * That is the whole contract of the modification: Tape Echo behaves exactly
 * as Dusk Audio intended unless someone switches Ping Pong on.
 */
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include "TapeEchoDSP.hpp"
int main(){
  duskaudio::TapeEchoDSP d;
  d.prepare(44100.0,128); d.reset();
  d.setMode(11); d.setRepeatRate(0.5f); d.setIntensity(0.45f);
  d.setEchoLevel(0.8f); d.setReverbLevel(0.3f); d.setMix(0.42f);
  d.setInputGain(0.7f); d.setWowFlutter(0.0f); d.setTapeAge(0.0f);
  float L[128],R[128]; const float*in[2]={L,R}; float*out[2]={L,R};
  double acc=0;
  for(int b=0;b<400;b++){
    for(int i=0;i<128;i++){
      double s=(b<40)?0.5*sin(2*M_PI*220.0*(b*128+i)/44100.0):0.0;
      L[i]=R[i]=(float)s;
    }
    d.processBlock(in,out,2,128);
    for(int i=0;i<128;i++){acc+=L[i]*13.0+R[i]*7.0;}
  }
  printf("%.9f\n",acc);
  return 0;
}
