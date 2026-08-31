// VineCpuCostBench —— README「CPU 版本耗时预估」一节里那几个单次成本常量的来源。
//
// 测的都是从在跑的代码里逐行抄出来的原语：
//   PerlinNoise3D  <- Shaders/Private/VinePerlinNoise.ush（它本身是 FMath::PerlinNoise3D 的精确移植）
//   CurlNoise      <- Source/GeometryScriptExtraEditor/Private/GeometryMathUtils.cpp（= 5 次 Perlin）
//   表面投影       <- SampleVoxelNeighbors() in Shaders/Private/VVVoxel.usf（8 角三线性 + 哈希线性探测）
//   表面体素化     <- TriangleSurfaceVoxelsCS in Shaders/Private/StaticMeshPointSampler.usf
//                     （逐三角遍历 AABB 格，每格 6 平面 Sutherland-Hodgman 裁剪求面积/质心）
//   BVH 最近三角   <- 旧 VisVine 走的 FDynamicMeshAABBTree3 那条路（这里是更紧凑的数组式 BVH，见下）
//
// 不依赖 UE，独立编译即可（本文件是 UTF-8，MSVC 要带 /utf-8，否则按 936 代码页读会报 C4819）：
//   cl /nologo /O2 /Oi /fp:fast /EHsc /std:c++17 /utf-8 VineCpuCostBench.cpp
//
// 本机（i7-13700KF）参考输出，README 那一节的常量就是这个：
//   PerlinNoise3D :   65.0 ns/call
//   CurlNoise(5x) :  154.4 ns/call
//   Projection    :   72.6 /  99.4 / 124.3 / 165.0 ns/call（体素表 1.1 / 4.5 / 18.1 / 72.2 MB）
//   BVH nearest   :  959 ns（25 万三角）/ 1411 ns（100 万三角），建树 54 / 240 ms
//   Voxelize      :  547 ns/tri —— 100 万三角 @ VoxelSize=5 → 约 109 万体素，548 ms
//
// 规模按测试场景对齐：拾取三角约 100 万，SC.VoxelSize=5。三角面来自合成的起伏表面，
// 不是真实关卡几何，所以体素数是模型值不是实测值——量级参考，别当实测引用。
//
// 注意这是独立基准，比 UE Development 配置少一层容器/边界检查开销：真实实现只会更慢，不会更快。
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <vector>
#include <random>
#include <algorithm>

static const int Perm[512] = {
63,9,212,205,31,128,72,59,137,203,195,170,181,115,165,40,116,139,175,225,132,99,222,2,41,15,197,93,169,90,228,43,221,38,206,204,73,17,97,10,96,47,32,138,136,30,219,
78,224,13,193,88,134,211,7,112,176,19,106,83,75,217,85,0,98,140,229,80,118,151,117,251,103,242,81,238,172,82,110,4,227,77,243,46,12,189,34,188,200,161,68,76,171,194,
57,48,247,233,51,105,5,23,42,50,216,45,239,148,249,84,70,125,108,241,62,66,64,240,173,185,250,49,6,37,26,21,244,60,223,255,16,145,27,109,58,102,142,253,120,149,160,
124,156,79,186,135,127,14,121,22,65,54,153,91,213,174,24,252,131,192,190,202,208,35,94,231,56,95,183,163,111,147,25,67,36,92,236,71,166,1,187,100,130,143,237,178,158,
104,184,159,177,52,214,230,119,87,114,201,179,198,3,248,182,39,11,152,196,113,20,232,69,141,207,234,53,86,180,226,74,150,218,29,133,8,44,123,28,146,89,101,154,220,126,
155,122,210,168,254,162,129,33,18,209,61,191,199,157,245,55,164,167,215,246,144,107,235,
63,9,212,205,31,128,72,59,137,203,195,170,181,115,165,40,116,139,175,225,132,99,222,2,41,15,197,93,169,90,228,43,221,38,206,204,73,17,97,10,96,47,32,138,136,30,219,
78,224,13,193,88,134,211,7,112,176,19,106,83,75,217,85,0,98,140,229,80,118,151,117,251,103,242,81,238,172,82,110,4,227,77,243,46,12,189,34,188,200,161,68,76,171,194,
57,48,247,233,51,105,5,23,42,50,216,45,239,148,249,84,70,125,108,241,62,66,64,240,173,185,250,49,6,37,26,21,244,60,223,255,16,145,27,109,58,102,142,253,120,149,160,
124,156,79,186,135,127,14,121,22,65,54,153,91,213,174,24,252,131,192,190,202,208,35,94,231,56,95,183,163,111,147,25,67,36,92,236,71,166,1,187,100,130,143,237,178,158,
104,184,159,177,52,214,230,119,87,114,201,179,198,3,248,182,39,11,152,196,113,20,232,69,141,207,234,53,86,180,226,74,150,218,29,133,8,44,123,28,146,89,101,154,220,126,
155,122,210,168,254,162,129,33,18,209,61,191,199,157,245,55,164,167,215,246,144,107,235 };

static inline double Grad3(int h, double x, double y, double z){
  switch(h & 15){
  case 0: return x + z;  case 1: return x + y;  case 2: return y + z;  case 3: return -x + y;
  case 4: return -x + z; case 5: return -x - y; case 6: return -y + z; case 7: return x - y;
  case 8: return x - z;  case 9: return y - z;  case 10: return -x - z; case 11: return -y - z;
  case 12: return x + y; case 13: return -x + y; case 14: return -y + z; case 15: return -y - z; }
  return 0.0;
}
static inline double Smooth(double x){ return x*x*x*(x*(x*6.0-15.0)+10.0); }
static inline double Lerp(double a,double b,double t){ return a + (b-a)*t; }

static double PerlinNoise3D(double px,double py,double pz){
  double Xfl=std::floor(px), Yfl=std::floor(py), Zfl=std::floor(pz);
  int Xi=((int)Xfl)&255, Yi=((int)Yfl)&255, Zi=((int)Zfl)&255;
  double X=px-Xfl, Y=py-Yfl, Z=pz-Zfl;
  double Xm1=X-1.0, Ym1=Y-1.0, Zm1=Z-1.0;
  int A=Perm[Xi]+Yi, AA=Perm[A]+Zi, AB=Perm[A+1]+Zi;
  int B=Perm[Xi+1]+Yi, BA=Perm[B]+Zi, BB=Perm[B+1]+Zi;
  double U=Smooth(X), V=Smooth(Y), W=Smooth(Z);
  double r = 0.97 * Lerp(
      Lerp(Lerp(Grad3(Perm[AA],X,Y,Z),   Grad3(Perm[BA],Xm1,Y,Z),   U),
           Lerp(Grad3(Perm[AB],X,Ym1,Z), Grad3(Perm[BB],Xm1,Ym1,Z), U), V),
      Lerp(Lerp(Grad3(Perm[AA+1],X,Y,Zm1),   Grad3(Perm[BA+1],Xm1,Y,Zm1),   U),
           Lerp(Grad3(Perm[AB+1],X,Ym1,Zm1), Grad3(Perm[BB+1],Xm1,Ym1,Zm1), U), V), W);
  return r < -1.0 ? -1.0 : (r > 1.0 ? 1.0 : r);
}

struct V3 { double x,y,z; };

// UNoise::CurlNoise -> 5 PerlinNoise3D calls
static inline V3 CurlNoise(V3 p, double Strength, double Frequency){
  double h=0.001, n,n1,a,b; V3 curl{0,0,0};
  Frequency/=100.0;
  V3 np{p.x*Frequency,p.y*Frequency,p.z*Frequency};
  n  = PerlinNoise3D(np.x,np.y,np.z);
  n1 = PerlinNoise3D(np.x,np.y-h,np.z); a=(n-n1)/h;
  n1 = PerlinNoise3D(np.x,np.y,np.z-h); b=(n-n1)/h; curl.x=a-b;
  a=(n-n1)/h;
  n1 = PerlinNoise3D(np.x-h,np.y,np.z); b=(n-n1)/h; curl.y=a-b;
  a=(n-n1)/h;
  n1 = PerlinNoise3D(np.x,np.y-h,np.z); b=(n-n1)/h; curl.z=a-b;
  return V3{p.x+curl.x*Strength, p.y+curl.y*Strength, p.z+curl.z*Strength};
}

// ---- voxel hash projection (SampleVoxelNeighbors in VVVoxel.usf) ----
struct Cell { int32_t x,y,z,w; };
static inline uint32_t HashCell(int32_t x,int32_t y,int32_t z){
  return ((uint32_t)x*73856093u) ^ ((uint32_t)y*19349663u) ^ ((uint32_t)z*83492791u);
}
struct VoxelGrid {
  std::vector<Cell> Cells;
  std::vector<float> Targets;
  std::vector<float> Normals;
  std::vector<uint32_t> Slots;
  uint32_t SlotMask=0;
  int FindCell(int32_t cx,int32_t cy,int32_t cz) const {
    uint32_t slot = HashCell(cx,cy,cz) & SlotMask;
    for(;;){
      uint32_t packed = Slots[slot];
      if(packed==0u) return -1;
      uint32_t vi = packed-1u;
      const Cell& c = Cells[vi];
      if(c.x==cx && c.y==cy && c.z==cz) return (int)vi;
      slot = (slot+1u) & SlotMask;
    }
  }
};

static bool SampleVoxelNeighbors(const VoxelGrid& G, double qx,double qy,double qz,
                                 double ox,double oy,double oz, double VoxelSize,
                                 V3& OutTarget){
  double lx=(qx-ox)/VoxelSize-0.5, ly=(qy-oy)/VoxelSize-0.5, lz=(qz-oz)/VoxelSize-0.5;
  int bx=(int)std::floor(lx), by=(int)std::floor(ly), bz=(int)std::floor(lz);
  double fx=lx-bx, fy=ly-by, fz=lz-bz;
  double W[8] = {
    (1-fx)*(1-fy)*(1-fz), fx*(1-fy)*(1-fz), (1-fx)*fy*(1-fz), fx*fy*(1-fz),
    (1-fx)*(1-fy)*fz,     fx*(1-fy)*fz,     (1-fx)*fy*fz,     fx*fy*fz };
  static const int Off[8][3] = {{0,0,0},{1,0,0},{0,1,0},{1,1,0},{0,0,1},{1,0,1},{0,1,1},{1,1,1}};
  double sx=0,sy=0,sz=0,tw=0; int valid=0;
  for(int c=0;c<8;++c){
    int vi = G.FindCell(bx+Off[c][0], by+Off[c][1], bz+Off[c][2]);
    if(vi<0) continue;
    ++valid;
    if(W[c]<=1e-8) continue;
    double tx=G.Targets[vi*4+0], ty=G.Targets[vi*4+1], tz=G.Targets[vi*4+2];
    double dx=qx-tx, dy=qy-ty, dz=qz-tz;
    double d2=dx*dx+dy*dy+dz*dz;
    double w = W[c] / (d2>1e-8?d2:1e-8);
    sx+=tx*w; sy+=ty*w; sz+=tz*w; tw+=w;
  }
  if(valid<=0 || tw<=1e-8) return false;
  OutTarget = V3{sx/tw, sy/tw, sz/tw};
  return true;
}

static VoxelGrid MakeGrid(int side, unsigned seed){
  // A hollow shell of surface voxels on a side^3 lattice: surface voxels are sparse,
  // so the live set is O(side^2), not O(side^3).
  VoxelGrid G;
  for(int z=0;z<side;++z) for(int y=0;y<side;++y) for(int x=0;x<side;++x){
    double h = 0.5*side + 0.18*side*std::sin(x*0.11)*std::cos(y*0.13);
    if(std::fabs(z-h) < 1.5) G.Cells.push_back(Cell{x,y,z,0});
  }
  size_t n=G.Cells.size();
  G.Targets.resize(n*4); G.Normals.resize(n*4);
  for(size_t i=0;i<n;++i){
    G.Targets[i*4+0]=(float)(G.Cells[i].x*5.0+2.5);
    G.Targets[i*4+1]=(float)(G.Cells[i].y*5.0+2.5);
    G.Targets[i*4+2]=(float)(G.Cells[i].z*5.0+2.5);
    G.Normals[i*4+2]=1.0f;
  }
  uint32_t slots=2; while(slots < n*2) slots<<=1;
  G.Slots.assign(slots,0u); G.SlotMask=slots-1u;
  for(size_t i=0;i<n;++i){
    uint32_t s = HashCell(G.Cells[i].x,G.Cells[i].y,G.Cells[i].z) & G.SlotMask;
    while(G.Slots[s]!=0u) s=(s+1u)&G.SlotMask;
    G.Slots[s]=(uint32_t)(i+1);
  }
  return G;
}

// ---- BVH 最近三角查询（旧 VisVine 走 FDynamicMeshAABBTree3 的那条路）----
// 这里是一棵紧凑的数组式中位分割 BVH，节点 32 字节、叶子 4 个三角。它比 UE 的
// FDynamicMeshAABBTree3 更贴 cache（UE 那边节点更胖、还要过 FDynamicMesh3 的间接层，
// 且 Geometry Script 封装每次查询还要走一次 ProcessMesh），所以测出来的数是**下界**。
struct Tri { float v[9]; };

static inline float ClosestPointTriDistSq(const Tri& T, float px,float py,float pz){
  const float ax=T.v[0],ay=T.v[1],az=T.v[2];
  const float bx=T.v[3],by=T.v[4],bz=T.v[5];
  const float cx=T.v[6],cy=T.v[7],cz=T.v[8];
  float abx=bx-ax, aby=by-ay, abz=bz-az;
  float acx=cx-ax, acy=cy-ay, acz=cz-az;
  float apx=px-ax, apy=py-ay, apz=pz-az;
  float d1=abx*apx+aby*apy+abz*apz, d2=acx*apx+acy*apy+acz*apz;
  float qx,qy,qz;
  if(d1<=0 && d2<=0){ qx=ax; qy=ay; qz=az; }
  else{
    float bpx=px-bx, bpy=py-by, bpz=pz-bz;
    float d3=abx*bpx+aby*bpy+abz*bpz, d4=acx*bpx+acy*bpy+acz*bpz;
    if(d3>=0 && d4<=d3){ qx=bx; qy=by; qz=bz; }
    else{
      float vc=d1*d4-d3*d2;
      if(vc<=0 && d1>=0 && d3<=0){ float t=d1/(d1-d3); qx=ax+abx*t; qy=ay+aby*t; qz=az+abz*t; }
      else{
        float cpx=px-cx, cpy=py-cy, cpz=pz-cz;
        float d5=abx*cpx+aby*cpy+abz*cpz, d6=acx*cpx+acy*cpy+acz*cpz;
        if(d6>=0 && d5<=d6){ qx=cx; qy=cy; qz=cz; }
        else{
          float vb=d5*d2-d1*d6;
          if(vb<=0 && d2>=0 && d6<=0){ float t=d2/(d2-d6); qx=ax+acx*t; qy=ay+acy*t; qz=az+acz*t; }
          else{
            float va=d3*d6-d5*d4;
            if(va<=0 && (d4-d3)>=0 && (d5-d6)>=0){
              float t=(d4-d3)/((d4-d3)+(d5-d6));
              qx=bx+(cx-bx)*t; qy=by+(cy-by)*t; qz=bz+(cz-bz)*t;
            } else {
              float den=1.0f/(va+vb+vc), v=vb*den, w=vc*den;
              qx=ax+abx*v+acx*w; qy=ay+aby*v+acy*w; qz=az+abz*v+acz*w;
            }
          }
        }
      }
    }
  }
  float dx=px-qx, dy=py-qy, dz=pz-qz;
  return dx*dx+dy*dy+dz*dz;
}

struct Node { float bmin[3], bmax[3]; uint32_t start, count; };  // count==0 -> 内部节点，start=右孩子

struct BVH {
  std::vector<Tri> Tris;
  std::vector<uint32_t> Idx;
  std::vector<Node> Nodes;
  std::vector<float> Cent;  // 3 floats / tri

  void BuildRange(uint32_t nodeIdx, uint32_t begin, uint32_t end){
    Node& n = Nodes[nodeIdx];
    for(int k=0;k<3;++k){ n.bmin[k]= 1e30f; n.bmax[k]=-1e30f; }
    for(uint32_t i=begin;i<end;++i){
      const Tri& T=Tris[Idx[i]];
      for(int c=0;c<3;++c) for(int k=0;k<3;++k){
        float val=T.v[c*3+k];
        if(val<n.bmin[k]) n.bmin[k]=val;
        if(val>n.bmax[k]) n.bmax[k]=val;
      }
    }
    if(end-begin<=4){ n.start=begin; n.count=end-begin; return; }
    int axis=0; float ext=n.bmax[0]-n.bmin[0];
    if(n.bmax[1]-n.bmin[1]>ext){ axis=1; ext=n.bmax[1]-n.bmin[1]; }
    if(n.bmax[2]-n.bmin[2]>ext){ axis=2; }
    uint32_t mid=(begin+end)/2;
    std::nth_element(Idx.begin()+begin, Idx.begin()+mid, Idx.begin()+end,
      [&](uint32_t a, uint32_t b){ return Cent[a*3+axis] < Cent[b*3+axis]; });
    uint32_t left=(uint32_t)Nodes.size();  Nodes.push_back(Node{});
    uint32_t right=(uint32_t)Nodes.size(); Nodes.push_back(Node{});
    Nodes[nodeIdx].count=0; Nodes[nodeIdx].start=right;
    BuildRange(left, begin, mid);
    BuildRange(right, mid, end);
  }

  void Build(){
    uint32_t n=(uint32_t)Tris.size();
    Idx.resize(n); for(uint32_t i=0;i<n;++i) Idx[i]=i;
    Cent.resize(n*3);
    for(uint32_t i=0;i<n;++i) for(int k=0;k<3;++k)
      Cent[i*3+k]=(Tris[i].v[k]+Tris[i].v[3+k]+Tris[i].v[6+k])/3.0f;
    Nodes.clear(); Nodes.reserve(n/2+8); Nodes.push_back(Node{});
    BuildRange(0,0,n);
  }

  static inline float BoxDistSq(const Node& n, float px,float py,float pz){
    float dx = px<n.bmin[0] ? n.bmin[0]-px : (px>n.bmax[0] ? px-n.bmax[0] : 0.0f);
    float dy = py<n.bmin[1] ? n.bmin[1]-py : (py>n.bmax[1] ? py-n.bmax[1] : 0.0f);
    float dz = pz<n.bmin[2] ? n.bmin[2]-pz : (pz>n.bmax[2] ? pz-n.bmax[2] : 0.0f);
    return dx*dx+dy*dy+dz*dz;
  }

  int FindNearestTriangle(float px,float py,float pz, float& outDistSq) const {
    float best=3.4e38f; int bestTri=-1;
    uint32_t stack[64]; int sp=0; stack[sp++]=0;
    while(sp){
      uint32_t ni=stack[--sp];
      const Node& n=Nodes[ni];
      if(BoxDistSq(n,px,py,pz)>=best) continue;
      if(n.count>0){
        for(uint32_t i=0;i<n.count;++i){
          uint32_t t=Idx[n.start+i];
          float d=ClosestPointTriDistSq(Tris[t],px,py,pz);
          if(d<best){ best=d; bestTri=(int)t; }
        }
      } else {
        uint32_t l=ni+1, r=n.start;
        float dl=BoxDistSq(Nodes[l],px,py,pz), dr=BoxDistSq(Nodes[r],px,py,pz);
        if(dl<dr){ stack[sp++]=r; stack[sp++]=l; } else { stack[sp++]=l; stack[sp++]=r; }
      }
    }
    outDistSq=best; return bestTri;
  }
};

// 造一张 triCount 个三角的起伏表面，空间范围和上面体素网格同量级。
static BVH MakeSurfaceBVH(size_t triCount, double extent){
  int grid=(int)std::ceil(std::sqrt(triCount/2.0));
  BVH B; B.Tris.reserve((size_t)grid*grid*2);
  double step=extent/grid;
  auto H=[&](int i,int j){ return 0.5*extent + 0.10*extent*std::sin(i*0.031)*std::cos(j*0.027); };
  for(int j=0;j<grid;++j) for(int i=0;i<grid;++i){
    double x0=i*step, x1=(i+1)*step, y0=j*step, y1=(j+1)*step;
    float h00=(float)H(i,j), h10=(float)H(i+1,j), h01=(float)H(i,j+1), h11=(float)H(i+1,j+1);
    Tri a{{(float)x0,(float)y0,h00,(float)x1,(float)y0,h10,(float)x1,(float)y1,h11}};
    Tri b{{(float)x0,(float)y0,h00,(float)x1,(float)y1,h11,(float)x0,(float)y1,h01}};
    B.Tris.push_back(a); B.Tris.push_back(b);
  }
  return B;
}

// ---- 表面体素化（TriangleSurfaceVoxelsCS in StaticMeshPointSampler.usf 的 CPU 转写）----
// 逐三角遍历它体素空间的 AABB（上限 512 格），每格做一次 6 平面 Sutherland-Hodgman 裁剪，
// 求交面积和质心，再写进哈希网格。GPU 版本是这条链路里唯一按三角形数量伸缩的段。
#define CLIP_MAX_VERTS 16

static inline float AxisVal(const float* p, int a){ return p[a]; }

static uint32_t ClipPolyAxis(const float (*In)[3], uint32_t InCount, float (*Out)[3],
                             int axis, float planeValue, bool keepGreater){
  uint32_t outCount=0;
  if(InCount==0) return 0;
  const float* prev = In[InCount-1];
  float prevD = keepGreater ? AxisVal(prev,axis)-planeValue : planeValue-AxisVal(prev,axis);
  bool prevIn = prevD >= -1.0e-5f;
  for(uint32_t i=0;i<InCount;++i){
    const float* cur = In[i];
    float curD = keepGreater ? AxisVal(cur,axis)-planeValue : planeValue-AxisVal(cur,axis);
    bool curIn = curD >= -1.0e-5f;
    if(curIn != prevIn){
      float den = prevD-curD;
      float t = std::fabs(den)>1.0e-8f ? prevD/den : 0.0f;
      t = t<0?0:(t>1?1:t);
      if(outCount<CLIP_MAX_VERTS){
        for(int k=0;k<3;++k) Out[outCount][k] = prev[k] + (cur[k]-prev[k])*t;
        ++outCount;
      }
    }
    if(curIn && outCount<CLIP_MAX_VERTS){
      for(int k=0;k<3;++k) Out[outCount][k]=cur[k];
      ++outCount;
    }
    prev=cur; prevD=curD; prevIn=curIn;
  }
  return outCount;
}

static float TriVoxelAreaCentroid(const float* P0,const float* P1,const float* P2,
                                  const int* cell, const float* N, float cellSize,
                                  const float* voxOrigin, float* outCentroid){
  float boxMin[3], boxMax[3];
  for(int k=0;k<3;++k){ boxMin[k]=voxOrigin[k]+cell[k]*cellSize; boxMax[k]=boxMin[k]+cellSize; }
  float A[CLIP_MAX_VERTS][3], B[CLIP_MAX_VERTS][3];
  for(int k=0;k<3;++k){ A[0][k]=P0[k]; A[1][k]=P1[k]; A[2][k]=P2[k]; }
  uint32_t c=3;
  c=ClipPolyAxis(A,c,B,0,boxMin[0],true);  if(c<3) return 0.0f;
  c=ClipPolyAxis(B,c,A,0,boxMax[0],false); if(c<3) return 0.0f;
  c=ClipPolyAxis(A,c,B,1,boxMin[1],true);  if(c<3) return 0.0f;
  c=ClipPolyAxis(B,c,A,1,boxMax[1],false); if(c<3) return 0.0f;
  c=ClipPolyAxis(A,c,B,2,boxMin[2],true);  if(c<3) return 0.0f;
  c=ClipPolyAxis(B,c,A,2,boxMax[2],false); if(c<3) return 0.0f;
  float total=0.0f, cs[3]={0,0,0};
  for(uint32_t i=1;i+1<c;++i){
    float e1[3],e2[3],cr[3];
    for(int k=0;k<3;++k){ e1[k]=A[i][k]-A[0][k]; e2[k]=A[i+1][k]-A[0][k]; }
    cr[0]=e1[1]*e2[2]-e1[2]*e2[1]; cr[1]=e1[2]*e2[0]-e1[0]*e2[2]; cr[2]=e1[0]*e2[1]-e1[1]*e2[0];
    float area=0.5f*std::fabs(cr[0]*N[0]+cr[1]*N[1]+cr[2]*N[2]);
    if(area<=1.0e-12f) continue;
    total+=area;
    for(int k=0;k<3;++k) cs[k]+=((A[0][k]+A[i][k]+A[i+1][k])/3.0f)*area;
  }
  if(total<=1.0e-12f) return 0.0f;
  for(int k=0;k<3;++k) outCentroid[k]=cs[k]/total;
  return total;
}

// 返回写出的体素数；hash 表按容量预开，单线程所以不用 atomic。
static size_t VoxelizeTriangles(const std::vector<Tri>& Tris, float cellSize,
                                const float* voxOrigin, size_t slotCount){
  std::vector<uint32_t> slots(slotCount, 0u);
  std::vector<Cell> cells; cells.reserve(slotCount/2);
  uint32_t mask=(uint32_t)slotCount-1u;
  for(const Tri& T : Tris){
    float e1[3],e2[3],N[3];
    for(int k=0;k<3;++k){ e1[k]=T.v[3+k]-T.v[k]; e2[k]=T.v[6+k]-T.v[k]; }
    N[0]=e1[1]*e2[2]-e1[2]*e2[1]; N[1]=e1[2]*e2[0]-e1[0]*e2[2]; N[2]=e1[0]*e2[1]-e1[1]*e2[0];
    float len=std::sqrt(N[0]*N[0]+N[1]*N[1]+N[2]*N[2]);
    if(len<=1e-8f) continue;
    for(int k=0;k<3;++k) N[k]/=len;
    float tmin[3],tmax[3];
    for(int k=0;k<3;++k){
      tmin[k]=std::min(T.v[k],std::min(T.v[3+k],T.v[6+k]));
      tmax[k]=std::max(T.v[k],std::max(T.v[3+k],T.v[6+k]));
    }
    int minC[3],maxC[3],span[3]; uint32_t total=1;
    for(int k=0;k<3;++k){
      minC[k]=(int)std::floor((tmin[k]-voxOrigin[k])/cellSize);
      maxC[k]=(int)std::floor((tmax[k]-voxOrigin[k])/cellSize);
      span[k]=maxC[k]-minC[k]+1;
      if(span[k]<=0){ total=0; break; }
      total*=(uint32_t)span[k];
    }
    if(total==0) continue;
    if(total>512u) total=512u;
    uint32_t slice=(uint32_t)(span[0]*span[1]);
    for(uint32_t li=0; li<total; ++li){
      uint32_t lz=li/slice, rem=li-lz*slice, ly=rem/(uint32_t)span[0], lx=rem-ly*(uint32_t)span[0];
      int cell[3]={ minC[0]+(int)lx, minC[1]+(int)ly, minC[2]+(int)lz };
      float centroid[3];
      float area=TriVoxelAreaCentroid(T.v,T.v+3,T.v+6,cell,N,cellSize,voxOrigin,centroid);
      if(area/(cellSize*cellSize) <= 1.0e-8f) continue;
      uint32_t s=HashCell(cell[0],cell[1],cell[2]) & mask;
      for(;;){
        uint32_t packed=slots[s];
        if(packed==0u){ cells.push_back(Cell{cell[0],cell[1],cell[2],0}); slots[s]=(uint32_t)cells.size(); break; }
        const Cell& ec=cells[packed-1u];
        if(ec.x==cell[0]&&ec.y==cell[1]&&ec.z==cell[2]) break;
        s=(s+1u)&mask;
      }
    }
  }
  return cells.size();
}

template<class F> static double TimeMs(F&& f){
  auto t0=std::chrono::high_resolution_clock::now(); f();
  auto t1=std::chrono::high_resolution_clock::now();
  return std::chrono::duration<double,std::milli>(t1-t0).count();
}

int main(){
  const int N = 197960;      // path points in the README baseline scene
  std::mt19937 rng(1234);
  std::uniform_real_distribution<double> U(0.0, 3000.0);
  std::vector<V3> pts(N);
  for(int i=0;i<N;++i) pts[i]=V3{U(rng),U(rng),U(rng)};

  volatile double sink=0;

  {
    double ms = TimeMs([&]{
      double acc=0;
      for(int r=0;r<10;++r) for(int i=0;i<N;++i) acc += PerlinNoise3D(pts[i].x*0.01,pts[i].y*0.01,pts[i].z*0.01);
      sink=acc;
    });
    printf("PerlinNoise3D : %10.2f ms / %8d calls = %6.2f ns/call\n", ms, N*10, ms*1e6/(N*10.0));
  }

  {
    double ms = TimeMs([&]{
      double acc=0;
      for(int r=0;r<10;++r) for(int i=0;i<N;++i){ V3 o=CurlNoise(pts[i],0.2,3.0); acc+=o.x; }
      sink=acc;
    });
    printf("CurlNoise(5x) : %10.2f ms / %8d calls = %6.2f ns/call\n", ms, N*10, ms*1e6/(N*10.0));
  }

  for(int side : {80, 160, 320, 640}){
    VoxelGrid G = MakeGrid(side, 7u);
    double gridMB = (G.Cells.size()*16.0 + G.Targets.size()*4.0 + G.Normals.size()*4.0 + G.Slots.size()*4.0)/1048576.0;
    std::vector<V3> q(N);
    std::mt19937 r2(99);
    std::uniform_real_distribution<double> UX(2.0, (double)side-3.0);
    for(int i=0;i<N;++i){
      double x=UX(r2), y=UX(r2);
      double h = 0.5*side + 0.18*side*std::sin(x*0.11)*std::cos(y*0.13);
      q[i]=V3{x*5.0, y*5.0, h*5.0};
    }
    double ms = TimeMs([&]{
      double acc=0; V3 out;
      for(int r=0;r<5;++r) for(int i=0;i<N;++i){ if(SampleVoxelNeighbors(G,q[i].x,q[i].y,q[i].z,0,0,0,5.0,out)) acc+=out.x; }
      sink=acc;
    });
    printf("Projection    : voxels=%8zu  slots=%8zu (%6.1f MB)  %9.2f ms / %8d calls = %6.2f ns/call\n",
           G.Cells.size(), G.Slots.size(), gridMB, ms, N*5, ms*1e6/(N*5.0));
  }
  // ---- BVH：测试场景拾取到约 100 万三角，按这个规模建树 + 查最近三角 ----
  for(size_t triCount : {size_t(250000), size_t(1000000)}){
    BVH B = MakeSurfaceBVH(triCount, 3200.0);
    double buildMs = TimeMs([&]{ B.Build(); });
    double bvhMB = (B.Tris.size()*sizeof(Tri) + B.Nodes.size()*sizeof(Node) + B.Idx.size()*4 + B.Cent.size()*4)/1048576.0;

    // 查询点贴着表面，和藤蔓点的分布一致——这是 BVH 剪枝最有利的情形。
    std::vector<V3> q(N);
    std::mt19937 r3(4242);
    std::uniform_real_distribution<double> UQ(50.0, 3150.0);
    int grid=(int)std::ceil(std::sqrt(triCount/2.0));
    double step=3200.0/grid;
    for(int i=0;i<N;++i){
      double x=UQ(r3), y=UQ(r3);
      double h = 0.5*3200.0 + 0.10*3200.0*std::sin((x/step)*0.031)*std::cos((y/step)*0.027);
      q[i]=V3{x,y,h+2.0};
    }
    const int QN = 200000;   // 一遍就够：单次成本是 µs 级
    double ms = TimeMs([&]{
      double acc=0; float d2;
      for(int i=0;i<QN;++i){
        int t=B.FindNearestTriangle((float)q[i%N].x,(float)q[i%N].y,(float)q[i%N].z,d2);
        acc += t + d2;
      }
      sink=acc;
    });
    printf("BVH nearest   : tris=%8zu nodes=%8zu (%6.1f MB)  build=%8.1f ms  query %8.2f ms / %6d = %7.0f ns/call\n",
           B.Tris.size(), B.Nodes.size(), bvhMB, buildMs, ms, QN, ms*1e6/QN);
  }

  // ---- 表面体素化：同样按 100 万三角、VoxelSize=5 ----
  {
    BVH B = MakeSurfaceBVH(1000000, 3200.0);
    float origin[3]={0,0,0};
    size_t voxels=0;
    double ms = TimeMs([&]{ voxels = VoxelizeTriangles(B.Tris, 5.0f, origin, 1u<<22); });
    printf("Voxelize      : tris=%8zu VoxelSize=5  -> voxels=%8zu  %9.1f ms = %6.0f ns/tri\n",
           B.Tris.size(), voxels, ms, ms*1e6/B.Tris.size());
  }

  printf("sink=%g\n",(double)sink);
  return 0;
}
