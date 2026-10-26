// V12 lossless RNG/biome-branch upper-bound filter for Minecraft Java 26.3.
// It intentionally does NOT claim generated blocks or terrain confirmation.
#define main deadbush15_legacy_main
#include "deadbush15.cpp"
#undef main
#include <filesystem>
#include "vendor/cubiomes/generator.h"
#include <unordered_map>
#include <unordered_set>

struct Row { int64_t seed; int x,z,dx,dz,n; };
static int floorDiv16(int x) { return x >= 0 ? x/16 : -int((-(int64_t)x+15)/16); }
static std::vector<std::string> csv(const std::string &s) {
    std::vector<std::string> out; std::string v; bool q=false;
    for(size_t i=0;i<s.size();++i) { char c=s[i];
        if(c=='"') { if(q&&i+1<s.size()&&s[i+1]=='"'){v+='"';++i;} else q=!q; }
        else if(c==','&&!q){out.push_back(v);v.clear();} else v+=c;
    } out.push_back(v); return out;
}
static int tri(Xoro &r,int k) { return r.pow2(k)-r.pow2(k); }
static bool badlandsBiome(int id) { return id==badlands||id==wooded_badlands||id==eroded_badlands; }
static bool commonBadlandsY(const Generator &g,const Row &r) {
    for(int qy=-16;qy<=80;++qy) {
        bool all=true;
        for(int i=0;i<r.n;++i) {
            int x=r.x+i*r.dx,z=r.z+i*r.dz;
            int qx=x>=0?x/4:-int((-(int64_t)x+3)/4), qz=z>=0?z/4:-int((-(int64_t)z+3)/4);
            if(!badlandsBiome(getBiomeAt(&g,4,qx,qy,qz))){all=false;break;}
        }
        if(all)return true;
    }
    return false;
}

// FeaturePlacer runs each modifier over the full position list. Therefore all
// 20 square samples precede biome filtering, then accepted bases consume their
// four offsets in order. Branch over both filter outcomes; all actual traces
// are included, while the union is only an optimistic upper bound.
static std::unordered_set<uint64_t> possiblePositions(uint64_t seed,int cx,int cz,int index) {
    Xoro afterBases(feature_seed(seed,cx,cz,index));
    int bx[20],bz[20];
    for(int i=0;i<20;++i){bx[i]=cx*16+afterBases.pow2(4);bz[i]=cz*16+afterBases.pow2(4);}
    std::vector<Xoro> states{afterBases};
    std::unordered_set<uint64_t> positions;
    for(int attempt=0;attempt<20;++attempt) {
        std::vector<Xoro> next; next.reserve(states.size()*2);
        auto add=[&](const Xoro &r){
            for(const Xoro &old:next) if(old.a==r.a&&old.b==r.b) return;
            next.push_back(r);
        };
        for(const Xoro &before:states) {
            add(before); // biome rejects this already-sampled base
            Xoro accepted=before;
            for(int i=0;i<4;++i) {
                int x=bx[attempt]+tri(accepted,3), y=tri(accepted,2), z=bz[attempt]+tri(accepted,3);
                (void)y;
                uint64_t key=(uint64_t(uint32_t(x))<<32)|uint32_t(z);
                positions.insert(key);
            }
            add(accepted); // biome accepts; 4 offsets consume 24 bounded draws
        }
        states.swap(next);
    }
    return positions;
}
static bool supportsRow(const Row &r,int index) {
    int x2=r.x+(r.n-1)*r.dx, z2=r.z+(r.n-1)*r.dz;
    int minx=std::min(r.x,x2), maxx=std::max(r.x,x2), minz=std::min(r.z,z2), maxz=std::max(r.z,z2);
    // A chunk's base square is [16c,16c+15], followed by [-7,+7] offsets.
    int cxl=floorDiv16(minx-22), cxh=floorDiv16(maxx+7);
    int czl=floorDiv16(minz-22), czh=floorDiv16(maxz+7);
    std::unordered_set<uint64_t> unionPos;
    for(int cx=cxl;cx<=cxh;++cx) for(int cz=czl;cz<=czh;++cz) {
        auto p=possiblePositions(uint64_t(r.seed),cx,cz,index);
        unionPos.insert(p.begin(),p.end());
    }
    for(int i=0;i<r.n;++i) {
        int x=r.x+i*r.dx,z=r.z+i*r.dz;
        if(!unionPos.count((uint64_t(uint32_t(x))<<32)|uint32_t(z))) return false;
    }
    return true;
}
static bool validate(const Row&r){return r.n>=1&&r.n<=64&&((r.dx==1&&r.dz==0)||(r.dx==0&&r.dz==1));}
int main(int argc,char**argv) {
    try {
        std::string input,output; int imin=75,imax=75; bool self=false,resume=false; uint64_t limit=UINT64_MAX;
        for(int i=1;i<argc;++i){std::string a=argv[i];auto val=[&](){if(++i>=argc)throw std::runtime_error("missing value for "+a);return std::string(argv[i]);};
            if(a=="--input")input=val();else if(a=="--output")output=val();
            else if(a=="--index"){imin=imax=std::stoi(val());}else if(a=="--index-range"){imin=std::stoi(val());imax=std::stoi(val());}
            else if(a=="--limit")limit=std::stoull(val());else if(a=="--self-test")self=true;else if(a=="--resume")resume=true;
            else if(a=="--help"){std::cout<<"--self-test | --input candidates.csv --output v12_upper_bound.csv [--index N | --index-range A B] [--limit N] [--resume]\n";return 0;}
            else throw std::runtime_error("unknown option "+a);
        }
        if(imin<0||imax<imin||imax>1024)throw std::runtime_error("index range must be 0..1024");
        if(self){
            const Row refs[]={{-6825794747614541757LL,16530130,-25619539,1,0,14},{923652124993224547LL,5653714,10797037,1,0,12}};
            Generator g;setupGenerator(&g,MC_26_3,0);
            for(const auto&r:refs){applySeed(&g,DIM_OVERWORLD,uint64_t(r.seed));if(!commonBadlandsY(g,r)){std::cerr<<"FAIL published row lacks a common Badlands-family biome Y\n";return 2;}bool ok=false;for(int k=imin;k<=imax&&!ok;++k)ok=supportsRow(r,k);
                std::cout<<(ok?"PASS":"FAIL")<<" published row is possible under RNG/biome upper bound; index="<<imin<<".."<<imax<<"\n";
                if(!ok)return 2;
            } return 0;
        }
        if(input.empty()||output.empty())throw std::runtime_error("--input and --output are required");
        if(std::filesystem::absolute(input)==std::filesystem::absolute(output))throw std::runtime_error("input and output paths must differ");
        std::ifstream in(input);if(!in)throw std::runtime_error("cannot open input");
        auto inpath=std::filesystem::absolute(input);
        std::string meta=output+".meta", signature="D15V12\n"+inpath.string()+"\n"+std::to_string(std::filesystem::file_size(inpath))+"\n"+std::to_string(std::filesystem::last_write_time(inpath).time_since_epoch().count())+"\n"+std::to_string(imin)+"\n"+std::to_string(imax)+"\n";
        if(resume){std::ifstream mf(meta);std::string old((std::istreambuf_iterator<char>(mf)),{});if(!mf||old!=signature||!std::filesystem::exists(output))throw std::runtime_error("resume metadata mismatch or output missing");}
        else {std::ofstream mf(meta,std::ios::trunc);mf<<signature;if(!mf)throw std::runtime_error("cannot write resume metadata");}
        std::ofstream out(output,resume?std::ios::app:std::ios::trunc);if(!out)throw std::runtime_error("cannot open output");
        std::string line;if(!std::getline(in,line))throw std::runtime_error("empty CSV");if(line.size()>=3&&(unsigned char)line[0]==0xef&&(unsigned char)line[1]==0xbb&&(unsigned char)line[2]==0xbf)line.erase(0,3);auto h=csv(line);std::unordered_map<std::string,int> col;
        for(int i=0;i<(int)h.size();++i)col[h[i]]=i;
        for(const char*k:{"seed_signed","start_x","start_z","dx","dz","run"})if(!col.count(k))throw std::runtime_error(std::string("missing CSV field ")+k);
        uint64_t rows=0,possible=0,skip=0;
        if(resume){std::ifstream prior(output);std::string oldline;std::getline(prior,oldline);while(std::getline(prior,oldline))if(!oldline.empty())++skip;}
        else out<<line<<",v12_badlands_possible,v12_rng_possible,v12_possible_index,v12_scope\n";
        auto t0=std::chrono::steady_clock::now();uint64_t seen=0;
        while(rows<limit&&std::getline(in,line)){if(line.empty())continue;auto f=csv(line);if(f.size()!=h.size())continue;if(seen++<skip)continue;
            Row r{std::stoll(f[col["seed_signed"]]),std::stoi(f[col["start_x"]]),std::stoi(f[col["start_z"]]),std::stoi(f[col["dx"]]),std::stoi(f[col["dz"]]),std::stoi(f[col["run"]])};
            if(!validate(r))continue;++rows;Generator g;setupGenerator(&g,MC_26_3,0);applySeed(&g,DIM_OVERWORLD,uint64_t(r.seed));bool biome=commonBadlandsY(g,r);int found=-1;if(biome)for(int k=imin;k<=imax;++k)if(supportsRow(r,k)){found=k;break;}
            if(found>=0)++possible;out<<line<<','<<(biome?"POSSIBLE":"NO_COMMON_BADLANDS_Y")<<','<<(found>=0?"POSSIBLE":"EXCLUDED_BY_RNG_XZ")<<','<<(found>=0?std::to_string(found):"")<<",BADLANDS_AND_BIOME_BRANCH_UPPER_BOUNDS\n";out.flush();
            if(rows%100==0)std::cerr<<"Checked "<<rows<<" candidates; possible "<<possible<<"\r";
        }
        double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        std::cout<<"Checked "<<rows<<" rows; "<<possible<<" pass upper bound; "<<(rows/sec)<<" candidates/s.\n"
                 <<"An exclusion is valid only for supplied feature-index range; pass means RNG XZ possibility, not a generated row.\n";
        return 0;
    }catch(const std::exception&e){std::cerr<<"ERROR: "<<e.what()<<"\n";return 1;}
}
