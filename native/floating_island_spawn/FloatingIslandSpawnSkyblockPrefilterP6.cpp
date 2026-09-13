#define main floating_island_spawn_visual_embedded_main
#include "FloatingIslandSpawnVisualRankP6.cpp"
#undef main

#include <deque>
#include <functional>
#include <unordered_map>
#include <utility>

namespace floating_island_spawn_skyblock_p6 {
namespace v = floating_island_spawn_visual_p6;
using namespace highest_pillar_spawn_p1;

static constexpr std::uint64_t JR_MASK = (1ULL << 48) - 1ULL;

class JRandom {
public:
    explicit JRandom(std::int64_t seed = 0) { setSeed(seed); }
    void setSeed(std::int64_t seed) { state_ = (static_cast<std::uint64_t>(seed) ^ 0x5DEECE66DULL) & JR_MASK; }
    std::uint32_t nextBits(int bits) {
        state_ = (state_ * 0x5DEECE66DULL + 0xBULL) & JR_MASK;
        return static_cast<std::uint32_t>(state_ >> (48 - bits));
    }
    int nextInt(int bound) {
        if (bound <= 0) throw std::runtime_error("Java Random bound must be positive");
        if ((bound & -bound) == bound) return static_cast<int>((static_cast<std::int64_t>(bound) * nextBits(31)) >> 31);
        for (;;) {
            const std::int32_t bits = static_cast<std::int32_t>(nextBits(31));
            const std::int32_t val = bits % bound;
            const std::int32_t test = static_cast<std::int32_t>(static_cast<std::uint32_t>(bits) - static_cast<std::uint32_t>(val) + static_cast<std::uint32_t>(bound - 1));
            if (test >= 0) return val;
        }
    }
    std::int64_t nextLong() {
        const std::int32_t hi = static_cast<std::int32_t>(nextBits(32));
        const std::int32_t lo = static_cast<std::int32_t>(nextBits(32));
        const std::uint64_t u = (static_cast<std::uint64_t>(static_cast<std::int64_t>(hi)) << 32)
                              + static_cast<std::uint64_t>(static_cast<std::int64_t>(lo));
        std::int64_t out; std::memcpy(&out, &u, sizeof(out)); return out;
    }
    double nextDouble() {
        const std::uint64_t a = nextBits(26), b = nextBits(27);
        return static_cast<double>((a << 27) + b) / 9007199254740992.0;
    }
private:
    std::uint64_t state_ = 0;
};

static std::int64_t wrapAddMulXor(std::int64_t cx, std::int64_t a, std::int64_t cz, std::int64_t b, std::int64_t seed) {
    const std::uint64_t u = static_cast<std::uint64_t>(cx) * static_cast<std::uint64_t>(a)
                          + static_cast<std::uint64_t>(cz) * static_cast<std::uint64_t>(b);
    const std::uint64_t x = u ^ static_cast<std::uint64_t>(seed);
    std::int64_t out; std::memcpy(&out, &x, sizeof(out)); return out;
}
static std::int64_t oddJava(std::int64_t x) { return (x / 2LL) * 2LL + 1LL; }

// Biome lookup exactly mirrors Beta 1.7.3 BiomeGenBase.getBiome on the 64x64 lookup table.
enum class BiomeKind { Rainforest, Swampland, SeasonalForest, Forest, Savanna, Shrubland, Taiga, Desert, Plains, Tundra };
static BiomeKind biomeFromClimate(double temperature, double rainfall) {
    int ti = static_cast<int>(temperature * 63.0), ri = static_cast<int>(rainfall * 63.0);
    ti = std::max(0, std::min(63, ti)); ri = std::max(0, std::min(63, ri));
    const float t = static_cast<float>(ti) / 63.0f;
    float wet = static_cast<float>(ri) / 63.0f;
    wet *= t;
    if (t < 0.1f) return BiomeKind::Tundra;
    if (wet < 0.2f) {
        if (t < 0.5f) return BiomeKind::Tundra;
        return t < 0.95f ? BiomeKind::Savanna : BiomeKind::Desert;
    }
    if (wet > 0.5f && t < 0.7f) return BiomeKind::Swampland;
    if (t < 0.5f) return BiomeKind::Taiga;
    if (t < 0.97f) return wet < 0.35f ? BiomeKind::Shrubland : BiomeKind::Forest;
    if (wet < 0.45f) return BiomeKind::Plains;
    return wet < 0.9f ? BiomeKind::SeasonalForest : BiomeKind::Rainforest;
}
static const char* biomeName(BiomeKind b) {
    switch (b) {
        case BiomeKind::Rainforest: return "rainforest"; case BiomeKind::Swampland: return "swampland";
        case BiomeKind::SeasonalForest: return "seasonal_forest"; case BiomeKind::Forest: return "forest";
        case BiomeKind::Savanna: return "savanna"; case BiomeKind::Shrubland: return "shrubland";
        case BiomeKind::Taiga: return "taiga"; case BiomeKind::Desert: return "desert";
        case BiomeKind::Plains: return "plains"; case BiomeKind::Tundra: return "tundra";
    }
    return "unknown";
}
static double expectedTreeAttempts(BiomeKind b) {
    // Ranking proxy, not a claim that this many trees actually generate. Forest/rainforest/taiga
    // get var13+5, seasonal forest var13+2, neutral biomes only the 10% bonus tree,
    // and desert/tundra/plains subtract 20 and therefore generate none.
    switch (b) {
        case BiomeKind::Forest: case BiomeKind::Rainforest: case BiomeKind::Taiga: return 7.0;
        case BiomeKind::SeasonalForest: return 4.0;
        case BiomeKind::Swampland: case BiomeKind::Savanna: case BiomeKind::Shrubland: return 0.1;
        case BiomeKind::Desert: case BiomeKind::Plains: case BiomeKind::Tundra: return 0.0;
    }
    return 0.0;
}

struct Component {
    int minX=0,maxX=0,minY=0,maxY=0,minZ=0,maxZ=0,sx=0,sy=0,sz=0;
    std::vector<unsigned char> member;
    std::vector<int> topY;
    int blocks = 0;
    bool ok = false;
    int idx(int x,int y,int z) const { return ((y-minY)*sz + (z-minZ))*sx + (x-minX); }
    int col(int x,int z) const { return (z-minZ)*sx + (x-minX); }
    bool contains(int x,int y,int z) const {
        if (x<minX||x>maxX||y<minY||y>maxY||z<minZ||z>maxZ) return false;
        return member[static_cast<std::size_t>(idx(x,y,z))] != 0;
    }
};

static Component traceComponent(const v::Row& r, const v::TileView& view) {
    Component c;
    c.minX=r.minX; c.maxX=r.maxX; c.minY=r.minY; c.maxY=r.maxY; c.minZ=r.minZ; c.maxZ=r.maxZ;
    c.sx=c.maxX-c.minX+1; c.sy=c.maxY-c.minY+1; c.sz=c.maxZ-c.minZ+1;
    if (c.sx<=0||c.sy<=0||c.sz<=0 || c.sx>320 || c.sz>320 || c.sy>128) return c;
    const std::size_t volume=static_cast<std::size_t>(c.sx)*c.sy*c.sz;
    c.member.assign(volume,0); c.topY.assign(static_cast<std::size_t>(c.sx)*c.sz,-1);
    if (0<c.minX||0>c.maxX||0<c.minZ||0>c.maxZ||r.supportY<c.minY||r.supportY>c.maxY||!view.solid(0,r.supportY,0)) return c;
    std::deque<std::array<int,3>> q; q.push_back({0,r.supportY,0}); c.member[static_cast<std::size_t>(c.idx(0,r.supportY,0))]=1;
    static constexpr int D[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    while(!q.empty()) {
        auto p=q.front(); q.pop_front(); ++c.blocks;
        c.topY[static_cast<std::size_t>(c.col(p[0],p[2]))]=std::max(c.topY[static_cast<std::size_t>(c.col(p[0],p[2]))],p[1]);
        for(const auto& d:D) {
            const int x=p[0]+d[0],y=p[1]+d[1],z=p[2]+d[2];
            if(x<c.minX||x>c.maxX||y<c.minY||y>c.maxY||z<c.minZ||z>c.maxZ) continue;
            const int id=c.idx(x,y,z); if(c.member[static_cast<std::size_t>(id)]) continue;
            if(!view.solid(x,y,z)) continue;
            c.member[static_cast<std::size_t>(id)]=1; q.push_back({x,y,z});
        }
    }
    c.ok = c.blocks > 0;
    return c;
}

static int highestSolid(const v::TileView& view,int x,int z) {
    for(int y=127;y>=0;--y) if(view.solid(x,y,z)) return y;
    return -1;
}
static bool minimalOakClear(const v::TileView& view,int x,int groundY,int z) {
    const int base=groundY+1, h=4;
    if(base<1 || base+h+1>128) return false;
    for(int y=base;y<=base+1+h;++y) {
        int rad=1;
        if(y==base) rad=0;
        if(y>=base+1+h-2) rad=2;
        for(int xx=x-rad;xx<=x+rad;++xx) for(int zz=z-rad;zz<=z+rad;++zz) if(view.solid(xx,y,zz)) return false;
    }
    return true;
}

static double waterYProb(int y) {
    if(y<0||y>126) return 0.0;
    double p=0.0;
    for(int a=8;a<=127;++a) if(y<a) p += (1.0/120.0)*(1.0/static_cast<double>(a));
    return p;
}
static double lavaYProb(int y) {
    if(y<0||y>125) return 0.0;
    double p=0.0;
    for(int a=8;a<=119;++a) {
        const double pa=1.0/112.0;
        for(int b=8;b<=a+7;++b) if(y<b) p += pa*(1.0/static_cast<double>(a))*(1.0/static_cast<double>(b));
    }
    return p;
}
static int populationCoverCount(int x,int z) {
    int n=0;
    const int cx0=v::floorDiv16(x-23), cx1=v::floorDiv16(x-8);
    const int cz0=v::floorDiv16(z-23), cz1=v::floorDiv16(z-8);
    for(int cx=cx0;cx<=cx1;++cx) for(int cz=cz0;cz<=cz1;++cz) {
        if(x>=cx*16+8&&x<=cx*16+23&&z>=cz*16+8&&z<=cz*16+23) ++n;
    }
    return n;
}

struct LakeProbe { bool attempted=false, likelyGenerates=false, touchesComponent=false; int x=0,y=0,z=0; };

static void buildLakeMask(JRandom& rng, std::array<unsigned char,2048>& mask) {
    mask.fill(0); const int count=rng.nextInt(4)+4;
    for(int i=0;i<count;++i) {
        const double dx=rng.nextDouble()*6.0+3.0, dy=rng.nextDouble()*4.0+2.0, dz=rng.nextDouble()*6.0+3.0;
        const double cx=rng.nextDouble()*(16.0-dx-2.0)+1.0+dx/2.0;
        const double cy=rng.nextDouble()*(8.0-dy-4.0)+2.0+dy/2.0;
        const double cz=rng.nextDouble()*(16.0-dz-2.0)+1.0+dz/2.0;
        for(int x=1;x<15;++x) for(int z=1;z<15;++z) for(int y=1;y<7;++y) {
            const double ax=(x-cx)/(dx/2.0), ay=(y-cy)/(dy/2.0), az=(z-cz)/(dz/2.0);
            if(ax*ax+ay*ay+az*az<1.0) mask[static_cast<std::size_t>((x*16+z)*8+y)]=1;
        }
    }
}
static bool lakeNeighborMask(const std::array<unsigned char,2048>& m,int x,int z,int y) {
    auto at=[&](int a,int b,int c){return m[static_cast<std::size_t>((a*16+b)*8+c)]!=0;};
    return !at(x,z,y) && ((x<15&&at(x+1,z,y))||(x>0&&at(x-1,z,y))||(z<15&&at(x,z+1,y))||(z>0&&at(x,z-1,y))||(y<7&&at(x,z,y+1))||(y>0&&at(x,z,y-1)));
}
static void assessLake(const v::TileView& view,const Component& comp,int passedX,int passedY,int passedZ,bool water,const std::array<unsigned char,2048>& mask,LakeProbe& out) {
    out.attempted=true; out.x=passedX; out.y=passedY; out.z=passedZ;
    int ox=passedX-8, oz=passedZ-8, by=passedY;
    auto isSolid=[&](int x,int y,int z){return view.solid(x,y,z);};
    auto isLiquid=[&](int x,int y,int z){return y>=0&&y<64&&!isSolid(x,y,z);};
    auto isAir=[&](int x,int y,int z){return y>=64&&!isSolid(x,y,z);};
    while(by>0&&isAir(ox,by,oz)) --by;
    by-=4; if(by<0) return;
    bool valid=true, touches=false;
    auto at=[&](int a,int b,int c){return mask[static_cast<std::size_t>((a*16+b)*8+c)]!=0;};
    for(int x=0;x<16&&valid;++x) for(int z=0;z<16&&valid;++z) for(int y=0;y<8;++y) {
        if(!lakeNeighborMask(mask,x,z,y)) continue;
        const int wx=ox+x, wy=by+y, wz=oz+z;
        if(y>=4&&isLiquid(wx,wy,wz)) {valid=false;break;}
        if(y<4&&!isSolid(wx,wy,wz)) {
            const bool sameTarget = water && isLiquid(wx,wy,wz);
            if(!sameTarget) {valid=false;break;}
        }
    }
    if(!valid) return;
    for(int x=0;x<16;++x) for(int z=0;z<16;++z) for(int y=0;y<4;++y) if(at(x,z,y)) {
        const int wx=ox+x, wy=by+y, wz=oz+z;
        if(comp.contains(wx,wy,wz) || comp.contains(wx,wy+1,wz) || comp.contains(wx,wy-1,wz)) touches=true;
    }
    out.likelyGenerates=true; out.touchesComponent=touches;
}

struct SkyRow {
    const v::Row* r=nullptr;
    std::string biome;
    double expectedTrees=0.0, treeOpportunity=0.0, originTreeProb=0.0;
    int treeHostColumns=0, originTreeStructural=0;
    int springSlots=0, waterfallSlots=0, maxFall=0;
    double expectedWaterSpringHits=0.0, expectedLavaSpringHits=0.0;
    int waterLakeAttemptsNear=0, lavaLakeAttemptsNear=0, waterLakeLikelyOnIsland=0, lavaLakeLikelyOnIsland=0;
    double scoreTree=0.0, scoreWater=0.0, scoreLava=0.0, scoreBoth=0.0;
};

static SkyRow analyzeSky(const v::Row& r,const v::TileView& view,double temp,double rain) {
    SkyRow s; s.r=&r;
    const Component comp=traceComponent(r,view); if(!comp.ok) return s;
    const BiomeKind biome=biomeFromClimate(temp,rain); s.biome=biomeName(biome); s.expectedTrees=expectedTreeAttempts(biome);
    double treeCoverage=0.0;
    for(int z=comp.minZ;z<=comp.maxZ;++z) for(int x=comp.minX;x<=comp.maxX;++x) {
        const int ty=comp.topY[static_cast<std::size_t>(comp.col(x,z))]; if(ty<0) continue;
        if(ty>65 && highestSolid(view,x,z)==ty && minimalOakClear(view,x,ty,z)) {
            ++s.treeHostColumns; treeCoverage += static_cast<double>(populationCoverCount(x,z));
        }
    }
    s.treeOpportunity = s.expectedTrees * treeCoverage / 256.0;
    const int originTop=(0>=comp.minX&&0<=comp.maxX&&0>=comp.minZ&&0<=comp.maxZ)?comp.topY[static_cast<std::size_t>(comp.col(0,0))]:-1;
    if(originTop>65 && originTop==r.supportY && highestSolid(view,0,0)==originTop && minimalOakClear(view,0,originTop,0) && s.expectedTrees>0.0) {
        s.originTreeStructural=1;
        s.originTreeProb=1.0-std::exp(-s.expectedTrees/256.0);
    }

    for(int z=comp.minZ;z<=comp.maxZ;++z) for(int x=comp.minX;x<=comp.maxX;++x) {
        const int top=comp.topY[static_cast<std::size_t>(comp.col(x,z))]; if(top<0) continue;
        for(int y=comp.minY+1;y<=top-3;++y) {
            if(!comp.contains(x,y,z) || !view.solid(x,y-1,z) || !view.solid(x,y+1,z)) continue;
            int solidSides=0, airDx=0, airDz=0;
            const int DX[4]={-1,1,0,0}, DZ[4]={0,0,-1,1};
            for(int k=0;k<4;++k) {
                if(view.solid(x+DX[k],y,z+DZ[k])) ++solidSides; else {airDx=DX[k];airDz=DZ[k];}
            }
            if(solidSides!=3) continue;
            ++s.springSlots;
            int drop=0; for(int yy=y;yy>=1&&drop<80;--yy) { if(view.solid(x+airDx,yy,z+airDz)) break; ++drop; }
            if(drop>=4) {++s.waterfallSlots; s.maxFall=std::max(s.maxFall,drop);}
            const int cover=populationCoverCount(x,y); // typo-resistant overload not available; corrected below
            (void)cover;
        }
    }
    // Re-scan slots only for weighted expected spawn counts; kept separate for clarity.
    for(int z=comp.minZ;z<=comp.maxZ;++z) for(int x=comp.minX;x<=comp.maxX;++x) {
        const int top=comp.topY[static_cast<std::size_t>(comp.col(x,z))]; if(top<0) continue;
        for(int y=comp.minY+1;y<=top-3;++y) {
            if(!comp.contains(x,y,z)||!view.solid(x,y-1,z)||!view.solid(x,y+1,z)) continue;
            int solidSides=0; const int DX[4]={-1,1,0,0},DZ[4]={0,0,-1,1};
            for(int k=0;k<4;++k) if(view.solid(x+DX[k],y,z+DZ[k])) ++solidSides;
            if(solidSides!=3) continue;
            const int cover=populationCoverCount(x,z);
            s.expectedWaterSpringHits += 50.0*waterYProb(y)*static_cast<double>(cover)/256.0;
            s.expectedLavaSpringHits += 20.0*lavaYProb(y)*static_cast<double>(cover)/256.0;
        }
    }

    // Early lake attempt RNG is exact through the water and lava attempt coordinates.
    // Lake success uses the uncarved terrain approximation here, so files are explicitly candidates.
    const int pcx0=v::floorDiv16(comp.minX-23), pcx1=v::floorDiv16(comp.maxX-8);
    const int pcz0=v::floorDiv16(comp.minZ-23), pcz1=v::floorDiv16(comp.maxZ-8);
    for(int pcx=pcx0;pcx<=pcx1;++pcx) for(int pcz=pcz0;pcz<=pcz1;++pcz) {
        JRandom rng(r.seed); const std::int64_t a=oddJava(rng.nextLong()), b=oddJava(rng.nextLong());
        rng.setSeed(wrapAddMulXor(pcx,a,pcz,b,r.seed));
        if(rng.nextInt(4)==0) {
            const int x=pcx*16+rng.nextInt(16)+8, y=rng.nextInt(128), z=pcz*16+rng.nextInt(16)+8;
            std::array<unsigned char,2048> mask; buildLakeMask(rng,mask);
            LakeProbe p; assessLake(view,comp,x,y,z,true,mask,p);
            if(x>=comp.minX-16&&x<=comp.maxX+16&&z>=comp.minZ-16&&z<=comp.maxZ+16) ++s.waterLakeAttemptsNear;
            if(p.likelyGenerates&&p.touchesComponent) ++s.waterLakeLikelyOnIsland;
        }
        if(rng.nextInt(8)==0) {
            const int x=pcx*16+rng.nextInt(16)+8;
            const int y=rng.nextInt(rng.nextInt(120)+8);
            const int z=pcz*16+rng.nextInt(16)+8;
            const bool doLake=(y<64||rng.nextInt(10)==0);
            if(doLake) {
                std::array<unsigned char,2048> mask; buildLakeMask(rng,mask);
                LakeProbe p; assessLake(view,comp,x,y,z,false,mask,p);
                if(x>=comp.minX-16&&x<=comp.maxX+16&&z>=comp.minZ-16&&z<=comp.maxZ+16) ++s.lavaLakeAttemptsNear;
                if(p.likelyGenerates&&p.touchesComponent) ++s.lavaLakeLikelyOnIsland;
            }
        }
    }

    const double tree=std::max(0.0,s.treeOpportunity);
    const double water=s.expectedWaterSpringHits+2.0*s.waterLakeLikelyOnIsland;
    const double lava=s.expectedLavaSpringHits+3.0*s.lavaLakeLikelyOnIsland;
    s.scoreTree=1000.0*tree + 150.0*s.originTreeStructural + 2.0*s.treeHostColumns;
    s.scoreWater=s.scoreTree + 5000.0*water + 30.0*s.waterfallSlots + 2.0*s.maxFall;
    s.scoreLava=s.scoreTree + 12000.0*lava + 40.0*s.waterfallSlots + 2.0*s.maxFall;
    s.scoreBoth=s.scoreTree + 6000.0*water + 14000.0*lava + 50.0*s.waterfallSlots + 3.0*s.maxFall;
    return s;
}

static void header(std::ofstream& f) {
    f<<"rank,seed,sequence_index,biome,component_blocks,footprint,span_x,span_z,min_y,max_y,feet_y,tree_host_columns,tree_opportunity,origin_tree_structural,origin_tree_probability,spring_slots,waterfall_slots,max_fall,expected_water_spring_hits,expected_lava_spring_hits,water_lake_attempts_near,lava_lake_attempts_near,water_lake_likely_on_island,lava_lake_likely_on_island,score_tree,score_water,score_lava,score_both\n";
}
static void rowOut(std::ofstream& f,const SkyRow& s,int rank) {
    const auto&r=*s.r;
    f<<rank<<','<<r.seed<<','<<r.sequenceIndex<<','<<s.biome<<','<<r.blocks<<','<<r.footprint<<','<<r.spanX<<','<<r.spanZ<<','<<r.minY<<','<<r.maxY<<','<<r.playerFeetY<<','
     <<s.treeHostColumns<<','<<std::fixed<<std::setprecision(6)<<s.treeOpportunity<<','<<s.originTreeStructural<<','<<s.originTreeProb<<','<<s.springSlots<<','<<s.waterfallSlots<<','<<s.maxFall<<','
     <<s.expectedWaterSpringHits<<','<<s.expectedLavaSpringHits<<','<<s.waterLakeAttemptsNear<<','<<s.lavaLakeAttemptsNear<<','<<s.waterLakeLikelyOnIsland<<','<<s.lavaLakeLikelyOnIsland<<','
     <<std::setprecision(3)<<s.scoreTree<<','<<s.scoreWater<<','<<s.scoreLava<<','<<s.scoreBoth<<'\n';
}
template<class Score,class Filter> static void writeTop(const std::filesystem::path&p,const std::vector<SkyRow>&rows,int top,Score score,Filter filter) {
    std::vector<const SkyRow*> a; for(const auto&r:rows) if(r.r&&filter(r)) a.push_back(&r);
    std::sort(a.begin(),a.end(),[&](const SkyRow*x,const SkyRow*y){const double sx=score(*x),sy=score(*y);if(sx!=sy)return sx>sy;return x->r->sequenceIndex<y->r->sequenceIndex;});
    if(static_cast<int>(a.size())>top)a.resize(static_cast<std::size_t>(top));
    std::ofstream f(p,std::ios::trunc); if(!f)throw std::runtime_error("cannot write "+p.string()); header(f); for(std::size_t i=0;i<a.size();++i)rowOut(f,*a[i],static_cast<int>(i+1));
}

struct Config {std::filesystem::path input,output;int chunkRadius=4,batch=128,terrainThreads=64,top=500;};
static Config parse(int argc,char**argv){Config c;for(int i=1;i<argc;++i){std::string a=argv[i];auto val=[&](){if(++i>=argc)throw std::invalid_argument("missing value");return std::string(argv[i]);};if(a=="--input")c.input=val();else if(a=="--output")c.output=val();else if(a=="--chunk-radius")c.chunkRadius=std::stoi(val());else if(a=="--batch")c.batch=std::stoi(val());else if(a=="--terrain-threads")c.terrainThreads=std::stoi(val());else if(a=="--top")c.top=std::stoi(val());else throw std::invalid_argument("unknown arg: "+a);}if(c.input.empty()||c.output.empty())throw std::invalid_argument("--input and --output required");if(c.chunkRadius<3||c.chunkRadius>8)throw std::invalid_argument("chunk radius 3..8");return c;}

static int run(const Config&c){
    std::filesystem::create_directories(c.output); auto baseRows=v::readRows(c.input); std::vector<SkyRow> out; out.reserve(baseRows.size());
    printDevice(); std::cout<<"P6 SkyBlock prefilter rows="<<baseRows.size()<<" chunkR="<<c.chunkRadius<<'\n';
    DeviceBuffers b=allocateBuffers(c.batch); const int side=c.chunkRadius*2+1,tileCount=side*side;
    try{
        for(std::size_t base=0;base<baseRows.size();base+=static_cast<std::size_t>(c.batch)){
            const int n=static_cast<int>(std::min<std::size_t>(c.batch,baseRows.size()-base));
            std::vector<std::int64_t> seeds(static_cast<std::size_t>(n));for(int i=0;i<n;++i)seeds[static_cast<std::size_t>(i)]=baseRows[base+static_cast<std::size_t>(i)].seed;
            checkHip(hipMemcpy(b.seeds,seeds.data(),static_cast<std::size_t>(n)*sizeof(std::int64_t),hipMemcpyHostToDevice),"copy skyblock seeds");
            std::vector<double> tiles(static_cast<std::size_t>(tileCount)*n*coarsecore::CELLS),temp(static_cast<std::size_t>(n)),rain(static_cast<std::size_t>(n));
            const int originTile=c.chunkRadius*side+c.chunkRadius;
            v::launchChunk(b,n,c.terrainThreads,0,0);
            checkHip(hipMemcpy(tiles.data()+static_cast<std::size_t>(originTile)*n*coarsecore::CELLS,b.noise1,static_cast<std::size_t>(n)*coarsecore::CELLS*sizeof(double),hipMemcpyDeviceToHost),"copy skyblock origin density");
            checkHip(hipMemcpy(temp.data(),b.originTemperature,static_cast<std::size_t>(n)*sizeof(double),hipMemcpyDeviceToHost),"copy skyblock temp");
            checkHip(hipMemcpy(rain.data(),b.originRainfall,static_cast<std::size_t>(n)*sizeof(double),hipMemcpyDeviceToHost),"copy skyblock rain");
            for(int cz=-c.chunkRadius;cz<=c.chunkRadius;++cz)for(int cx=-c.chunkRadius;cx<=c.chunkRadius;++cx){if(cx==0&&cz==0)continue;const int tile=(cz+c.chunkRadius)*side+(cx+c.chunkRadius);v::launchChunk(b,n,c.terrainThreads,cx,cz);checkHip(hipMemcpy(tiles.data()+static_cast<std::size_t>(tile)*n*coarsecore::CELLS,b.noise1,static_cast<std::size_t>(n)*coarsecore::CELLS*sizeof(double),hipMemcpyDeviceToHost),"copy skyblock tile");}
            for(int i=0;i<n;++i){v::TileView view(tiles,n,i,c.chunkRadius);out.push_back(analyzeSky(baseRows[base+static_cast<std::size_t>(i)],view,temp[static_cast<std::size_t>(i)],rain[static_cast<std::size_t>(i)]));}
            std::cout<<"SkyBlock analyzed "<<std::min(base+static_cast<std::size_t>(n),baseRows.size())<<'/'<<baseRows.size()<<'\n';
        }
    }catch(...){freeBuffers(b);throw;}freeBuffers(b);
    auto all=[](const SkyRow&s){return s.treeHostColumns>0;};
    writeTop(c.output/"top_tree_candidates.csv",out,c.top,[](const SkyRow&s){return s.scoreTree;},all);
    writeTop(c.output/"top_tree_at_origin_candidates.csv",out,c.top,[](const SkyRow&s){return s.originTreeProb*100000.0+s.scoreTree;},[](const SkyRow&s){return s.originTreeStructural!=0;});
    writeTop(c.output/"top_tree_waterfall_candidates.csv",out,c.top,[](const SkyRow&s){return s.scoreWater;},[](const SkyRow&s){return s.treeHostColumns>0&&s.waterfallSlots>0&&s.expectedWaterSpringHits>0;});
    writeTop(c.output/"top_tree_lavafall_candidates.csv",out,c.top,[](const SkyRow&s){return s.scoreLava;},[](const SkyRow&s){return s.treeHostColumns>0&&s.waterfallSlots>0&&s.expectedLavaSpringHits>0;});
    writeTop(c.output/"top_tree_both_fluids_candidates.csv",out,c.top,[](const SkyRow&s){return s.scoreBoth;},[](const SkyRow&s){return s.treeHostColumns>0&&s.waterfallSlots>1&&s.expectedWaterSpringHits>0&&s.expectedLavaSpringHits>0;});
    writeTop(c.output/"top_tree_lake_candidates.csv",out,c.top,[](const SkyRow&s){return s.scoreBoth+5000.0*(s.waterLakeLikelyOnIsland+s.lavaLakeLikelyOnIsland);},[](const SkyRow&s){return s.treeHostColumns>0&&(s.waterLakeLikelyOnIsland>0||s.lavaLakeLikelyOnIsland>0);});
    writeTop(c.output/"all_skyblock_prefilter_ranked.csv",out,static_cast<int>(std::min<std::size_t>(10000,out.size())),[](const SkyRow&s){return s.scoreBoth;},[](const SkyRow&){return true;});
    std::cout<<"P6 SkyBlock prefilter done output="<<c.output.string()<<'\n';return 0;
}
}

int main(int argc,char**argv){try{return floating_island_spawn_skyblock_p6::run(floating_island_spawn_skyblock_p6::parse(argc,argv));}catch(const std::exception&e){std::cerr<<"P6 SkyBlock prefilter ERROR: "<<e.what()<<'\n';return 1;}}
