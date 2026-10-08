// Independent SDK path-segment oracle (door counterexample fixture).
// Production FIMAcousticSimulation + real IPLPathingVisualizationCallback via
// SetPathVisualization (forwarded to Shared.pathingVisCallback in
// EvaluateBatch). No self-made pathfinding, no HRTF/audio render, no reverb.
// SDK contract phonon.h 4055-4065: callback(from=starting probe,
// to=ending probe, occluded, userData) visualizes valid path segments during
// iplSimulatorRunPathing. Like SDK 4.8.1 path_simulator.cpp it only draws
// probe-interior segments. Source/first-probe and last-probe/listener legs
// are UNCOVERED and never claimed. Endpoint epsilon 1e-4 m is far below the
// 0.1 m wall thickness, so it only suppresses endpoint touching.
// Occlusion convention: 1 = open, 0 = fully blocked (matches smoke gates).
// Exit: 0 = harness complete, no counterexample; 2 = harness complete but
// production counterexample observed; 1 = harness/SDK error.
#include "IMAcousticSimulation.h"
#include "IMAcousticSDKContext.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#define NOMINMAX
#include <Windows.h>
#include <DbgHelp.h>
LONG WINAPI CrashEvidence(EXCEPTION_POINTERS* Exception)
{
    HANDLE Process=GetCurrentProcess();
    SymInitialize(Process,nullptr,TRUE);
    CONTEXT Context=*Exception->ContextRecord;
    std::cerr << "fault_rip=" << Context.Rip << std::endl;
    if(Context.Rip==0) { Context.Rip=*reinterpret_cast<DWORD64*>(Context.Rsp);Context.Rsp+=8; }
    STACKFRAME64 Frame{};
    Frame.AddrPC.Offset=Context.Rip;Frame.AddrPC.Mode=AddrModeFlat;
    Frame.AddrFrame.Offset=Context.Rbp;Frame.AddrFrame.Mode=AddrModeFlat;
    Frame.AddrStack.Offset=Context.Rsp;Frame.AddrStack.Mode=AddrModeFlat;
    std::cerr << "exception=" << Exception->ExceptionRecord->ExceptionCode << std::endl;
    for(int I=0;I<24 && Frame.AddrPC.Offset;++I) {
        char Storage[sizeof(SYMBOL_INFO)+MAX_SYM_NAME]{};
        auto* Symbol=reinterpret_cast<SYMBOL_INFO*>(Storage);
        Symbol->SizeOfStruct=sizeof(SYMBOL_INFO);Symbol->MaxNameLen=MAX_SYM_NAME;
        DWORD64 Displacement=0;
        const DWORD64 Base=SymGetModuleBase64(Process,Frame.AddrPC.Offset);
        std::cerr << "pc=" << Frame.AddrPC.Offset << " baseoff=" << (Frame.AddrPC.Offset-Base);
        if(SymFromAddr(Process,Frame.AddrPC.Offset,&Displacement,Symbol))std::cerr << " sym=" << Symbol->Name;
        std::cerr << std::endl;
        if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,Process,GetCurrentThread(),&Frame,&Context,nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr))break;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
namespace IMSteamAudioPathsPrivate
{
void Require(bool Value, const std::string& Error) { if (!Value) throw std::runtime_error(Error); }
void Box(FIMAcousticSceneInput& S, IPLVector3 Lo, IPLVector3 Hi)
{
    const int Base = static_cast<int>(S.Vertices.size());
    for (int I = 0; I < 8; ++I)
        S.Vertices.push_back({I&1 ? Hi.x : Lo.x, I&2 ? Hi.y : Lo.y, I&4 ? Hi.z : Lo.z});
    const int Faces[12][3] = {{0,2,3},{0,3,1},{4,5,7},{4,7,6},
        {0,1,5},{0,5,4},{2,6,7},{2,7,3},{0,4,6},{0,6,2},{1,3,7},{1,7,5}};
    for (const auto& F : Faces) { S.Triangles.push_back({{Base+F[0],Base+F[1],Base+F[2]}}); S.MaterialIndices.push_back(0); }
}
FIMAcousticSceneInput Scene(bool Open)
{
    FIMAcousticSceneInput S;
    IPLMaterial M{};
    for (int B=0; B<3; ++B) { M.absorption[B]=0.25f; M.transmission[B]=0.0f; }
    M.scattering=0.5f;
    S.Materials.push_back(M);
    Box(S,{-4.1f,-.1f,-3.1f},{4.1f,0,3.1f});
    Box(S,{-4.1f,3,-3.1f},{4.1f,3.1f,3.1f});
    Box(S,{-4.1f,0,-3.1f},{-4,3,3.1f});
    Box(S,{4,0,-3.1f},{4.1f,3,3.1f});
    Box(S,{-4,0,-3.1f},{4,3,-3});
    Box(S,{-4,0,3},{4,3,3.1f});
    if (Open) {
        Box(S,{-.05f,0,-3},{.05f,3,.5f});
        Box(S,{-.05f,0,2.5f},{.05f,3,3});
        Box(S,{-.05f,2.5f,.5f},{.05f,3,2.5f});
    } else Box(S,{-.05f,0,-3},{.05f,3,3});
    return S;
}
IPLCoordinateSpace3 Space(float X,float Z)
{
    return {{1,0,0},{0,1,0},{0,0,-1},{X,1.5f,Z}};
}
struct CbSeg { float Fx,Fy,Fz,Tx,Ty,Tz; int Occ; };
void IPLCALL PathCb(IPLVector3 From, IPLVector3 To, IPLbool Occ, void* Ud)
{
    auto* V=static_cast<std::vector<CbSeg>*>(Ud);
    if (V) V->push_back({From.x,From.y,From.z,To.x,To.y,To.z,Occ?1:0});
}
struct V3 { double x,y,z; };
static V3 VSub(V3 A,V3 B){return {A.x-B.x,A.y-B.y,A.z-B.z};}
static V3 VCross(V3 A,V3 B){return {A.y*B.z-A.z*B.y,A.z*B.x-A.x*B.z,A.x*B.y-A.y*B.x};}
static double VDot(V3 A,V3 B){return A.x*B.x+A.y*B.y+A.z*B.z;}
static V3 ToV3(IPLVector3 V){return {(double)V.x,(double)V.y,(double)V.z};}
static V3 MkV3(float X,float Y,float Z){return {(double)X,(double)Y,(double)Z};}
static bool SegTriHit(V3 A,V3 B,V3 V0,V3 V1,V3 V2,double EndEpsM)
{
    V3 D=VSub(B,A);
    const double Len=std::sqrt(VDot(D,D));
    if (!(Len>1e-12)) return false;
    const double T0=EndEpsM/Len, T1=1.0-T0;
    if (T1<=T0) return false;
    V3 E1=VSub(V1,V0),E2=VSub(V2,V0);
    V3 P=VCross(D,E2);
    const double Det=VDot(E1,P);
    if (Det<1e-12 && Det>-1e-12) return false;
    const double Inv=1.0/Det;
    V3 S=VSub(A,V0);
    const double U=VDot(S,P)*Inv;
    if (U<-1e-9||U>1.0+1e-9) return false;
    V3 Q=VCross(S,E1);
    const double V=VDot(D,Q)*Inv;
    if (V<-1e-9||U+V>1.0+1e-9) return false;
    const double T=VDot(E2,Q)*Inv;
    if (T<T0||T>T1) return false;
    return true;
}
static int OracleViol(V3 A,V3 B,const std::vector<IPLVector3>& Vs,
    const std::vector<IPLTriangle>& Ts,double Eps)
{
    int H=0;
    for (const auto& T:Ts)
        if (SegTriHit(A,B,ToV3(Vs[T.indices[0]]),ToV3(Vs[T.indices[1]]),ToV3(Vs[T.indices[2]]),Eps)) ++H;
    return H;
}
static double SHEnergy(const FIMAcousticAudioFrame& F){double E=0;for(float V:F.PathSH)E+=double(V)*V;return E;}
static bool EvalOne(FIMAcousticSimulation& Sim,float SX,float SZ,float LX,float LZ,
    std::vector<CbSeg>& Segs,FIMAcousticAudioFrame& Frame,std::string& Error)
{
    Segs.clear();
    Sim.SetPathVisualization(PathCb,&Segs);
    const bool Ok=Sim.Evaluate(1,1,Space(SX,SZ),Space(LX,LZ),Frame,Error);
    Sim.SetPathVisualization(nullptr,nullptr);
    return Ok;
}
struct RowRes {
    std::string Scene,Name; float SX,SZ,LX,LZ;
    float Occ=0; bool Valid=false; float EQ[3]={}; double SH=0;
    int CbT=0,CbC=0,CbO=0,Viol=0,DHit=0;
};
struct W2Case {
    std::string Name,Group,Direct,Path;
    int Step=-1;
    double T=-1;
    double SourceUE[3]{},ListenerUE[3]{};
    IPLVector3 Source{},Listener{};
};
} // namespace
int main(int Argc,char** Argv)
{
    SetUnhandledExceptionFilter(CrashEvidence);
    IPLContext Context=nullptr;
    const double EndEpsM=1e-4;
    try {
        IMSteamAudioPathsPrivate::Require(Argc==2||Argc==4,"usage: paths evidence-dir [frozen-bake-dir w2-cases.txt]");
        const std::filesystem::path Dir=Argv[1];std::filesystem::create_directories(Dir);
        IMSteamAudioPathsPrivate::Require(IMAcousticSDKContext::GetAcousticSDKContext()!=nullptr,"context create");
        Context=iplContextRetain(IMAcousticSDKContext::GetAcousticSDKContext());
        IPLProbeGenerationParams Params{};Params.type=IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
        Params.spacing=1;Params.height=1.5f;
        Params.transform.elements[0][0]=8;
        Params.transform.elements[1][1]=3;Params.transform.elements[1][3]=1.5f;
        Params.transform.elements[2][2]=6;
        Params.transform.elements[3][3]=1;
        FIMAcousticSceneInput ClosedIn=IMSteamAudioPathsPrivate::Scene(false),OpenIn=IMSteamAudioPathsPrivate::Scene(true);
        ClosedIn.Probes.clear();OpenIn.Probes.clear();
        FIMAcousticSimulation Tmp;std::string Error;
        std::vector<IPLSphere> PC,PO;
        IMSteamAudioPathsPrivate::Require(Tmp.GenerateProbes(ClosedIn,Params,PC,Error),std::string("closed gen: ")+Error);
        IMSteamAudioPathsPrivate::Require(Tmp.GenerateProbes(OpenIn,Params,PO,Error),std::string("open gen: ")+Error);
        IMSteamAudioPathsPrivate::Require(!PC.empty()&&!PO.empty(),"SDK generated no probes");
        ClosedIn.Probes=PC;OpenIn.Probes=PO;
        FIMAcousticBakeData ClosedBake,OpenBake;
        IMSteamAudioPathsPrivate::Require(Tmp.Bake(ClosedIn,ClosedBake,Error),std::string("closed bake: ")+Error);
        IMSteamAudioPathsPrivate::Require(Tmp.Bake(OpenIn,OpenBake,Error),std::string("open bake: ")+Error);
        std::ofstream(Dir/"closed.scene",std::ios::binary).write(
            reinterpret_cast<const char*>(ClosedBake.Scene.data()),ClosedBake.Scene.size());
        std::ofstream(Dir/"closed.probes",std::ios::binary).write(
            reinterpret_cast<const char*>(ClosedBake.ProbeBatch.data()),ClosedBake.ProbeBatch.size());
        std::ofstream(Dir/"open.scene",std::ios::binary).write(
            reinterpret_cast<const char*>(OpenBake.Scene.data()),OpenBake.Scene.size());
        std::ofstream(Dir/"open.probes",std::ios::binary).write(
            reinterpret_cast<const char*>(OpenBake.ProbeBatch.data()),OpenBake.ProbeBatch.size());
        FIMAcousticSimulation SimC,SimO;
        IMSteamAudioPathsPrivate::Require(SimC.Load(ClosedBake,48000,512,Error),std::string("closed load: ")+Error);
        IMSteamAudioPathsPrivate::Require(SimO.Load(OpenBake,48000,512,Error),std::string("open load: ")+Error);
        std::cout << "closed_probes=" << PC.size() << " open_probes=" << PO.size() << std::endl;
        struct Spec { const char* Sc; const char* Nm; float SX,SZ,LX,LZ; };
        std::vector<Spec> Specs;
        Specs.push_back({"closed","far",-2,-1.5f,2,-1.5f});
        Specs.push_back({"open","far",-2,-1.5f,2,-1.5f});
        Specs.push_back({"closed","adjacent",-0.25f,-1.5f,0.25f,-1.5f});
        Specs.push_back({"open","adjacent-far-from-door",-0.25f,-1.5f,0.25f,-1.5f});
        const float SweepZ[6]={-2.5f,-1.5f,-0.5f,0.5f,1.5f,2.5f};
        for(float Z:SweepZ){Specs.push_back({"closed","sweep-adj",-0.3f,Z,0.3f,Z});}
        for(float Z:SweepZ){Specs.push_back({"open","sweep-adj",-0.3f,Z,0.3f,Z});}
        for(int K=0;K<4;++K){
            float S=-2+0.25f*K,L=2-0.25f*K,Z=-1.5f+0.5f*K;
            Specs.push_back({"open","moving-pair",S,Z,L,Z});
            Specs.push_back({"closed","moving-pair",S,Z,L,Z});
        }
        std::vector<IMSteamAudioPathsPrivate::RowRes> Rows;Rows.reserve(Specs.size());
        std::ofstream Csv(Dir/"per-point.csv");
        Csv<<std::setprecision(9);
        Csv<<"scene,case,src_x,src_z,lst_x,lst_z,direct_occlusion,path_valid,eq0,eq1,eq2,sh_energy,cb_total,cb_clear,cb_occluded,oracle_violations,direct_seg_hits,note\n";
        std::ofstream All(Dir/"callback-segments.csv");
        All<<std::setprecision(9)<<"scene,case,point_index,from_x,from_y,from_z,to_x,to_y,to_z,occluded,oracle_hit\n";
        for(const auto& Sp:Specs){
            FIMAcousticSimulation& Sim=(std::string(Sp.Sc)=="closed")?SimC:SimO;
            const FIMAcousticSceneInput& Geo=(std::string(Sp.Sc)=="closed")?ClosedIn:OpenIn;
            std::vector<IMSteamAudioPathsPrivate::CbSeg> Segs;FIMAcousticAudioFrame Fr;
            IMSteamAudioPathsPrivate::Require(IMSteamAudioPathsPrivate::EvalOne(Sim,Sp.SX,Sp.SZ,Sp.LX,Sp.LZ,Segs,Fr,Error),std::string(Sp.Sc)+"/"+Sp.Nm+": "+Error);
            IMSteamAudioPathsPrivate::RowRes R;R.Scene=Sp.Sc;R.Name=Sp.Nm;R.SX=Sp.SX;R.SZ=Sp.SZ;R.LX=Sp.LX;R.LZ=Sp.LZ;
            R.Occ=Fr.Direct.occlusion;R.Valid=Fr.PathValid;
            R.EQ[0]=Fr.PathEQ[0];R.EQ[1]=Fr.PathEQ[1];R.EQ[2]=Fr.PathEQ[2];R.SH=IMSteamAudioPathsPrivate::SHEnergy(Fr);
            R.CbT=(int)Segs.size();
            int Viol=0;
            for(const auto& Sg:Segs){
                if(Sg.Occ)R.CbO++;else R.CbC++;
                if(!Sg.Occ) Viol+=IMSteamAudioPathsPrivate::OracleViol(IMSteamAudioPathsPrivate::MkV3(Sg.Fx,Sg.Fy,Sg.Fz),IMSteamAudioPathsPrivate::MkV3(Sg.Tx,Sg.Ty,Sg.Tz),
                    Geo.Vertices,Geo.Triangles,EndEpsM);
            }
            R.Viol=Viol;
            IMSteamAudioPathsPrivate::V3 SA={(double)Sp.SX,1.5,(double)Sp.SZ},LA={(double)Sp.LX,1.5,(double)Sp.LZ};
            R.DHit=IMSteamAudioPathsPrivate::OracleViol(SA,LA,Geo.Vertices,Geo.Triangles,EndEpsM);
            Rows.push_back(R);
            Csv<<R.Scene<<','<<R.Name<<','<<R.SX<<','<<R.SZ<<','<<R.LX<<','<<R.LZ<<','
               <<R.Occ<<','<<(R.Valid?"1":"0")<<','<<R.EQ[0]<<','<<R.EQ[1]<<','<<R.EQ[2]<<','
               <<R.SH<<','<<R.CbT<<','<<R.CbC<<','<<R.CbO<<','<<R.Viol<<','<<R.DHit<<",callback-covers-probe-interior-only\n";
            for(const auto& Sg:Segs){
                int Hit=IMSteamAudioPathsPrivate::OracleViol(IMSteamAudioPathsPrivate::MkV3(Sg.Fx,Sg.Fy,Sg.Fz),IMSteamAudioPathsPrivate::MkV3(Sg.Tx,Sg.Ty,Sg.Tz),
                    Geo.Vertices,Geo.Triangles,EndEpsM);
                All<<R.Scene<<','<<R.Name<<','<<(Rows.size()-1)<<','<<Sg.Fx<<','<<Sg.Fy<<','<<Sg.Fz<<','
                   <<Sg.Tx<<','<<Sg.Ty<<','<<Sg.Tz<<','<<Sg.Occ<<','<<Hit<<"\n";
            }
        }
        auto Find=[&](const std::string& Sc,const std::string& Nm)->const IMSteamAudioPathsPrivate::RowRes&{
            for(const auto& R:Rows) if(R.Scene==Sc&&R.Name==Nm) return R;
            throw std::runtime_error("missing row "+Sc+"/"+Nm);
        };
        bool Pass=true;std::string Notes;
        auto Gate=[&](bool Ok,const std::string& Msg){
            std::cout << (Ok?"GATE-OK ":"GATE-FAIL ") << Msg << std::endl;
            Notes+=std::string(Ok?"OK ":"FAIL ")+Msg+"\n";
            if(!Ok) Pass=false;
        };
        {
            const IMSteamAudioPathsPrivate::RowRes& R=Find("closed","far");
            Gate(!R.Valid,"sealed-wall far pair must have no path (PathValid=false), got valid="+std::string(R.Valid?"true":"false"));
            Gate(R.Occ<=0.01f,"sealed-wall far direct must be blocked (occlusion<=0.01), got "+std::to_string(R.Occ));
        }
        {
            const IMSteamAudioPathsPrivate::RowRes& R=Find("open","far");
            Gate(R.Valid,"open-door far pair must recover path (PathValid=true)");
            Gate(R.Occ<=0.01f,"open-door far direct stays wall-blocked (occlusion<=0.01), got "+std::to_string(R.Occ));
            Gate(R.CbC>0,"open-door far must report clear probe segments (cb_clear>0), got "+std::to_string(R.CbC));
            Gate(R.Viol==0,"open-door far clear segments must not cross walls, violations="+std::to_string(R.Viol));
            Gate(R.DHit>0,"sanity: straight source-listener line must hit the wall (proves valid path is not direct), hits="+std::to_string(R.DHit));
        }
        {
            const IMSteamAudioPathsPrivate::RowRes& R=Find("closed","adjacent");
            Gate(R.Occ<=0.01f,"sealed adjacent-across-wall direct must be blocked (occlusion<=0.01), got "+std::to_string(R.Occ));
            Gate(!R.Valid,"sealed adjacent-across-wall must have no path (else through-wall), got valid="+std::string(R.Valid?"true":"false"));
            if(R.Valid) Gate(R.Viol==0,"sealed adjacent claimed path must have zero wall crossings, violations="+std::to_string(R.Viol));
        }
        {
            const IMSteamAudioPathsPrivate::RowRes& R=Find("open","adjacent-far-from-door");
            Gate(R.Occ<=0.01f,"open adjacent-across-wall direct must be blocked (occlusion<=0.01), got "+std::to_string(R.Occ));
            if(R.Valid){
                Gate(R.CbC>0,"open adjacent claimed path needs real clear segments, got "+std::to_string(R.CbC));
                Gate(R.Viol==0,"open adjacent clear segments must not cross walls, violations="+std::to_string(R.Viol));
                Gate(R.DHit>0,"open adjacent straight line must hit wall (not direct), hits="+std::to_string(R.DHit));
            } else std::cout << "GATE-OK open adjacent has no path (acceptable, recorded)" << std::endl;
        }
        for(const auto& R:Rows){
            const std::string Id=R.Scene+"/"+R.Name+" z="+std::to_string(R.SZ);
            if(R.Scene=="closed") {
                Gate(!R.Valid,Id+": every sealed-wall scan must have no path");
                Gate(R.Occ<=0.01f,Id+": every sealed-wall scan must have blocked direct");
            }
            Gate(R.Viol==0,Id+": all callback-clear probe segments must be triangle-clear");
            // The two exact open-door jamb samples lie on the box boundary.
            // Retain their raw hit/occlusion values without treating edge ownership
            // (SDK float ray vs inclusive double triangle test) as an interior hit.
            const bool DoorEdge=R.Scene=="open"&&R.Name=="sweep-adj"&&(R.SZ==0.5f||R.SZ==2.5f);
            if(R.DHit>0&&!DoorEdge) Gate(R.Occ<=0.01f,Id+": non-edge direct triangle hit must be blocked");
            if(DoorEdge) std::cout<<"EDGE-AMBIGUITY "<<Id<<" exact jamb z=0.5/2.5m; raw result retained"<<std::endl;
        }
        std::cout << "door-gates-pass=" << (Pass?"true":"false") << std::endl;
        const bool DoorPass=Pass;
        bool W2=false,W2Pass=true;int W2Count=0,W2Failures=0,W2Trajectory=0,W2PathCount=0,W2ClearSegments=0,W2Violations=0;
        std::string W2Note="not-run";
        if(Argc>=3){
            W2=true;
            const std::filesystem::path BakeDir=Argv[2];
            std::vector<uint8_t> SB,PB;
            {
                std::ifstream F(BakeDir/"scene.bin",std::ios::binary);
                IMSteamAudioPathsPrivate::Require(F.good(),"W2 scene.bin unavailable");
                SB.assign(std::istreambuf_iterator<char>(F),std::istreambuf_iterator<char>());
            }
            {
                std::ifstream F(BakeDir/"probes.bin",std::ios::binary);
                IMSteamAudioPathsPrivate::Require(F.good(),"W2 probes.bin unavailable");
                PB.assign(std::istreambuf_iterator<char>(F),std::istreambuf_iterator<char>());
            }
            IPLSerializedObject SObj=nullptr;IPLScene WS=nullptr;
            IPLSerializedObjectSettings SS{};
            SS.data=SB.data();SS.size=(IPLsize)SB.size();
            IMSteamAudioPathsPrivate::Require(iplSerializedObjectCreate(Context,&SS,&SObj)==IPL_STATUS_SUCCESS&&SObj,"W2 serial create");
            IPLSceneSettings St{};St.type=IPL_SCENETYPE_DEFAULT;
            IMSteamAudioPathsPrivate::Require(iplSceneLoad(Context,&St,SObj,nullptr,nullptr,&WS)==IPL_STATUS_SUCCESS&&WS,"W2 scene load");
            const std::string ObjBase=(Dir/"w2-whitebox").string();
            iplSceneSaveOBJ(WS,ObjBase.c_str());
            iplSceneRelease(&WS);iplSerializedObjectRelease(&SObj);
            std::filesystem::path ObjP=ObjBase+".obj";
            if(!std::filesystem::exists(ObjP)) ObjP=ObjBase;
            IMSteamAudioPathsPrivate::Require(std::filesystem::exists(ObjP),"W2 OBJ export missing");
            std::vector<IPLVector3> WV;std::vector<IPLTriangle> WT;
            {
                std::ifstream F(ObjP);
                IMSteamAudioPathsPrivate::Require(F.good(),"W2 OBJ open");
                std::string L;
                while(std::getline(F,L)){
                    if(L.size()>2&&L[0]=='v'&&L[1]==' '){
                        std::istringstream I(L.substr(2));float X,Y,Z;
                        IMSteamAudioPathsPrivate::Require(bool(I>>X>>Y>>Z)&&std::isfinite(X)&&std::isfinite(Y)&&std::isfinite(Z),"W2 OBJ invalid vertex");
                        WV.push_back({X,Y,Z});
                    } else if(L.size()>2&&L[0]=='f'&&L[1]==' '){
                        std::istringstream I(L.substr(2));std::string A,B,C,Extra;
                        IMSteamAudioPathsPrivate::Require(bool(I>>A>>B>>C)&&!(I>>Extra),"W2 OBJ requires triangle faces");
                        auto Idx=[](std::string T)->int{
                            auto P=T.find('/');if(P!=std::string::npos)T=T.substr(0,P);
                            std::size_t End=0;int N=std::stoi(T,&End);
                            IMSteamAudioPathsPrivate::Require(End==T.size()&&N>0,"W2 OBJ invalid positive index");return N-1;};
                        WT.push_back({{Idx(A),Idx(B),Idx(C)}});
                    }
                }
            }
            IMSteamAudioPathsPrivate::Require(!WV.empty()&&!WT.empty(),"W2 OBJ parse empty");
            // Validate before OracleViol dereferences an index; never skip bad faces.
            for(const auto& Tri:WT)for(int Id:Tri.indices)
                IMSteamAudioPathsPrivate::Require(Id>=0&&static_cast<std::size_t>(Id)<WV.size(),"W2 OBJ index out of bounds");
            std::cout << "w2-obj verts=" << WV.size() << " tris=" << WT.size() << std::endl;
            std::ifstream Cases(Argv[3]);
            int Count=0,ExpectedTriangles=0;
            IMSteamAudioPathsPrivate::Require(bool(Cases>>Count>>ExpectedTriangles)&&Count>0&&Count<=1024,"W2 case header invalid");
            IMSteamAudioPathsPrivate::Require(WT.size()==static_cast<std::size_t>(ExpectedTriangles),"W2 OBJ triangle count differs from metadata");
            std::vector<IMSteamAudioPathsPrivate::W2Case> SpecsW2;
            for(int I=0;I<Count;++I) {
                IMSteamAudioPathsPrivate::W2Case C;
                IMSteamAudioPathsPrivate::Require(bool(Cases>>C.Name>>C.Group>>C.Step>>C.T),"W2 case identity missing");
                for(double& V:C.SourceUE)IMSteamAudioPathsPrivate::Require(bool(Cases>>V)&&std::isfinite(V),"W2 source UE invalid");
                for(double& V:C.ListenerUE)IMSteamAudioPathsPrivate::Require(bool(Cases>>V)&&std::isfinite(V),"W2 listener UE invalid");
                IMSteamAudioPathsPrivate::Require(bool(Cases>>C.Source.x>>C.Source.y>>C.Source.z>>C.Listener.x>>C.Listener.y>>C.Listener.z>>C.Direct>>C.Path),"W2 case coordinates missing");
                for(float V:{C.Source.x,C.Source.y,C.Source.z,C.Listener.x,C.Listener.y,C.Listener.z})IMSteamAudioPathsPrivate::Require(std::isfinite(V),"W2 SDK position nonfinite");
                IMSteamAudioPathsPrivate::Require(C.Direct=="clear"||C.Direct=="blocked"||C.Direct=="oracle","W2 invalid direct contract");
                IMSteamAudioPathsPrivate::Require(C.Path=="absent"||C.Path=="optional","W2 invalid path contract");
                if(C.Group=="trajectory") {
                    IMSteamAudioPathsPrivate::Require(C.Step==W2Trajectory&&std::abs(C.T-C.Step/63.0)<1e-12,"W2 trajectory must be ordered 0..63");
                    ++W2Trajectory;
                }
                SpecsW2.push_back(C);
            }
            std::string Extra;
            IMSteamAudioPathsPrivate::Require(!(Cases>>Extra)&&W2Trajectory==64,"W2 expected exactly 64 trajectory steps and no trailing cases");
            FIMAcousticBakeData WB;WB.Scene=SB;WB.ProbeBatch=PB;
            FIMAcousticSimulation WSim;
            IMSteamAudioPathsPrivate::Require(WSim.Load(WB,48000,512,Error),std::string("W2 load: ")+Error);
            std::ofstream WC(Dir/"w2-per-point.csv"),WSC(Dir/"w2-callback-segments.csv");
            WC<<std::setprecision(12)<<"case,group,step,t,src_ue_x_cm,src_ue_y_cm,src_ue_z_cm,lst_ue_x_cm,lst_ue_y_cm,lst_ue_z_cm,src_sdk_x_m,src_sdk_y_m,src_sdk_z_m,lst_sdk_x_m,lst_sdk_y_m,lst_sdk_z_m,generation,sequence,direct_valid,occlusion,path_valid,eq0,eq1,eq2,sh0,sh1,sh2,sh3,sh_energy,cb_total,cb_clear,cb_occluded,oracle_violations,direct_seg_hits,expected_direct,expected_path,pass,source_step_m,listener_step_m,normalized_sh_delta_l2,eq_delta_l2\n";
            WSC<<std::setprecision(12)<<"case,group,step,segment,from_x,from_y,from_z,to_x,to_y,to_z,occluded,oracle_hit\n";
            FIMAcousticAudioFrame Previous{};IPLVector3 PreviousSource{},PreviousListener{};
            bool HavePrevious=false;
            for(const auto& C:SpecsW2){
                std::vector<IMSteamAudioPathsPrivate::CbSeg> Segs;FIMAcousticAudioFrame Fr;
                IPLCoordinateSpace3 S{{1,0,0},{0,1,0},{0,0,-1},C.Source};
                IPLCoordinateSpace3 Ls{{1,0,0},{0,1,0},{0,0,-1},C.Listener};
                // Fixed counterexamples reset the SDK cache. The ordered movement
                // sequence deliberately retains one source key AND generation.
                const std::uint64_t Generation=C.Group=="trajectory"?1000:W2Count+1;
                Segs.clear();WSim.SetPathVisualization(IMSteamAudioPathsPrivate::PathCb,&Segs);
                IMSteamAudioPathsPrivate::Require(WSim.Evaluate(1,Generation,S,Ls,Fr,Error),std::string("W2 eval ")+C.Name+": "+Error);
                WSim.SetPathVisualization(nullptr,nullptr);
                int Viol=0,Cc=0,Co=0,SegIndex=0;
                for(const auto& Sg:Segs){if(Sg.Occ)Co++;else Cc++;
                    const int Hit=IMSteamAudioPathsPrivate::OracleViol(IMSteamAudioPathsPrivate::MkV3(Sg.Fx,Sg.Fy,Sg.Fz),IMSteamAudioPathsPrivate::MkV3(Sg.Tx,Sg.Ty,Sg.Tz),WV,WT,EndEpsM);
                    if(!Sg.Occ)Viol+=Hit;
                    WSC<<C.Name<<','<<C.Group<<','<<C.Step<<','<<SegIndex++<<','<<Sg.Fx<<','<<Sg.Fy<<','<<Sg.Fz<<','
                        <<Sg.Tx<<','<<Sg.Ty<<','<<Sg.Tz<<','<<Sg.Occ<<','<<Hit<<'\n';
                }
                const int DHit=IMSteamAudioPathsPrivate::OracleViol(IMSteamAudioPathsPrivate::ToV3(C.Source),IMSteamAudioPathsPrivate::ToV3(C.Listener),WV,WT,EndEpsM);
                bool RowPass=true;
                auto WGate=[&](bool Ok,const std::string& Msg){Gate(Ok,"W2 "+C.Name+": "+Msg);if(!Ok)RowPass=false;};
                WGate(Fr.DirectValid&&std::isfinite(Fr.Direct.occlusion),"direct output is valid and finite");
                if(C.Direct=="clear")WGate(DHit==0,"fixed LoS expectation agrees with exported triangles");
                if(C.Direct=="blocked")WGate(DHit>0,"fixed blocked expectation intersects exported triangles");
                // No W2 edge exemptions: a disagreement remains a counterexample.
                WGate(DHit>0?Fr.Direct.occlusion<=0.01f:Fr.Direct.occlusion>=0.99f,
                    "direct occlusion agrees with independent triangle oracle; hits="+std::to_string(DHit)+" occlusion="+std::to_string(Fr.Direct.occlusion));
                if(C.Path=="absent"||DHit==0)WGate(!Fr.PathValid,"path must be absent (sealed floor or direct branch owns LoS)");
                WGate(Viol==0,"callback-clear probe-interior segments must not cross geometry; violations="+std::to_string(Viol));
                if(Fr.PathValid) {
                    // A valid one-probe path can have no interior callback legs;
                    // it remains endpoint-unverified, never a full-path legality pass.
                    WGate(DHit>0&&IMSteamAudioPathsPrivate::SHEnergy(Fr)>0,"claimed indirect path has blocked direct and positive SH energy");
                }
                if(C.Group=="trajectory")WGate(Fr.Sequence==static_cast<std::uint64_t>(C.Step+1),"movement retains source generation and increasing sequence");
                WC<<C.Name<<','<<C.Group<<','<<C.Step<<','<<C.T;
                for(double V:C.SourceUE)WC<<','<<V;for(double V:C.ListenerUE)WC<<','<<V;
                WC<<','<<C.Source.x<<','<<C.Source.y<<','<<C.Source.z<<','<<C.Listener.x<<','<<C.Listener.y<<','<<C.Listener.z
                  <<','<<Generation<<','<<Fr.Sequence<<','<<Fr.DirectValid<<','<<Fr.Direct.occlusion<<','<<Fr.PathValid;
                for(float V:Fr.PathEQ)WC<<','<<V;for(float V:Fr.PathSH)WC<<','<<V;
                WC<<','<<IMSteamAudioPathsPrivate::SHEnergy(Fr)<<','<<Segs.size()<<','<<Cc<<','<<Co<<','<<Viol<<','<<DHit<<','<<C.Direct<<','<<C.Path<<','<<RowPass;
                if(C.Group=="trajectory"&&HavePrevious) {
                    auto Distance=[](IPLVector3 A,IPLVector3 B){const auto D=IMSteamAudioPathsPrivate::VSub(IMSteamAudioPathsPrivate::ToV3(A),IMSteamAudioPathsPrivate::ToV3(B));return std::sqrt(IMSteamAudioPathsPrivate::VDot(D,D));};
                    WC<<','<<Distance(C.Source,PreviousSource)<<','<<Distance(C.Listener,PreviousListener);
                    if(Fr.PathValid&&Previous.PathValid) {
                        double SHD=0,EQD=0;const double A=std::sqrt(IMSteamAudioPathsPrivate::SHEnergy(Fr)),B=std::sqrt(IMSteamAudioPathsPrivate::SHEnergy(Previous));
                        for(int K=0;K<4;++K){const double D=Fr.PathSH[K]/A-Previous.PathSH[K]/B;SHD+=D*D;}
                        for(int K=0;K<3;++K){const double D=Fr.PathEQ[K]-Previous.PathEQ[K];EQD+=D*D;}
                        WC<<','<<std::sqrt(SHD)<<','<<std::sqrt(EQD);
                    } else WC<<",,";
                } else WC<<",,,,";
                WC<<'\n';
                if(C.Group=="trajectory"){Previous=Fr;PreviousSource=C.Source;PreviousListener=C.Listener;HavePrevious=true;}
                ++W2Count;W2PathCount+=Fr.PathValid?1:0;W2ClearSegments+=Cc;W2Violations+=Viol;
                if(!RowPass){W2Pass=false;++W2Failures;}
            }
            W2Note="actual UE-coordinate fixed/floor pairs and 64 sequential movement steps; endpoint legs and audible continuity unverified";
        }
        std::ofstream(Dir/"gate-notes.txt")<<Notes;
        std::ofstream J(Dir/"results.json");
        J<<"{\"scope\":\"sdk-path-oracle-door-and-w2-sampled\",\"pass\":"<<(Pass?"true":"false")<<",\"door_pass\":"<<(DoorPass?"true":"false");
        J<<",\"w2\":"<<(W2?"true":"false")<<",\"w2_note\":\""<<W2Note<<"\"";
        J<<",\"w2_pass\":"<<(W2?(W2Pass?"true":"false"):"null")<<",\"w2_points\":"<<W2Count<<",\"w2_failed_points\":"<<W2Failures;
        J<<",\"w2_trajectory_steps\":"<<W2Trajectory<<",\"w2_path_valid_points\":"<<W2PathCount<<",\"w2_clear_callback_segments\":"<<W2ClearSegments<<",\"w2_clear_segment_triangle_hits\":"<<W2Violations;
        J<<",\"callback_coverage\":\"probe-interior-only; source/probe and probe/listener legs UNCOVERED\"";
        J<<",\"epsilon_m\":"<<EndEpsM<<",\"wall_m\":0.1";
        J<<",\"sdk\":\"4.8.1\",\"phonon_ref\":\"pathingVisCallback 4055-4065, iplSceneLoad 1002, iplSceneSaveOBJ 1024\"";
        J<<",\"ue_audio_gate\":\"NOT_RUN\",\"hrtfs\":\"NONE\",\"full_map\":\"NOT_VERIFIED\"}\n";
        iplContextRelease(&Context);
        return Pass?0:2;
    } catch(const std::exception& E) {
        std::cerr<<"FAIL "<<E.what()<<std::endl;
        if(Context)iplContextRelease(&Context);return 1;
    }
}
