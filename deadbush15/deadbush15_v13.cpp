// Deadbush15 v13: vanilla-ordered GPU candidate search plus checkpointing.
// This is an RNG placement candidate generator; exact terrain verification remains separate.
#define main deadbush15_legacy_main
#include "deadbush15.cpp"
#undef main
#include <filesystem>
#include <map>
#include <stdexcept>

// A bitset covering the union of all chunks in the selected rectangular region.
// All chunk decoration streams are generated exactly once per candidate seed.
struct RegionGrid {
    int x0,z0,width,height,xWords,zWords;
    std::vector<u64> rows,cols;
    RegionGrid(int wChunks,int hChunks,int radius)
        : x0(-radius*16-7),z0(-radius*16-7),
          width((wChunks+2*radius)*16+14),height((hChunks+2*radius)*16+14),
          xWords((width+63)/64),zWords((height+63)/64),
          rows(size_t(height)*xWords,0),cols(size_t(width)*zWords,0) {}
    void put(int x,int z) {
        x-=x0;z-=z0;
        if(x<0||x>=width||z<0||z>=height)return;
        rows[size_t(z)*xWords+x/64]|=u64(1)<<(x%64);
        cols[size_t(x)*zWords+z/64]|=u64(1)<<(z%64);
    }
    bool get(int x,int z) const {
        return (rows[size_t(z)*xWords+x/64]>>(x%64))&1;
    }
    Hit best(bool diag) const {
        Hit h;
        // Score bit runs, carrying sequences across 64-bit word boundaries.
        for(int axis=0;axis<2;axis++) {
            int lines=axis==0?height:width;
            int words=axis==0?xWords:zWords;
            int length=axis==0?width:height;
            const auto &data=axis==0?rows:cols;
            for(int l=0;l<lines;l++) {
                int carry=0;
                for(int w=0;w<words;w++) {
                    u64 mask=data[size_t(l)*words+w];
                    int remaining=std::min(64,length-64*w);
                    if(remaining<64)mask&=(u64(1)<<remaining)-1;
                    int offset=0;
                    while(mask) {
                        int zeros=__builtin_ctzll(mask);
                        if(zeros)carry=0;
                        offset+=zeros;
                        mask>>=zeros;
                        int ones=(mask==~u64(0))?64:__builtin_ctzll(~mask);
                        int run=carry+ones;
                        if(run>h.run) {
                            int start=64*w+offset-carry;
                            h={run,axis==0?start+x0:l+x0,
                                axis==0?l+z0:start+z0,axis==0?1:0,axis==0?0:1};
                        }
                        // Carry applies only when a segment touches the next word.
                        carry=(offset+ones==64)?run:0;
                        if(ones>=64||offset+ones>=64)break;
                        mask>>=ones;
                        offset+=ones;
                    }
                    if(!data[size_t(l)*words+w])carry=0;
                    if(remaining<64)carry=0;
                }
            }
        }
        if(diag) for(int sign:{-1,1}) for(int z=0;z<height;z++) for(int x=0;x<width;x++) {
            if(!get(x,z))continue;
            // A run's predecessor should be empty (only inspect starts).
            if(z>0 && x-sign>=0 && x-sign<width && get(x-sign,z-1))continue;
            int len=1;
            while(z+len<height && x+sign*len>=0 && x+sign*len<width
                  && get(x+sign*len,z+len))++len;
            if(len>h.run)h={len,x+x0,z+z0,sign,1};
        }
        return h;
    }
};
static void generateRegionChunk(RegionGrid &g,u64 world,int absCx,int absCz,int relativeCx,int relativeCz,int index,bool flat) {
    Xoro r(feature_seed(world,absCx,absCz,index));
    int baseX[20],baseZ[20];
    for(int n=0;n<20;n++) {
        baseX[n]=relativeCx*16+r.pow2(4);baseZ[n]=relativeCz*16+r.pow2(4);
    }
    for(int n=0;n<20;n++) {
        for(int t=0;t<4;t++) {
            int x=baseX[n]+r.pow2(3)-r.pow2(3);
            int dy=r.pow2(2)-r.pow2(2);
            int z=baseZ[n]+r.pow2(3)-r.pow2(3);
            if(!flat||dy==0)g.put(x,z);
        }
    }
}
static Hit evaluateRegion(u64 seed,int index,int radius,bool flat,bool diag,
                          int baseX,int baseZ,int width,int height) {
    RegionGrid g(width,height,radius);
    for(int x=-radius;x<width+radius;x++)for(int z=-radius;z<height+radius;z++)
        generateRegionChunk(g,seed,baseX+x,baseZ+z,x,z,index,flat);
    return g.best(diag);
}
// FNV1a fingerprint protects resumes from accidentally changing search options.
static u64 hashString(const std::string &s) {
    u64 h=14695981039346656037ULL;
    for(unsigned char c:s){h^=c;h*=1099511628211ULL;}
    return h;
}
static std::string fingerprint(u64 start,u64 count,int index,int radius,int width,int height,
                               int cx,int cz,int spread,int report,int target,int threads,
                               bool flat,bool diag,const std::string &output) {
    std::ostringstream s;
    s<<"D15V2|"<<start<<'|'<<count<<'|'<<index<<'|'<<radius<<'|'<<width<<'|'<<height
     <<'|'<<cx<<'|'<<cz<<'|'<<spread<<'|'<<report<<'|'<<target<<'|'<<flat<<'|'<<diag<<'|'<<output;
    (void)threads; // Thread count can change across resume.
    return std::to_string(hashString(s.str()));
}
static void appendCheckpoint(const std::string &path,const std::string &signature,u64 offset) {
    if(path.empty())return;
    std::ofstream f(path,std::ios::app);
    if(!f)throw std::runtime_error("cannot write checkpoint: "+path);
    const std::string payload="D15V2 "+signature+" "+std::to_string(offset);
    f<<payload<<' '<<hashString(payload)<<'\n';
    f.flush();
    if(!f)throw std::runtime_error("checkpoint write failed: "+path);
}
static u64 readCheckpoint(const std::string &path,const std::string &signature,u64 count) {
    std::ifstream f(path);
    if(!f)return 0;
    std::string line;
    u64 last=0;
    bool any=false;
    while(std::getline(f,line)) {
        std::istringstream in(line);
        std::string version,sig,offset,check,extra;
        if(!(in>>version>>sig>>offset>>check)||(in>>extra))continue; // partial crash line
        const std::string payload=version+" "+sig+" "+offset;
        if(version!="D15V2"||std::to_string(hashString(payload))!=check)continue;
        if(sig!=signature)throw std::runtime_error("checkpoint belongs to different options/output; choose another checkpoint file");
        try {
            u64 n=parseU(offset);
            if(n<=count){last=n;any=true;}
        }catch(...){}
    }
    if(!any)throw std::runtime_error("no valid progress in existing checkpoint; choose a new path");
    return last;
}
static void usageV2() {
    std::cout<<"Deadbush15 v2 - Minecraft Java 26.3 decoration RNG scout (NOT terrain verified)\n"
      <<"  --self-test                  reference and region-equivalence checks\n"
      <<"  --search --start 0 --count 1000000 --threads 8 --radius 1\n"
      <<"  --chunk-x 0 --chunk-z 0      first region chunk (default world origin)\n"
      <<"  --region-width 4 --region-height 4    scan a rectangular union of chunks\n"
      <<"  --spread-span 1000000       choose a deterministic random center per seed\n"
      <<"  --checkpoint progress.log --resume   checkpoint each 5 sec; resume on restart\n"
      <<"  --output region_candidates.csv --report 10 --target 15 --mode flat\n"
      <<"Rows may span the whole rectangular region and its radius border.\n"
      <<"All candidates require vanilla terrain/biome verification.\n";
}
#include <memory>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
using cl_int=int; using cl_uint=uint32_t; using cl_ulong=uint64_t;
using cl_device_type=cl_ulong; using cl_bool=cl_uint;
using cl_context_properties=intptr_t;using cl_command_queue_properties=cl_ulong;
using cl_platform_id=void*;using cl_device_id=void*;using cl_context=void*;
using cl_command_queue=void*;using cl_program=void*;using cl_kernel=void*;
using cl_mem=void*;using cl_event=void*;
static constexpr cl_device_type CL_DEVICE_TYPE_GPU_=4;
static constexpr cl_uint CL_DEVICE_NAME_=0x102B,CL_DEVICE_VENDOR_=0x102C;
static constexpr cl_uint CL_PROGRAM_BUILD_LOG_=0x1183;
static constexpr cl_ulong CL_MEM_WRITE_ONLY_=2;
static constexpr cl_bool CL_TRUE_=1;
#define FN(ret,name,...) using name##_fn=ret (*)(__VA_ARGS__);name##_fn name=nullptr;
struct CL {
#ifdef _WIN32
 HMODULE lib=nullptr;
#else
 void *lib=nullptr;
#endif
 FN(cl_int,clGetPlatformIDs,cl_uint,cl_platform_id*,cl_uint*)
 FN(cl_int,clGetDeviceIDs,cl_platform_id,cl_device_type,cl_uint,cl_device_id*,cl_uint*)
 FN(cl_int,clGetDeviceInfo,cl_device_id,cl_uint,size_t,void*,size_t*)
 FN(cl_context,clCreateContext,const cl_context_properties*,cl_uint,const cl_device_id*,void*,void*,cl_int*)
 FN(cl_command_queue,clCreateCommandQueue,cl_context,cl_device_id,cl_command_queue_properties,cl_int*)
 FN(cl_program,clCreateProgramWithSource,cl_context,cl_uint,const char**,const size_t*,cl_int*)
 FN(cl_int,clBuildProgram,cl_program,cl_uint,const cl_device_id*,const char*,void*,void*)
 FN(cl_int,clGetProgramBuildInfo,cl_program,cl_device_id,cl_uint,size_t,void*,size_t*)
 FN(cl_kernel,clCreateKernel,cl_program,const char*,cl_int*)
 FN(cl_mem,clCreateBuffer,cl_context,cl_ulong,size_t,void*,cl_int*)
 FN(cl_int,clSetKernelArg,cl_kernel,cl_uint,size_t,const void*)
 FN(cl_int,clEnqueueNDRangeKernel,cl_command_queue,cl_kernel,cl_uint,const size_t*,const size_t*,const size_t*,cl_uint,const cl_event*,cl_event*)
 FN(cl_int,clEnqueueReadBuffer,cl_command_queue,cl_mem,cl_bool,size_t,size_t,void*,cl_uint,const cl_event*,cl_event*)
 FN(cl_int,clReleaseMemObject,cl_mem)
 FN(cl_int,clReleaseKernel,cl_kernel)
 FN(cl_int,clReleaseProgram,cl_program)
 FN(cl_int,clReleaseCommandQueue,cl_command_queue)
 FN(cl_int,clReleaseContext,cl_context)
 void *sym(const char *s) {
#ifdef _WIN32
  return reinterpret_cast<void*>(GetProcAddress(lib,s));
#else
  return dlsym(lib,s);
#endif
 }
 CL(){
#ifdef _WIN32
  lib=LoadLibraryA("OpenCL.dll");
#else
  lib=dlopen("libOpenCL.so.1",RTLD_NOW);
#endif
  if(!lib)throw std::runtime_error("OpenCL runtime unavailable: update your AMD Adrenalin driver");
#define LOAD(n) n=reinterpret_cast<decltype(n)>(sym(#n));if(!n)throw std::runtime_error("Missing OpenCL function " #n);
  LOAD(clGetPlatformIDs) LOAD(clGetDeviceIDs) LOAD(clGetDeviceInfo)
  LOAD(clCreateContext) LOAD(clCreateCommandQueue) LOAD(clCreateProgramWithSource)
  LOAD(clBuildProgram) LOAD(clGetProgramBuildInfo) LOAD(clCreateKernel)
  LOAD(clCreateBuffer) LOAD(clSetKernelArg) LOAD(clEnqueueNDRangeKernel)
  LOAD(clEnqueueReadBuffer) LOAD(clReleaseMemObject) LOAD(clReleaseKernel)
  LOAD(clReleaseProgram) LOAD(clReleaseCommandQueue) LOAD(clReleaseContext)
#undef LOAD
 }
 ~CL(){
#ifdef _WIN32
  if(lib)FreeLibrary(lib);
#else
  if(lib)dlclose(lib);
#endif
 }
};
static void ck(cl_int code,const char*where){if(code)throw std::runtime_error(std::string(where)+" OpenCL error "+std::to_string(code));}
struct GPU {
 CL cl;cl_device_id device=nullptr;cl_context ctx=nullptr;cl_command_queue q=nullptr;
 cl_program program=nullptr;cl_kernel kernel=nullptr;
 std::string name;
 GPU(const std::string &kernelPath){
  cl_int err=0;cl_uint np=0;
  ck(cl.clGetPlatformIDs(0,nullptr,&np),"clGetPlatformIDs");
  if(!np)throw std::runtime_error("No OpenCL platforms found (install AMD GPU driver)");
  std::vector<cl_platform_id> platforms(np);
  ck(cl.clGetPlatformIDs(np,platforms.data(),nullptr),"platform list");
  int bestScore=-1;
  for(auto p:platforms){
   cl_uint nd=0;
   if(cl.clGetDeviceIDs(p,CL_DEVICE_TYPE_GPU_,0,nullptr,&nd)||!nd)continue;
   std::vector<cl_device_id> ds(nd);
   if(cl.clGetDeviceIDs(p,CL_DEVICE_TYPE_GPU_,nd,ds.data(),nullptr))continue;
   for(auto d:ds){
    char vendor[256]={},devname[256]={};
    cl.clGetDeviceInfo(d,CL_DEVICE_VENDOR_,sizeof(vendor),vendor,nullptr);
    cl.clGetDeviceInfo(d,CL_DEVICE_NAME_,sizeof(devname),devname,nullptr);
    std::string v=vendor,n=devname;
    int rank=((v.find("AMD")!=std::string::npos||v.find("Advanced Micro")!=std::string::npos)?10:0);
    if(n.find("7800")!=std::string::npos)rank+=5;
    if(rank>bestScore){bestScore=rank;device=d;name=v+" / "+n;}
   }
  }
  if(!device)throw std::runtime_error("No OpenCL GPU device available");
  ctx=cl.clCreateContext(nullptr,1,&device,nullptr,nullptr,&err);ck(err,"context");
  q=cl.clCreateCommandQueue(ctx,device,0,&err);ck(err,"queue");
  std::ifstream f(kernelPath,std::ios::binary);
  if(!f)throw std::runtime_error("Cannot open GPU kernel file: "+kernelPath+" (run from deadbush15 folder)");
  std::string source((std::istreambuf_iterator<char>(f)),{});
  const char*txt=source.c_str();size_t sz=source.size();
  program=cl.clCreateProgramWithSource(ctx,1,&txt,&sz,&err);ck(err,"create program");
  err=cl.clBuildProgram(program,1,&device,"-cl-std=CL1.2",nullptr,nullptr);
  if(err){
   size_t len=0;cl.clGetProgramBuildInfo(program,device,CL_PROGRAM_BUILD_LOG_,0,nullptr,&len);
   std::string log(len,'\0');if(len)cl.clGetProgramBuildInfo(program,device,CL_PROGRAM_BUILD_LOG_,len,log.data(),nullptr);
   throw std::runtime_error("GPU kernel build failed: "+log);
  }
  kernel=cl.clCreateKernel(program,"scout",&err);ck(err,"create kernel");
 }
 ~GPU(){
  if(kernel)cl.clReleaseKernel(kernel);
  if(program)cl.clReleaseProgram(program);
  if(q)cl.clReleaseCommandQueue(q);
  if(ctx)cl.clReleaseContext(ctx);
 }
 std::vector<uint8_t> run(u64 start,u64 offset,int seeds,int locations,int cx,int cz,int spread,bool flat,int index,int cutoff){
  if(seeds<=0||locations<=0)throw std::runtime_error("GPU batch cannot be empty");
  const size_t n=size_t(seeds)*size_t(locations);
  cl_int err=0;
  cl_mem buf=cl.clCreateBuffer(ctx,CL_MEM_WRITE_ONLY_,n*sizeof(uint8_t),nullptr,&err);ck(err,"buffer");
  try {
   cl_ulong cs=start,co=offset;cl_uint ll=cl_uint(locations),ix=cl_uint(index);
   cl_int bx=cx,bz=cz,sp=spread,fl=flat?1:0,cut=cutoff;
#define ARG(i,v) ck(cl.clSetKernelArg(kernel,i,sizeof(v),&(v)),"kernel arg " #i)
   ARG(0,buf);ARG(1,cs);ARG(2,co);ARG(3,ll);ARG(4,bx);ARG(5,bz);
   ARG(6,sp);ARG(7,fl);ARG(8,ix);ARG(9,cut);
#undef ARG
   ck(cl.clEnqueueNDRangeKernel(q,kernel,1,nullptr,&n,nullptr,0,nullptr,nullptr),"kernel dispatch");
   std::vector<uint8_t> scores(n);
   ck(cl.clEnqueueReadBuffer(q,buf,CL_TRUE_,0,n*sizeof(uint8_t),scores.data(),0,nullptr,nullptr),"read scores");
   cl.clReleaseMemObject(buf);
   return scores;
  }catch(...){cl.clReleaseMemObject(buf);throw;}
 }
};
static std::pair<int,int> sampleCenter(u64 seed,int loc,int cx,int cz,int spread){
 if(!spread)return {cx,cz};
 u64 sx=seed^0xa0761d6478bd642fULL,sz=seed^0xe7037ed1a0b428dbULL;
 if(loc){sx^=u64(loc)*GOLDEN;sz^=u64(loc)*0xbf58476d1ce4e5b9ULL;}
 u64 span=2ULL*spread+1;
 return {cx+int(mix13(sx)%span)-spread,cz+int(mix13(sz)%span)-spread};
}
static int hostHasRun(u64 mask,int cutoff){
 if(!mask)return 0;u64 cont=mask;int covered=1;
 while(2*covered<=cutoff){cont&=cont>>covered;covered*=2;if(!cont)return 0;}
 for(int i=covered;i<cutoff;i++){cont&=mask>>i;if(!cont)return 0;}
 return cont!=0;
}
static int hostMaxRun(u64 mask){int len=0;while(mask){mask&=mask>>1;++len;}return len;}
static int cpuSelfTest(int index){
 char arg[]="--self-test",prog[]="v4";char*args[]={prog,arg};
 if(deadbush15_legacy_main(2,args))return 1;
 for(u64 s:{0ULL,1ULL,23456ULL,923652124993224547ULL,static_cast<unsigned long long>(-6825794747614541757LL)}) {
  for(int cx:{0,1033133,-12345}){
   const auto a=evaluateRegion(s,index,1,true,false,cx,-1601222,1,1);
   const auto b=evaluateRegion(s,index,1,false,false,cx,-1601222,1,1);
   if(a.run>b.run){std::cerr<<"ERROR: xz upper bound violated\n";return 1;}
  }
 }
 u64 state=0x12daab34f8193011ULL;
 for(int j=0;j<200000;j++){
  state=mix13(state+GOLDEN);u64 m=state;
  if(j%3==0)m&=mix13(state+21)&mix13(state+37);
  const int maxRun=hostMaxRun(m);
  for(int cutoff:{1,2,3,4,5,8,10,12,15,24,32}) {
   if(bool(hostHasRun(m,cutoff))!=(maxRun>=cutoff)){
    std::cerr<<"ERROR: sparse prefilter mismatch at "<<j<<", cutoff="<<cutoff<<"\n";return 1;
   }
  }
 }
 std::cout<<"PASS 2.2 million sparse prefilter assertions and XZ upper-bound check\n";
 return 0;
}
static void v4Help(){
 std::cout<<"Deadbush15 v13 OpenCL GPU candidate finder (terrain not verified)\n"
 <<" --cpu-self-test | --gpu-self-test [--kernel deadbush15_gpu_v13.cl]\n"
 <<" --gpu-search --start 0 --count 1000000 --batch 8192 --locations 4\n"
 <<"     --chunk-x 0 --chunk-z 0 --spread-span 1000000\n"
 <<"     --report 12 --prefilter 12 --target 15 --mode xz --output gpu_candidates_v4.csv\n"
 <<"     --checkpoint gpu_progress.log --resume [--mode flat|xz]\n"
 <<" GPU reports exact scores >= --prefilter and zero otherwise; --prefilter <= --report.\n GPU always scores a radius-1 (3x3) decoration grid PER location.\n"
 <<" Use v2 --search for larger contiguous regions. Actual-world verification: verify_world.py\n";
}
int main(int argc,char**argv){
 bool test=false,search=false,resume=false,cpuTest=false;u64 start=0,count=1000000;
 int cx=0,cz=0,spread=1000000,locations=4,batch=8192,report=12,target=15,index=75,prefilter=12;
 std::string output="gpu_candidates_v13.csv",checkpoint="",kernelPath="deadbush15_gpu_v13.cl",mode="xz";
 try{
  for(int i=1;i<argc;i++){
   std::string a=argv[i];auto val=[&](){if(++i>=argc)throw std::runtime_error("missing value: "+a);return std::string(argv[i]);};
   if(a=="--help"||a=="-h"){v4Help();return 0;}
   else if(a=="--gpu-self-test")test=true;
   else if(a=="--cpu-self-test")cpuTest=true;
   else if(a=="--gpu-search")search=true;
   else if(a=="--start")start=parseU(val());else if(a=="--count")count=parseU(val());
   else if(a=="--batch")batch=std::stoi(val());else if(a=="--locations")locations=std::stoi(val());
   else if(a=="--chunk-x")cx=std::stoi(val());else if(a=="--chunk-z")cz=std::stoi(val());
   else if(a=="--spread-span")spread=std::stoi(val());
   else if(a=="--report")report=std::stoi(val());else if(a=="--target")target=std::stoi(val());
   else if(a=="--prefilter")prefilter=std::stoi(val());
   else if(a=="--index")index=std::stoi(val());else if(a=="--mode")mode=val();
   else if(a=="--output")output=val();else if(a=="--checkpoint")checkpoint=val();
   else if(a=="--resume")resume=true;else if(a=="--kernel")kernelPath=val();
   else throw std::runtime_error("unknown option: "+a);
  }
  if(!test&&!search&&!cpuTest){v4Help();return 0;}
  if(batch<1||batch>262144||locations<1||locations>64||size_t(batch)*locations>1048576
     ||spread<0||spread>1800000||index<0||report<1||target<1||prefilter<1||prefilter>32||report<prefilter||count>std::numeric_limits<u64>::max()-start
     ||(mode!="flat"&&mode!="xz")||std::abs(int64_t(cx))+int64_t(spread)+2>1870000
     ||std::abs(int64_t(cz))+int64_t(spread)+2>1870000||(!spread&&locations>1))
   throw std::runtime_error("invalid options (GPU 3x3, batch*locations <=1048576, coordinates within world border; locations>1 requires spread)");
  if(cpuTest)return cpuSelfTest(index);
  GPU gpu(kernelPath);
  std::cout<<"OpenCL GPU: "<<gpu.name<<"\n";
  if(test){
   bool ok=true;
   std::cout<<"INFO known 14-bush row is validated by V12's biome-branch upper-bound self-test; this GPU stage scouts the all-accepted trace only.\n";
   for(bool flat:{true,false})for(int spreadCase:{0,1000000})for(int cutoff:{2,5,8,10,12,15}){
    int locs=spreadCase?3:1;
    auto scores=gpu.run(0,0,96,locs,0,0,spreadCase,flat,75,cutoff);
    int bad=0;
    for(int j=0;j<96;j++)for(int l=0;l<locs;l++){
     auto [x,z]=sampleCenter(u64(j),l,0,0,spreadCase);
     int expected=evaluateRegion(u64(j),75,1,flat,false,x,z,1,1).run;
     if(scores[size_t(j)*locs+l]!=(expected>=cutoff?expected:0)){if(bad++<3)
       std::cerr<<"MISMATCH seed="<<j<<" loc="<<l<<" GPU="<<scores[size_t(j)*locs+l]<<" CPU="<<expected<<"\n";}
    }
    bool equal=bad==0;ok&=equal;
    std::cout<<(equal?"PASS":"FAIL")<<" CPU/GPU parity 96 seeds, "<<locs<<" locations, mode="<<(flat?"flat":"xz")<<", prefilter="<<cutoff<<"; mismatches="<<bad<<"\n";
   }
   return ok?0:1;
  }
  if(resume&&checkpoint.empty())throw std::runtime_error("--resume requires --checkpoint");
  // Versioned signature prevents resuming scans from the old draw ordering.
  std::ostringstream os;os<<"D15V13|"<<start<<'|'<<count<<'|'<<index<<'|'<<cx<<'|'<<cz<<'|'<<spread<<'|'<<locations
         <<'|'<<report<<'|'<<prefilter<<'|'<<target<<'|'<<mode<<'|'<<output;
  std::string sig=std::to_string(hashString(os.str()));
  if(!checkpoint.empty()&&std::filesystem::exists(checkpoint)&&!resume)throw std::runtime_error("checkpoint exists; pass --resume");
  u64 offset=(!checkpoint.empty()&&resume&&std::filesystem::exists(checkpoint))?readCheckpoint(checkpoint,sig,count):0;
  if(!checkpoint.empty()&&offset==0)appendCheckpoint(checkpoint,sig,0);
  std::string header="seed_unsigned,seed_signed,run,start_x,start_z,dx,dz,mode,chunk_radius,global_index,region_chunk_x,region_chunk_z,location_index,locations_per_seed,terrain_verified,flat_run,xz_run";
  if(std::filesystem::exists(output)&&std::filesystem::file_size(output)){
   std::ifstream f(output);std::string first;std::getline(f,first);
   if(first!=header)throw std::runtime_error("CSV header mismatch, select fresh output file");
  }
  std::ofstream csv(output,std::ios::app);if(!csv)throw std::runtime_error("cannot open output CSV");
  if(csv.tellp()==0)csv<<header<<'\n';
  int best=0;u64 hits=0;
  auto t0=std::chrono::steady_clock::now();
  std::cout<<"GPU search seeds="<<count<<", start="<<start<<", resume="<<offset<<", locations/seed="<<locations
           <<", total candidates="<<count*uint64_t(locations)<<", batch="<<batch<<", mode="<<mode<<", prefilter="<<prefilter<<"\n";
  u64 done=offset;
  while(done<count){
   int n=int(std::min<u64>(batch,count-done));
   auto scores=gpu.run(start,done,n,locations,cx,cz,spread,mode=="flat",index,prefilter);
   for(int j=0;j<n;j++)for(int l=0;l<locations;l++){
    int score=scores[size_t(j)*locations+l];best=std::max(best,score);
    if(score>=report){
     const u64 seed=start+done+u64(j);
     auto [x,z]=sampleCenter(seed,l,cx,cz,spread);
     Hit hit=evaluateRegion(seed,index,1,mode=="flat",false,x,z,1,1);
     // CPU recomputes exact flat and optimistic XZ rows for each GPU-positive candidate.
     Hit flatHit=(mode=="flat")?hit:evaluateRegion(seed,index,1,true,false,x,z,1,1);
     Hit xzHit=(mode=="xz")?hit:evaluateRegion(seed,index,1,false,false,x,z,1,1);
     if(hit.run!=score)throw std::runtime_error("GPU/CPU mismatch on a candidate! GPU="+std::to_string(score)+", CPU="+std::to_string(hit.run));
     long long absX=x*16LL+hit.x,absZ=z*16LL+hit.z;
     csv<<seed<<','<<static_cast<int64_t>(seed)<<','<<hit.run<<','<<absX<<','<<absZ<<','<<hit.dx<<','<<hit.dz
        <<','<<mode<<",1,"<<index<<','<<x<<','<<z<<','<<l<<','<<locations<<",0,"<<flatHit.run<<','<<xzHit.run<<"\n";
     hits++;
     if(hit.run>=target)std::cout<<"CANDIDATE ONLY seed="<<static_cast<int64_t>(seed)<<" run="<<hit.run
            <<" at "<<absX<<','<<absZ<<" (NOT terrain verified)\n";
    }
   }
   csv.flush();if(!csv)throw std::runtime_error("CSV write failed");
   done+=u64(n);
   if(!checkpoint.empty())appendCheckpoint(checkpoint,sig,done);
   if(done==count||done%uint64_t(batch*10)==0){
    double secs=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
    std::cout<<"Progress "<<done<<'/'<<count<<", best="<<best<<", hits="<<hits
             <<", "<<std::fixed<<std::setprecision(0)<<((done-offset)/std::max(0.001,secs))<<" seeds/s, "
             <<((done-offset)*locations/std::max(0.001,secs))<<" location checks/s\n"<<std::flush;
   }
  }
  std::cout<<"Done; best recorded RNG-only="<<best<<", hits="<<hits<<", candidate CSV="<<output<<" (scores below prefilter are returned as 0)\n";
  return 0;
 }catch(const std::exception &e){std::cerr<<"ERROR: "<<e.what()<<'\n';return 2;}
}
