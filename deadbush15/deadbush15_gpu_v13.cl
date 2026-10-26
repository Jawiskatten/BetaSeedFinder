// V13 OpenCL 1.2: vanilla feature modifier order; GPU scouts all-accepted trace.
// Scores below cutoff map to 0. Scores at/above cutoff remain exact.
// One work item per (world seed, sampled chunk center). Radius-1 3x3 scout.
// This is strictly an RNG scout; NO terrain/biomes are simulated.
typedef ulong U;
typedef struct { U a,b; } RNG;
inline U mix64(U z) {
 z=(z^(z>>30))*0xbf58476d1ce4e5b9UL;
 z=(z^(z>>27))*0x94d049bb133111ebUL;
 return z^(z>>31);
}
inline RNG init_rng(U seed) {
 RNG r;
 U s=seed^7640891576956012809UL;
 r.a=mix64(s); r.b=mix64(s+0x9e3779b97f4a7c15UL);
 if(!(r.a|r.b)){r.a=0x9e3779b97f4a7c15UL;r.b=7640891576956012809UL;}
 return r;
}
inline U rol(U x,uint n){return (x<<n)|(x>>(64U-n));}
inline U next_rng(__private RNG *r){
 U a=r->a,b=r->b,out=rol(a+b,17U)+a;
 b^=a; r->a=rol(a,49U)^b^(b<<21); r->b=rol(b,28U);
 return out;
}
inline int pow2(__private RNG *r,int bits){return (int)(next_rng(r)>>(64-bits));}
inline U java_long(__private RNG *r){
 U h=(U)(uint)(next_rng(r)>>32);
 long l=(long)(int)(uint)(next_rng(r)>>32);
 return (h<<32)+(U)l;
}
inline int has_run(U mask,int cutoff){
 if(!mask)return 0;
 U contiguous=mask;int covered=1;
 while(covered*2<=cutoff){contiguous &= contiguous>>covered;covered*=2;if(!contiguous)return 0;}
 for(int i=covered;i<cutoff;i++){contiguous &= mask>>i;if(!contiguous)return 0;}
 return contiguous!=0UL;
}
inline int run_max(U mask){
 int count=0;
 while(mask){count++;mask&=(mask>>1);}
 return count;
}
__kernel void scout(__global uchar *scores,U start,U offset,uint locations,
                    int center_x,int center_z,int spread,int flat,uint index,int cutoff){
 size_t id=get_global_id(0);
 uint loc=(uint)(id%(size_t)locations);
 U seed=start+offset+(U)(id/(size_t)locations);
 int cx=center_x,cz=center_z;
 if(spread>0){
  U sx=seed^0xa0761d6478bd642fUL,sz=seed^0xe7037ed1a0b428dbUL;
  if(loc){sx^=(U)loc*0x9e3779b97f4a7c15UL;sz^=(U)loc*0xbf58476d1ce4e5b9UL;}
  U span=(U)spread*2UL+1UL;
  cx+=(int)(mix64(sx)%span)-spread;
  cz+=(int)(mix64(sz)%span)-spread;
 }
 U rows[64],cols[64];
 for(int i=0;i<64;i++){rows[i]=0UL;cols[i]=0UL;}
 RNG world=init_rng(seed);
 U xs=java_long(&world)|1UL;
 U zs=java_long(&world)|1UL;
 for(int rx=-1;rx<=1;rx++)for(int rz=-1;rz<=1;rz++){
  U xterm=(U)((long)(cx+rx)*16L)*xs;
  U zterm=(U)((long)(cz+rz)*16L)*zs;
  U fs=((xterm+zterm)^seed)+(U)index+90000UL;
  RNG r=init_rng(fs);
  int bx[20],bz[20];
  for(int n=0;n<20;n++){bx[n]=rx*16+pow2(&r,4);bz[n]=rz*16+pow2(&r,4);}
  for(int n=0;n<20;n++){
   for(int t=0;t<4;t++){
    int x=bx[n]+pow2(&r,3)-pow2(&r,3)+24;
    int dy=pow2(&r,2)-pow2(&r,2);
    int z=bz[n]+pow2(&r,3)-pow2(&r,3)+24;
    if((!flat||dy==0) && x>=0&&x<64&&z>=0&&z<64){rows[z]|=(1UL<<x);cols[x]|=(1UL<<z);}
   }
  }
 }
 int found=0;
 for(int i=0;i<64;i++){
  if(has_run(rows[i],cutoff)||has_run(cols[i],cutoff)){found=1;break;}
 }
 if(!found){scores[id]=(uchar)0;return;}
 // Rare positives get exact scoring. Every score >= cutoff is preserved.
 int best=0;
 for(int i=0;i<64;i++){
  int a=run_max(rows[i]);int b=run_max(cols[i]);
  best=max(best,max(a,b));
 }
 scores[id]=(uchar)best;
}
