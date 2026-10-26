// Deadbush15 V7: Minecraft 26.3 NORMAL-world Badlands biome gate.
// Uses pinned xpple/cubiomes. Biome-compatible is NOT terrain-verified.
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "generator.h"

using std::string;
using std::vector;
static vector<string> split(const string &line) {
    vector<string> a; string f; bool quoted=false;
    for(size_t i=0;i<line.size();++i) {
        char c=line[i];
        if(c=='"') {if(quoted&&i+1<line.size()&&line[i+1]=='"'){f+='"';++i;}else quoted=!quoted;}
        else if(c==','&&!quoted){a.push_back(f);f.clear();}
        else f+=c;
    }
    if(quoted)throw std::runtime_error("Unterminated CSV quote");
    a.push_back(f);return a;
}
static string escape(const string &s) {
    if(s.find_first_of(",\"\r\n")==string::npos)return s;
    string x="\"";for(char c:s){if(c=='"')x+='"';x+=c;}return x+'"';
}
static string join(const vector<string>&v) {
    string x;for(size_t i=0;i<v.size();++i){if(i)x+=',';x+=escape(v[i]);}return x;
}
static int idx(const vector<string>& v,const string& key) {
    auto i=std::find(v.begin(),v.end(),key);
    if(i==v.end())throw std::runtime_error("Missing CSV column: "+key);
    return int(i-v.begin());
}
static int64_t dec(const string &s) {
    size_t j=0;auto v=std::stoll(s,&j);
    if(j!=s.size())throw std::runtime_error("Invalid integer: "+s);return v;
}
static bool bad(int v){return v==badlands||v==wooded_badlands||v==eroded_badlands;}
static int div4(int x){return x>=0?x/4:-int((-(int64_t)x+3)/4);}
static int biome(Generator &g,int x,int y,int z){
    return getBiomeAt(&g,4,div4(x),div4(y),div4(z));
}
static constexpr std::array<int,9> heights={64,72,80,96,112,128,144,160,192};
struct Gate{bool pass=false;int y=-999;int cells=0;};
static Gate check(Generator &g,int x,int z,int dx,int dz,int n){
    Gate out;
    for(int y:heights){
        int a[3]={0,n/2,n-1};bool eligible=true;
        for(int i:a)if(!bad(biome(g,x+i*dx,y,z+i*dz))){eligible=false;break;}
        if(!eligible)continue;
        int cells=0;
        for(int i=0;i<n;++i)cells+=bad(biome(g,x+i*dx,y,z+i*dz));
        if(cells==n)return {true,y,n};
        out.cells=std::max(out.cells,cells);
    }
    return out;
}
int main(int argc,char**argv){
 try{
    string input,output,rejects;bool selftest=false,probe=false;
    int64_t s0=0;int x0=0,z0=0;
    for(int i=1;i<argc;++i){
        string arg=argv[i];
        auto next=[&]()->string{if(++i>=argc)throw std::runtime_error("Missing "+arg);return argv[i];};
        if(arg=="--input")input=next();else if(arg=="--output")output=next();
        else if(arg=="--rejects")rejects=next();else if(arg=="--self-test")selftest=true;
        else if(arg=="--probe"){s0=dec(next());x0=int(dec(next()));z0=int(dec(next()));probe=true;}
        else if(arg=="--help"){std::cout<<"--self-test | --probe seed x z | --input GPU.csv --output badlands.csv --rejects excluded.csv\n";return 0;}
        else throw std::runtime_error("Unknown arg "+arg);
    }
    Generator g;setupGenerator(&g,MC_26_3,0);
    if(selftest){
        struct Ref{int64_t seed;int x,z;};
        const Ref refs[]={{-6825794747614541757LL,16530130,-25619539},{923652124993224547LL,5653714,10797037}};
        for(const Ref r:refs){
            applySeed(&g,DIM_OVERWORLD,uint64_t(r.seed));bool ok=false;
            for(int y:heights)ok |= bad(biome(g,r.x,y,r.z));
            std::cout<<"Published seed "<<r.seed<<": "<<(ok?"BADLANDS":"NOT_BADLANDS")<<"\n";
            if(!ok){std::cerr<<"CALIBRATION FAILED. Do not scan until corrected.\n";return 2;}
        }
        std::cout<<"PASS: Both normal-world references have Badlands at sampled heights.\n";return 0;
    }
    if(probe){
        applySeed(&g,DIM_OVERWORLD,uint64_t(s0));
        for(int y:heights){int b=biome(g,x0,y,z0);std::cout<<"y="<<y<<" biome="<<b<<" badlands="<<bad(b)<<"\n";}
        return 0;
    }
    if(input.empty()||output.empty()||rejects.empty())throw std::runtime_error("Specify --input, --output and --rejects");
    if(input==output||input==rejects||output==rejects)throw std::runtime_error("Input/outputs must be different paths");
    std::ifstream in(input);std::ofstream yes(output),no(rejects);
    if(!in||!yes||!no)throw std::runtime_error("Cannot open input or output files");
    string line;if(!std::getline(in,line))throw std::runtime_error("Empty input");
    auto cols=split(line);
    if(!cols.empty()&&cols[0].substr(0,3)=="\xef\xbb\xbf")cols[0]=cols[0].substr(3);
    int cs=idx(cols,"seed_signed"),cx=idx(cols,"start_x"),cz=idx(cols,"start_z"),cdx=idx(cols,"dx"),cdz=idx(cols,"dz"),cn=idx(cols,"run");
    auto headers=cols;headers.insert(headers.end(),{"biome_gate","sample_y","badlands_cells_at_sample","biome_algorithm"});
    yes<<join(headers)<<"\n";no<<join(headers)<<"\n";
    uint64_t count=0,passed=0,invalid=0,last_seed=0;bool initialized=false;
    while(std::getline(in,line)){
        if(line.empty())continue;
        try {
            auto row=split(line);
            if(row.size()!=cols.size())throw std::runtime_error("Wrong column count");
            uint64_t seed=uint64_t(dec(row[cs]));
            int x=int(dec(row[cx])),z=int(dec(row[cz])),dx=int(dec(row[cdx])),dz=int(dec(row[cdz])),n=int(dec(row[cn]));
            if(n<1||n>64||!((dx==1&&dz==0)||(dx==0&&dz==1)))throw std::runtime_error("Invalid row geometry");
            if(!initialized||seed!=last_seed){applySeed(&g,DIM_OVERWORLD,seed);last_seed=seed;initialized=true;}
            auto result=check(g,x,z,dx,dz,n);++count;passed+=result.pass;
            row.push_back(result.pass?"BIOME_COMPATIBLE":"NO_BADLANDS_AT_SAMPLED_HEIGHTS");
            row.push_back(result.pass?std::to_string(result.y):"");
            row.push_back(std::to_string(result.cells));
            row.push_back("xpple_cubiomes_MC_26_3_quart");
            (result.pass?yes:no)<<join(row)<<"\n";
        }catch(const std::exception&e){++invalid;if(invalid<6)std::cerr<<"Bad candidate: "<<e.what()<<"\n";}
        if(count&&count%10000==0)std::cerr<<"Checked "<<count<<"; compatible "<<passed<<"\r";
    }
    std::cout<<"\nProcessed "<<count<<" GPU rows; "<<passed<<" biome-compatible, "<<count-passed<<" rejected samples, "<<invalid<<" invalid lines.\n"
             <<"Rejected rows preserved in "<<rejects<<". All results are UNVERIFIED terrain patterns.\n";
    return 0;
 }catch(const std::exception&e){std::cerr<<"ERROR: "<<e.what()<<"\n";return 1;}
}