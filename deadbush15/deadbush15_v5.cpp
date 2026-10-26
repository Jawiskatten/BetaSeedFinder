// Deadbush15 V5 row-specific placement offset annotator and shortlist builder.
// Does not claim to model terrain. Replays the exact same Xoroshiro decoration
// RNG as V4, but evaluates dy=0 specifically on the *reported XZ row*.
// Compiled independently of V4 by including its existing implementation.
#define main deadbush15_legacy_main
#include "deadbush15.cpp"
#undef main
#include <map>
#include <cmath>
#include <queue>
#include <set>

struct Record {
    std::string source;
    int64_t seed, x, z;
    int run, dx, dz;
    int sameFlatLongest, sameFlatCount, yOffsetCost;
    int fullFlatScore;
};

static std::vector<std::string> splitCsv(const std::string &s) {
    // CSV schema consists solely of unquoted numeric and mode columns.
    std::vector<std::string> v; std::string item; std::istringstream ss(s);
    while(std::getline(ss,item,','))v.push_back(item);
    return v;
}
static std::vector<int> dyRowMasks(u64 seed,int x,int z,int dx,int dz,int run,int index) {
    if(run < 1 || run > 64 || (dx!=0&&dx!=1) || (dz!=0&&dz!=1) || dx+dz!=1)
        throw std::runtime_error("invalid candidate direction/length");
    // Conservative enumeration: include any chunk origin whose +/-7 offset
    // could place a bush at any target coordinate. Each chunk stream once.
    int minx=std::min(x,x+dx*(run-1)), maxx=std::max(x,x+dx*(run-1));
    int minz=std::min(z,z+dz*(run-1)), maxz=std::max(z,z+dz*(run-1));
    int cxl= int(std::floor(double(minx-8)/16.0)),cxh=int(std::floor(double(maxx+8)/16.0));
    int czl= int(std::floor(double(minz-8)/16.0)),czh=int(std::floor(double(maxz+8)/16.0));
    std::vector<int> out(run,0);
    for(int cx=cxl;cx<=cxh;cx++)for(int cz=czl;cz<=czh;cz++) {
        Xoro r(feature_seed(seed,cx,cz,index));
        for(int n=0;n<20;n++) {
            int bx=cx*16+r.pow2(4),bz=cz*16+r.pow2(4);
            for(int i=0;i<4;i++) {
                int px=bx+r.pow2(3)-r.pow2(3);
                int dy=r.pow2(2)-r.pow2(2);
                int pz=bz+r.pow2(3)-r.pow2(3);
                int position=(dx?px-x:pz-z);
                if(position>=0&&position<run && px==x+dx*position && pz==z+dz*position)
                    out[position] |= 1 << (dy+3);
            }
        }
    }
    return out;
}
static Record annotate(const std::vector<std::string>&v,const std::map<std::string,int>&h,int index){
    auto f=[&](const char*name)->std::string{
        auto it=h.find(name);if(it==h.end()||it->second >= int(v.size()))throw std::runtime_error(std::string("missing CSV column ")+name);
        return v[it->second];
    };
    Record a{};
    a.seed=std::stoll(f("seed_signed"));a.x=std::stoll(f("start_x"));a.z=std::stoll(f("start_z"));
    a.run=std::stoi(f("run"));a.dx=std::stoi(f("dx"));a.dz=std::stoi(f("dz"));
    try {a.fullFlatScore=std::stoi(f("flat_run"));}catch(...){a.fullFlatScore=-1;}
    if(a.x < std::numeric_limits<int>::min()+100 || a.x > std::numeric_limits<int>::max()-100 ||
       a.z < std::numeric_limits<int>::min()+100 || a.z > std::numeric_limits<int>::max()-100)
        throw std::runtime_error("out of range candidate coordinates");
    auto masks=dyRowMasks(u64(a.seed),int(a.x),int(a.z),a.dx,a.dz,a.run,index);
    int streak=0;
    for(int mask:masks){
        if(mask&(1<<3)){++a.sameFlatCount;streak++;a.sameFlatLongest=std::max(streak,a.sameFlatLongest);}else streak=0;
        int minimum=10;
        for(int dy=-3;dy<=3;dy++)if(mask&(1<<(dy+3)))minimum=std::min(minimum,std::abs(dy));
        a.yOffsetCost+=minimum;
    }
    return a;
}
static bool better(const Record&a,const Record&b){
    if(a.sameFlatLongest!=b.sameFlatLongest)return a.sameFlatLongest>b.sameFlatLongest;
    if(a.sameFlatCount!=b.sameFlatCount)return a.sameFlatCount>b.sameFlatCount;
    if(a.yOffsetCost!=b.yOffsetCost)return a.yOffsetCost<b.yOffsetCost;
    if(a.run!=b.run)return a.run>b.run;
    return a.seed<b.seed;
}
static int runRank(const std::string&input,const std::string&output,int minrun,int top,int index){
    std::ifstream csv(input);if(!csv)throw std::runtime_error("cannot open "+input);
    std::string header,line;if(!std::getline(csv,header))throw std::runtime_error("empty candidate CSV");
    auto columns=splitCsv(header);std::map<std::string,int> col;
    for(int i=0;i<int(columns.size());i++)col[columns[i]]=i;
    if(!col.count("run"))throw std::runtime_error("missing run column");
    std::vector<Record> rec;size_t scanned=0,qualified=0;
    while(std::getline(csv,line)){
        if(line.empty())continue;++scanned;
        auto vals=splitCsv(line);
        if(int(vals.size())!=int(columns.size()))throw std::runtime_error("CSV malformed at row "+std::to_string(scanned));
        if(std::stoi(vals[col["run"]])<minrun)continue;
        ++qualified;
        auto r=annotate(vals,col,index);rec.push_back(r);
    }
    std::sort(rec.begin(),rec.end(),better);
    std::set<int64_t> unique;
    std::ofstream out(output);if(!out)throw std::runtime_error("cannot write "+output);
    out<<"seed_signed,run,start_x,start_z,dx,dz,same_row_flat_longest,same_row_flat_count,offset_cost,full_area_flat_run,terrain_verified\n";
    int written=0;
    for(const auto&r:rec){
        if(!unique.insert(r.seed).second)continue;
        out<<r.seed<<','<<r.run<<','<<r.x<<','<<r.z<<','<<r.dx<<','<<r.dz<<','<<r.sameFlatLongest<<','
           <<r.sameFlatCount<<','<<r.yOffsetCost<<','<<r.fullFlatScore<<",0\n";
        if(++written>=top)break;
    }
    std::cout<<"Scanned "<<scanned<<" candidates, "<<qualified<<" with XZ >= "<<minrun
             <<"; selected "<<written<<" unique world seeds to "<<output<<"\n";
    if(written)std::cout<<"Best same-row flat uninterrupted score: "<<rec.front().sameFlatLongest
                        <<" of XZ="<<rec.front().run<<" (NOT biome or terrain verified)\n";
    return 0;
}
static int selfTestV5(){
    char prog[]="v5",test[]="--self-test"; char*argv[]={prog,test};
    if(deadbush15_legacy_main(2,argv))return 1;
    struct Example{int64_t seed;int x,z,run;};
    for(const Example a: {Example{-6825794747614541757LL,16530130,-25619539,14},
                           Example{923652124993224547LL,5653714,10797037,12}}){
        auto mask=dyRowMasks(u64(a.seed),a.x,a.z,1,0,a.run,75);
        bool pass=std::all_of(mask.begin(),mask.end(),[](int m){return (m&(1<<3))!=0;});
        std::cout<<(pass?"PASS":"FAIL")<<" published "<<a.run<<" row has dy=0 at each target XZ\n";
        if(!pass)return 1;
    }
    return 0;
}
int main(int argc,char**argv){
    try{
        bool rank=false,test=false;std::string input="gpu_v4_candidates.csv",output="v5_shortlist.csv";
        int minrun=15,top=20,index=75;
        for(int i=1;i<argc;i++){
            std::string a=argv[i];auto val=[&](){if(++i>=argc)throw std::runtime_error("missing "+a);return std::string(argv[i]);};
            if(a=="--rank")rank=true;else if(a=="--self-test")test=true;
            else if(a=="--csv")input=val();else if(a=="--output")output=val();
            else if(a=="--min-run")minrun=std::stoi(val());else if(a=="--top")top=std::stoi(val());
            else if(a=="--index")index=std::stoi(val());
            else if(a=="--help"||a=="-h"){
                std::cout<<"--self-test | --rank --csv gpu_v4_candidates.csv --min-run 15 --top 20 --output v5_shortlist.csv\n";
                return 0;
            }else throw std::runtime_error("unknown argument "+a);
        }
        if(test)return selfTestV5();
        if(!rank||top<1||top>10000||minrun<1||minrun>64||index<0)throw std::runtime_error("use --rank and valid options");
        return runRank(input,output,minrun,top,index);
    }catch(const std::exception&e){std::cerr<<"ERROR: "<<e.what()<<'\n';return 2;}
}