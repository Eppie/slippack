#include "../../custom_codec.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <chrono>
using namespace slpz_custom;
using Clock=std::chrono::steady_clock;
int main(){double sec[8]{};size_t sizes[8]{};unsigned wins[8]{};size_t originalSize=0;unsigned blocks=0;
 for(auto& entry:std::filesystem::directory_iterator("build/round3-mask-data")){
  std::ifstream f(entry.path(),std::ios::binary);Bytes file((std::istreambuf_iterator<char>(f)),{});Reader r{file};unsigned count=r.word(),planes=r.word();auto profiles=r.take(planes),refs=r.take(planes),original=r.take(r.remaining());size_t width=(count+7)/8;Bytes delta(original.begin(),original.end());
  for(unsigned j=0;j<planes;++j)if(refs[j])for(size_t i=0;i<width;++i)delta[j*width+i]^=original[(j-refs[j])*width+i];
  Bytes results[8];
  for(unsigned mode=0;mode<8;++mode){auto start=Clock::now();auto& out=results[mode];
   if(mode==0)out=encode_presence(original,count,profiles,refs);
   else if(mode==1)out=encode(delta,Generic,8);
   else if(mode==2)channel(out,delta);
   else if(mode==3)channel(out,original);
   else {auto data=(mode==4||mode==6)?View(delta):original;Bytes runs[2];
    for(unsigned j=0;j<planes;++j){unsigned previous=0,run=0;
     for(unsigned i=0;i<count;++i){unsigned bit=(data[j*width+i/8]>>(i%8))&1;
      if(bit==previous)++run;else{varint(runs[(mode>=6)?0:previous],run);previous=bit;run=1;}}
     varint(runs[(mode>=6)?0:previous],run);
    }
    channel(out,runs[0]);channel(out,runs[1]);
   }
   sec[mode]+=std::chrono::duration<double>(Clock::now()-start).count();sizes[mode]+=out.size();
  }
  size_t winner=0;for(unsigned m=1;m<8;++m)if(results[m].size()<results[winner].size())winner=m;++wins[winner];
  originalSize+=original.size();++blocks;
 }
 printf("blocks=%u raw=%zu\n",blocks,originalSize);for(unsigned m=0;m<8;++m)printf("mode=%u bytes=%zu sec=%.4f wins=%u\n",m,sizes[m],sec[m],wins[m]);
}
