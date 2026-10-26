// V14 OpenCL 1.2: each work item unions every legal biome accept/reject trace.
// The union is a mathematically conservative XZ upper bound, never block proof.
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
inline int run_max_words(U lo,U hi,int length){
 int best=run_max(lo);int highLen=max(0,length-64);
 if(highLen<64)hi&=(highLen==0?0UL:((1UL<<highLen)-1UL));
 best=max(best,run_max(hi));
 int suffix=0;for(int i=min(length,64)-1;i>=0;i--){if((lo>>i)&1UL)suffix++;else break;}
 int prefix=0;for(int i=0;i<highLen;i++){if((hi>>i)&1UL)prefix++;else break;}
 return max(best,suffix+prefix);
}
inline int has_run_words(U lo,U hi,int cutoff,int length){
 if(has_run(lo,cutoff)||has_run(hi,cutoff))return 1;
 int suffix=0;for(int i=min(length,64)-1;i>=0&&suffix<cutoff;i--){if((lo>>i)&1UL)suffix++;else break;}
 int prefix=0;for(int i=0;i<max(0,length-64)&&prefix<cutoff;i++){if((hi>>i)&1UL)prefix++;else break;}
 return suffix+prefix>=cutoff;
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
 U rows[128][2],cols[128][2];
 for(int i=0;i<128;i++)for(int w=0;w<2;w++){rows[i][w]=0UL;cols[i][w]=0UL;}
 RNG world=init_rng(seed);
 U xs=java_long(&world)|1UL;
 U zs=java_long(&world)|1UL;
 for(int rx=-2;rx<=2;rx++)for(int rz=-2;rz<=2;rz++){
  U xterm=(U)((long)(cx+rx)*16L)*xs;
  U zterm=(U)((long)(cz+rz)*16L)*zs;
  U fs=((xterm+zterm)^seed)+(U)index+90000UL;
  RNG r=init_rng(fs);
  int bx[20],bz[20];
  for(int n=0;n<20;n++){bx[n]=rx*16+pow2(&r,4);bz[n]=rz*16+pow2(&r,4);}
  RNG states[21],nextStates[21];int stateCount=1;states[0]=r;
  for(int n=0;n<20;n++){
   int nextCount=0;
   for(int s=0;s<stateCount;s++)nextStates[nextCount++]=states[s]; // rejection
   for(int s=0;s<stateCount;s++){
    RNG accepted=states[s];
    for(int t=0;t<4;t++){
     int x=bx[n]+pow2(&accepted,3)-pow2(&accepted,3)+39;
     int dy=pow2(&accepted,2)-pow2(&accepted,2);
     int z=bz[n]+pow2(&accepted,3)-pow2(&accepted,3)+39;
     if(x>=0&&x<94&&z>=0&&z<94){rows[z][x>>6]|=(1UL<<(x&63));cols[x][z>>6]|=(1UL<<(z&63));}
    }
    int duplicate=0;for(int q=0;q<nextCount;q++)if(nextStates[q].a==accepted.a&&nextStates[q].b==accepted.b){duplicate=1;break;}
    if(!duplicate)nextStates[nextCount++]=accepted;
   }
   stateCount=nextCount;for(int s=0;s<stateCount;s++)states[s]=nextStates[s];
  }
 }
 int found=0;
 for(int i=0;i<94;i++){
  if(has_run_words(rows[i][0],rows[i][1],cutoff,94)||has_run_words(cols[i][0],cols[i][1],cutoff,94)){found=1;break;}
 }
 if(!found){scores[id]=(uchar)0;return;}
 // Score the branch union. This can overstate a coherent trace by design.
 int best=0;
 for(int i=0;i<94;i++){
  int a=run_max_words(rows[i][0],rows[i][1],94);
  int b=run_max_words(cols[i][0],cols[i][1],94);
  best=max(best,max(a,b));
 }
 scores[id]=(uchar)best;
}
