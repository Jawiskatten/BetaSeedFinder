// Deadbush15 V9 — approximate terrain-aware candidate prioritizer for NORMAL 26.3 worlds.
// Cubiomes mapApproxHeight IS NOT vanilla heightmap or exact terrain generation.
// No result from this program is a verified naturally generated dead-bush row.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "generator.h"
using U = uint64_t;
static constexpr U SILVER = 7640891576956012809ULL, GOLDEN=0x9e3779b97f4a7c15ULL;
static U rot(U v,int n){return (v<<n)|(v>>(64-n));}
static U mix(U x){x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;x=(x^(x>>27))*0x94d049bb133111ebULL;return x^(x>>31);}
struct Xoro {
 U a,b;
 explicit Xoro(U seed):a(mix(seed^SILVER)),b(mix((seed^SILVER)+GOLDEN)){if(!(a|b)){a=GOLDEN;b=SILVER;}}
 U next(){U x=a,y=b,v=rot(x+y,17)+x;y^=x;a=rot(x,49)^y^(y<<21);b=rot(y,28);return v;}
 int pow2(int k){return int(next()>>(64-k));}
 U javaLong(){U hi=uint32_t(next()>>32);auto lo=int32_t(next()>>32);return (hi<<32)+U(int64_t(lo));}
};
static U featureSeed(U ws,int cx,int cz){Xoro r(ws);U xs=r.javaLong()|1ULL,zs=r.javaLong()|1ULL;
 U dec=(U(int64_t(cx)*16)*xs+U(int64_t(cz)*16)*zs)^ws;return dec+75+90000ULL;}
static int floorDiv(int v,int d){return v>=0?v/d:-int((-(int64_t)v+d-1)/d);}
static bool isBad(int id){return id==badlands||id==wooded_badlands||id==eroded_badlands;}
static std::vector<std::string> splitCsv(const std::string& s){
 std::vector<std::string> v;std::string x;bool quote=false;
 for(size_t i=0;i<s.size();i++){char c=s[i];if(c=='"'){if(quote&&i+1<s.size()&&s[i+1]=='"'){x+='"';i++;}else quote=!quote;}
 else if(c==','&&!quote){v.push_back(x);x.clear();}else x+=c;}
 if(quote)throw std::runtime_error("unterminated CSV quote");
 v.push_back(x);return v;
}
static int col(const std::vector<std::string>& cols,const char* n){auto it=std::find(cols.begin(),cols.end(),n);if(it==cols.end())throw std::runtime_error(std::string("Missing CSV field ")+n);return int(it-cols.begin());}
static int integer(const std::string& v){size_t end=0;long long n=std::stoll(v,&end);if(end!=v.size()||n<-30000000||n>30000000)throw std::runtime_error("invalid coordinate: "+v);return int(n);}
static int64_t integer64(const std::string& v){size_t end=0;auto n=std::stoll(v,&end);if(end!=v.size())throw std::runtime_error("invalid long: "+v);return n;}
struct Row {int64_t seed;int x,z,dx,dz,len;};
struct Attempt{int baseX,baseZ,dy;};
struct Report{Row row;int strict=0,loose=0,flat=0,looseCount=0,atY=0;double error=1e9;bool approximateOk=true;};
struct Context {
 Generator g{};U last=0;bool ready=false;std::unordered_map<U,double> heights;
 Context(){setupGenerator(&g,MC_26_3,0);}
 void seed(U s){if(!ready||last!=s){applySeed(&g,DIM_OVERWORLD,s);heights.clear();last=s;ready=true;}}
 double height(int x,int z){
    int qx=floorDiv(x,4),qz=floorDiv(z,4);U key=(U(uint32_t(qx))<<32)|uint32_t(qz);
    auto it=heights.find(key);if(it!=heights.end())return it->second;
    float out=0;
    // Cubiomes x,z are quart coordinates. This is an approximation, not a heightmap.
    if(mapApproxHeight(&out,nullptr,&g,nullptr,qx,qz,1,1)!=0||!std::isfinite(out))
        throw std::runtime_error("Cubiomes could not estimate Overworld surface height");
    double value=double(out);heights.emplace(key,value);return value;
 }
};
static std::vector<std::vector<Attempt>> attempts(const Row &r){
 if(r.len<1||r.len>64||r.dx+r.dz!=1||(r.dx!=0&&r.dx!=1)||(r.dz!=0&&r.dz!=1))
    throw std::runtime_error("invalid row geometry");
 int x1=r.x+r.dx*(r.len-1),z1=r.z+r.dz*(r.len-1);
 int minX=std::min(r.x,x1),maxX=std::max(r.x,x1),minZ=std::min(r.z,z1),maxZ=std::max(r.z,z1);
 // Same +/-7 footprint as the verified V5 RNG replay, with base positions retained.
 std::vector<std::vector<Attempt>> result(r.len);
 for(int cx=floorDiv(minX-8,16);cx<=floorDiv(maxX+8,16);cx++)
 for(int cz=floorDiv(minZ-8,16);cz<=floorDiv(maxZ+8,16);cz++){
    Xoro random(featureSeed(U(r.seed),cx,cz));
    for(int j=0;j<20;j++){
        int baseX=cx*16+random.pow2(4),baseZ=cz*16+random.pow2(4);
        for(int k=0;k<4;k++){
            int px=baseX+random.pow2(3)-random.pow2(3);
            int dy=random.pow2(2)-random.pow2(2);
            int pz=baseZ+random.pow2(3)-random.pow2(3);
            int i=r.dx?px-r.x:pz-r.z;
            if(i>=0&&i<r.len&&px==r.x+i*r.dx&&pz==r.z+i*r.dz)
                result[i].push_back({baseX,baseZ,dy});
        }
    }
 }
 return result;
}
static Report score(Context& ctx,const Row& row,double tight,double wide){
 ctx.seed(U(row.seed)); auto masks=attempts(row);
 struct P {double y, support;int dy;};
 std::vector<std::vector<P>> options(row.len);
 std::vector<double> anchors;
 Report report{};report.row=row;
 int flatStreak=0;
 for(int i=0;i<row.len;i++){
    double targetH=ctx.height(row.x+row.dx*i,row.z+row.dz*i);
    bool hasZero=false;
    for(const Attempt &a:masks[i]){
        double h=ctx.height(a.baseX,a.baseZ);
        double placed=h+a.dy;
        options[i].push_back({placed,std::abs(placed-targetH),a.dy});
        anchors.push_back(placed);
        if(a.dy==0)hasZero=true;
    }
    flatStreak=hasZero?flatStreak+1:0;
    report.flat=std::max(report.flat,flatStreak);
 }
 if(anchors.empty())return report;
 std::sort(anchors.begin(),anchors.end());
 anchors.erase(std::unique(anchors.begin(),anchors.end()),anchors.end());
 for(double y:anchors){
    int tightRun=0,wideRun=0,bestTight=0,bestWide=0,wideCells=0;
    double totalErr=0;
    for(const auto& cell:options){
        double best=std::numeric_limits<double>::infinity();
        for(const auto& p:cell){
            // Separate approximate requirements: support surface at target and
            // same final planting Y throughout the run.
            double e=std::max(std::abs(p.y-y),p.support);
            best=std::min(best,e);
        }
        tightRun=best<=tight?tightRun+1:0;
        wideRun=best<=wide?wideRun+1:0;
        bestTight=std::max(bestTight,tightRun);
        bestWide=std::max(bestWide,wideRun);
        if(best<=wide){wideCells++;totalErr+=best;}
    }
    report.strict=std::max(report.strict,bestTight);
    report.looseCount=std::max(report.looseCount,wideCells);
    if(bestWide>report.loose||(bestWide==report.loose&&totalErr<report.error)){
        report.loose=bestWide;report.atY=int(std::lround(y));report.error=totalErr;
    }
 }
 return report;
}
static bool better(const Report&a,const Report&b){
 if(a.loose!=b.loose)return a.loose>b.loose;
 if(a.strict!=b.strict)return a.strict>b.strict;
 if(a.looseCount!=b.looseCount)return a.looseCount>b.looseCount;
 if(a.flat!=b.flat)return a.flat>b.flat;
 if(a.row.len!=b.row.len)return a.row.len>b.row.len;
 return a.row.seed<b.row.seed;
}
static int selfTest(double tight,double wide){
 Context ctx;
 std::vector<Row> test={{-6825794747614541757LL,16530130,-25619539,1,0,14},
 {923652124993224547LL,5653714,10797037,1,0,12},
 {300136533,-9734238,-12924882,1,0,8},
 {301572122,1656654,-2809583,1,0,8}};
 bool valid=true;
 for(const Row& r:test){
    Report p=score(ctx,r,tight,wide);
    int b=getBiomeAt(&ctx.g,4,floorDiv(r.x,4),floorDiv(96,4),floorDiv(r.z,4));
    std::cout<<"seed="<<r.seed<<" reference_len="<<r.len<<" dy0="<<p.flat<<" approx_strict="<<p.strict
             <<" approx_loose="<<p.loose<<" approxY="<<p.atY<<" sampled_badlands="<<isBad(b)<<"\n";
    if((r.len==14||r.len==12)&&p.flat!=r.len){std::cerr<<"RNG PARITY FAILURE on published reference!\n";valid=false;}
 }
 std::cout<<"Approximate height estimates are heuristic. Do not interpret scores as actual bushes.\n";
 return valid?0:3;
}
int main(int argc,char**argv){
 try{
    std::string input,output="v9_ranked.csv";int minrun=7,top=30;double tight=2,wide=6;bool test=false;
    for(int i=1;i<argc;i++){
        std::string a=argv[i];auto next=[&]()->std::string{if(++i>=argc)throw std::runtime_error("Missing value after "+a);return argv[i];};
        if(a=="--input")input=next();else if(a=="--output")output=next();
        else if(a=="--min-run")minrun=std::stoi(next());else if(a=="--top")top=std::stoi(next());
        else if(a=="--tight")tight=std::stod(next());else if(a=="--wide")wide=std::stod(next());
        else if(a=="--self-test")test=true;
        else if(a=="--help"){std::cout<<"--self-test OR --input v7_badlands.csv --output v9_ranked.csv --min-run 7 --top 30 [--tight 2 --wide 6]\n";return 0;}
        else throw std::runtime_error("Unknown option "+a);
    }
    if(minrun<1||minrun>64||top<1||top>10000||tight<0||wide<tight||wide>30)throw std::runtime_error("Invalid thresholds");
    if(test)return selfTest(tight,wide);
    if(input.empty())throw std::runtime_error("--input is required");
    std::ifstream csv(input);if(!csv)throw std::runtime_error("Cannot open "+input);
    std::string line;if(!std::getline(csv,line))throw std::runtime_error("CSV is empty");
    auto cols=splitCsv(line);if(cols.size()&&cols[0].substr(0,3)=="\xef\xbb\xbf")cols[0]=cols[0].substr(3);
    int s=col(cols,"seed_signed"),x=col(cols,"start_x"),z=col(cols,"start_z"),dx=col(cols,"dx"),dz=col(cols,"dz"),n=col(cols,"run");
    Context ctx;std::vector<Report> candidates;int seen=0,qualified=0,skipped=0;
    while(std::getline(csv,line)){
        if(line.empty())continue;
        seen++;
        try{
            auto v=splitCsv(line);if(v.size()!=cols.size())throw std::runtime_error("column count mismatch");
            Row row{integer64(v[s]),integer(v[x]),integer(v[z]),integer(v[dx]),integer(v[dz]),integer(v[n])};
            if(row.len<minrun)continue;
            qualified++;
            candidates.push_back(score(ctx,row,tight,wide));
        }catch(const std::exception& e){if(skipped++<5)std::cerr<<"Skipping invalid row: "<<e.what()<<"\n";}
    }
    std::sort(candidates.begin(),candidates.end(),better);
    std::ofstream out(output);if(!out)throw std::runtime_error("Cannot create "+output);
    out<<"seed_signed,run,start_x,start_z,dx,dz,approx_strict_run,approx_loose_run,approx_loose_cells,dy0_run,approx_y,tp_command,verification\n";
    std::set<int64_t> unique;int written=0;
    for(const auto& r:candidates){
        if(!unique.insert(r.row.seed).second)continue;
        std::string tp="/tp @s "+std::to_string(r.row.x)+" 160 "+std::to_string(r.row.z);
        out<<r.row.seed<<','<<r.row.len<<','<<r.row.x<<','<<r.row.z<<','<<r.row.dx<<','<<r.row.dz
           <<','<<r.strict<<','<<r.loose<<','<<r.looseCount<<','<<r.flat<<','<<r.atY<<",\""<<tp<<"\",APPROX_TERRAIN_ONLY\n";
        if(++written>=top)break;
    }
    std::cout<<"Input rows "<<seen<<", qualifying "<<qualified<<", rejected/malformed "<<skipped<<", selected "<<written<<" -> "<<output<<"\n";
    if(!candidates.empty())std::cout<<"Best approximate lenient Y-consistent run="<<candidates[0].loose
       <<", strict="<<candidates[0].strict<<", dy0="<<candidates[0].flat<<" (not exact terrain or block verification)\n";
    return 0;
 }catch(const std::exception& e){std::cerr<<"ERROR: "<<e.what()<<"\n";return 2;}
}