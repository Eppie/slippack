// Presence maps share contexts across related fields and nearby frames.
// A reciprocal replaces division in the rANS encoder's hot loop.
struct AnsDivision {
  uint32_t reciprocal=0;unsigned shift=0;
  void set(unsigned f){if(!f)return;shift=31+(32-std::countl_zero(f-1));reciprocal=uint32_t(((uint64_t(1)<<shift)+f-1)/f);}
  uint32_t quotient(uint32_t x)const{return uint32_t((uint64_t(x)*reciprocal)>>shift);}
};
inline unsigned presence_bit(View masks,size_t start,size_t i,uint32_t count){return i<count?(masks[start+i/8]>>(i%8))&1:0;}
inline Bytes mask_encode(View masks,uint32_t count,View profiles,View refs){
 size_t planes=profiles.size(),width=(size_t(count)+7)/8;
 std::array<std::array<uint32_t,2>,256> hist{};std::vector<uint16_t> trace;trace.reserve(size_t(count)*planes);
 for(size_t j=0;j<planes;++j){size_t src=refs[j]?(j-refs[j])*width:0;unsigned history=0,above=refs[j]?(presence_bit(masks,src,0,count)*2+presence_bit(masks,src,1,count)*4):0,cls=profiles[j]==FloatLow?128:0;
  for(size_t i=0;i<count;++i){unsigned bit=presence_bit(masks,j*width,i,count),c=(history&15)|(above<<4)|cls;trace.push_back(uint16_t(c|(bit<<8)));++hist[c][bit];history=history*2+bit;above=(above>>1)|(refs[j]?presence_bit(masks,src,i+2,count)<<2:0);}
 }
 std::array<std::array<uint16_t,256>,2> candidates{};double costs[2]={};
 for(unsigned split=0;split<2;++split)for(unsigned c=0;c<(128u<<split);++c){uint32_t a=hist[c][0]+(split?0:hist[c+128][0]),b=hist[c][1]+(split?0:hist[c+128][1]);unsigned p=a+b?uint64_t(a)*4096/(a+b):4096;if(a&&b)p=std::clamp(p,1u,4095u);candidates[split][c]=p;costs[split]+=p<128?8:16;if(a)costs[split]+=a*std::log2(4096./p);if(b)costs[split]+=b*std::log2(4096./(4096-p));}
 unsigned split=costs[1]<costs[0],nc=128u<<split,cmask=nc-1;auto& probs=candidates[split];Bytes out{uint8_t(split)};
 std::array<std::array<AnsDivision,2>,256> div;
 for(unsigned c=0;c<nc;++c){varint(out,probs[c]);div[c][0].set(probs[c]);div[c][1].set(4096-probs[c]);}
 uint32_t state[4]={1u<<23,1u<<23,1u<<23,1u<<23};Bytes bytes;
 for(size_t t=trace.size();t-->0;){unsigned bit=trace[t]>>8,c=trace[t]&cmask,p=probs[c];uint32_t f=bit?4096-p:p,s=bit?p:0,&x=state[t&3];while(x>=((1u<<23>>12)<<8)*f){bytes.push_back(uint8_t(x));x>>=8;}x+=div[c][bit].quotient(x)*(4096-f)+s;}
 for(auto x:state)u32(out,x);out.insert(out.end(),bytes.rbegin(),bytes.rend());return out;
}
inline Bytes mask_decode(Reader& r,uint32_t count,View profiles,View refs){
 size_t planes=profiles.size(),width=(size_t(count)+7)/8;unsigned split=r.byte();require(split<=1,"mask model");std::array<uint16_t,256> probs{};for(unsigned c=0;c<(128u<<split);++c){unsigned x=r.var();require(x<=4096,"mask probability");probs[c]=x;}uint32_t state[4];for(auto& x:state){x=r.word();require(x>=(1u<<23)&&x<(1u<<31),"mask state");}Bytes masks(width*planes,0);
 for(size_t j=0;j<planes;++j){size_t src=refs[j]?(j-refs[j])*width:0;unsigned history=0,above=refs[j]?(presence_bit(masks,src,0,count)*2+presence_bit(masks,src,1,count)*4):0,cls=split&&profiles[j]==FloatLow?128:0;
  for(size_t i=0;i<count;++i){unsigned c=(history&15)|(above<<4)|cls,p=probs[c];uint32_t& x=state[(j*count+i)&3],slot=x&4095;unsigned bit=slot>=p,f=bit?4096-p:p,s=bit?p:0;x=f*(x>>12)+slot-s;while(x<(1u<<23))x=(x<<8)|r.byte();masks[j*width+i/8]|=bit<<(i%8);history=history*2+bit;above=(above>>1)|(refs[j]?presence_bit(masks,src,i+2,count)<<2:0);}
 }
 for(auto x:state)require(x==(1u<<23),"mask terminal state");return masks;
}
