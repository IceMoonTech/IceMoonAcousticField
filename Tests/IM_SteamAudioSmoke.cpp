// D1-unit: renderer/history controlled counterexample (fixed A/B, one variable).
// D1 fix vs D: fixture sets Direct.flags = APPLYOCCLUSION-only (8). D left
// flags=0 so occlusion was silently ignored (fixture-only defect; production
// simulation already configures its own flag set; NOT copied here).
// Artificial fixture. Does NOT fit old 655B energies or tails, claims no UE
// routing/recording/audition PASS, and changes no product source.
//
// Protocol (frozen):
// - 48kHz, 1024 frames/block. W1 multi-tone PCM (233/997/3109Hz, 2600/32768
//   float, identical 1024-sample buffer looped every block: artificial).
// - Two independent IM_AcousticAudioRenderer instances (A and B), each with an
//   exclusive HRTF, sharing one process IPLContext. Single-threaded: A runs
//   fully, then B. Direct product renderer compilation, no test double.
// - Frame identity identical across branches: Generation constant (no Reset,
//   so no generation-triggered history clear), Sequence shared per block.
// - History blocks 0..9: A occlusion=0, B occlusion=1 (pollution charge).
// - Observation blocks 10..103: both occlusion=0, identical frames.
// - AudibleRoutes 7 pre-switch, 1 post-switch, both branches (existing 7->1).
// - Post-switch output fully preserved: 94 blocks = 96256 samples = 2.00533s.
//   Window W03 = samples [0,14400) (exact 0.3s, NOT block-rounded).
//   Window Wafter = samples [14400,96256).
// - Evidence: full params (full precision), per-block float energies from the
//   renderer Metrics, raw float32 stereo+stems (exact), PCM16 conversion with
//   an explicit quantization rule, last nonzero sample positions.
// - Float text evidence uses %.17g; raw binaries carry exact precision.
#include "IMAcousticAudioRenderer.h"
#include "IMAcousticSDKContext.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int kRate = 48000;
constexpr int kBlock = 1024;
constexpr int kHistBlocks = 10;
constexpr int kPostBlocks = 94;
constexpr int kTotalBlocks = kHistBlocks + kPostBlocks;
constexpr std::uint64_t kGeneration = 7;
constexpr double kToneAmp = 2600.0 / 32768.0;
constexpr double kFreqs[3] = {233.0, 997.0, 3109.0};
constexpr std::uint32_t kRoutesPre = 7;
constexpr std::uint32_t kRoutesPost = 1;
constexpr std::uint64_t kW03Samples = 14400; // exact 0.3s at 48kHz
// IPLDirectEffectFlags: APPLYOCCLUSION = 1<<3 = 8 (phonon.h). Occlusion only;
// deliberately NOT the production full set (distance|air|directivity|occlusion).
constexpr std::uint32_t kDirectFlags = 8;

void Require(bool Value, const std::string& Error) { if (!Value) throw std::runtime_error(Error); }

std::string Fmt17(double V) { char B[32]; std::snprintf(B, sizeof(B), "%.17g", V); return B; }
std::string FmtF(float V) { char B[32]; std::snprintf(B, sizeof(B), "%.17g", double(V)); return B; }

struct BlockRec {
    int Block = 0;
    std::uint64_t Seq = 0;
    float Occ = 0.f;
    std::uint32_t Flags = 0;
    std::uint32_t Routes = 0;
    int Ok = 0;
    int Fail = 0;
    double InputE = 0.0, DirectE = 0.0, PathE = 0.0;
};

IM_AcousticAudioFrame MakeFrame(float Occ, std::uint64_t Seq)
{
    IM_AcousticAudioFrame F;
    F.Generation = kGeneration;
    F.Sequence = Seq;
    F.DirectValid = true;
    F.PathValid = true;
    F.Direct.distanceAttenuation = 1.0f;
    for (int B = 0; B < IPL_NUM_BANDS; ++B) { F.Direct.airAbsorption[B] = 0.0f; F.Direct.transmission[B] = 0.0f; }
    F.Direct.directivity = 1.0f;
    F.Direct.occlusion = Occ;
    F.Direct.flags = static_cast<IPLDirectEffectFlags>(kDirectFlags);
    F.ListenerLocalDirection = {0.0f, 0.0f, -1.0f};
    F.Listener.origin = {2.0f, 0.0f, 0.0f};
    F.Listener.right = {1.0f, 0.0f, 0.0f};
    F.Listener.up = {0.0f, 1.0f, 0.0f};
    F.Listener.ahead = {0.0f, 0.0f, -1.0f};
    for (int B = 0; B < 3; ++B) F.PathEQ[B] = 1.0f;
    F.PathSH[0] = 1.0f;
    for (std::size_t I = 1; I < F.PathSH.size(); ++I) F.PathSH[I] = 0.0f;
    return F;
}

struct BranchOut {
    std::vector<BlockRec> Recs;
    std::vector<float> PreStereo, PreDirect, PrePath;   // hist blocks, interleaved stereo
    std::vector<float> PostStereo, PostDirect, PostPath; // observation blocks, interleaved
};

BranchOut RunBranch(IM_AcousticAudioRenderer& R, float HistOcc, const std::vector<float>& Input)
{
    BranchOut O;
    O.PreStereo.reserve(std::size_t(kHistBlocks) * kBlock * 2);
    O.PreDirect.reserve(std::size_t(kHistBlocks) * kBlock * 2);
    O.PrePath.reserve(std::size_t(kHistBlocks) * kBlock * 2);
    O.PostStereo.reserve(std::size_t(kPostBlocks) * kBlock * 2);
    O.PostDirect.reserve(std::size_t(kPostBlocks) * kBlock * 2);
    O.PostPath.reserve(std::size_t(kPostBlocks) * kBlock * 2);
    std::vector<float> Stereo(kBlock * 2), DS(kBlock * 2), PS(kBlock * 2);
    for (int B = 0; B < kTotalBlocks; ++B)
    {
        const bool Pre = B < kHistBlocks;
        const float Occ = Pre ? HistOcc : 0.0f;
        const std::uint32_t Routes = Pre ? kRoutesPre : kRoutesPost;
        IM_AcousticAudioFrame F = MakeFrame(Occ, std::uint64_t(B + 1));
        IM_AcousticAudioMetrics M;
        const bool Ok = R.Render(Input.data(), kBlock, F, Stereo.data(), DS.data(), PS.data(), &M, Routes);
        double InE = 0.0;
        for (int I = 0; I < kBlock; ++I) InE += double(Input[I]) * Input[I];
        BlockRec Rec;
        Rec.Block = B; Rec.Seq = std::uint64_t(B + 1); Rec.Occ = Occ; Rec.Flags = static_cast<std::uint32_t>(F.Direct.flags); Rec.Routes = Routes;
        Rec.Ok = Ok ? 1 : 0; Rec.Fail = int(M.Failure);
        Rec.InputE = InE; Rec.DirectE = M.DirectEnergy; Rec.PathE = M.PathEnergy;
        O.Recs.push_back(Rec);
        if (Pre) { O.PreStereo.insert(O.PreStereo.end(), Stereo.begin(), Stereo.end()); O.PreDirect.insert(O.PreDirect.end(), DS.begin(), DS.end()); O.PrePath.insert(O.PrePath.end(), PS.begin(), PS.end()); }
        else { O.PostStereo.insert(O.PostStereo.end(), Stereo.begin(), Stereo.end()); O.PostDirect.insert(O.PostDirect.end(), DS.begin(), DS.end()); O.PostPath.insert(O.PostPath.end(), PS.begin(), PS.end()); }
    }
    return O;
}

void WriteF32(const std::string& Path, const std::vector<float>& V)
{
    std::ofstream F(Path, std::ios::binary);
    Require(bool(F), "cannot open " + Path);
    F.write(reinterpret_cast<const char*>(V.data()), std::streamsize(V.size() * sizeof(float)));
    F.close();
    Require(bool(F), "cannot write " + Path);
}

// Explicit quantization rule (also recorded in params.json): half away from
// zero at 32767 scale, clamped to int16. Zero float maps to zero int16.
std::int16_t Quantize(float X)
{
    double V = double(X) * 32767.0;
    long R = std::lround(V);
    if (R > 32767) R = 32767;
    if (R < -32768) R = -32768;
    return std::int16_t(R);
}
} // namespace

int main(int argc, char** argv)
{
    try {
        Require(argc >= 2, "usage: IM_SteamAudioSmoke <output-dir>");
        const std::string Out = argv[1];
        // Block-periodic W1 multi-tone (identical buffer each block), both branches.
        std::vector<float> Input(kBlock);
        for (int I = 0; I < kBlock; ++I)
        {
            const double T = double(I) / kRate;
            double S = 0.0;
            for (double Fq : kFreqs) S += std::sin(2.0 * 3.141592653589793 * Fq * T);
            Input[I] = float(S * kToneAmp);
        }
        IPLContext Ctx = IM_GetAcousticSDKContext();
        Require(Ctx != nullptr, "null SDK context");
        IPLAudioSettings Audio{ kRate, kBlock };
        IPLHRTFSettings HS{};
        HS.type = IPL_HRTFTYPE_DEFAULT;
        HS.volume = 1.0f;
        // Branch A: clean history (occ=0 throughout pre-switch).
        IPLHRTF HrtfA = nullptr;
        Require(iplHRTFCreate(Ctx, &Audio, &HS, &HrtfA) == IPL_STATUS_SUCCESS && HrtfA, "HRTF A create failed");
        IM_AcousticAudioRenderer RA;
        Require(RA.Initialize(Ctx, HrtfA, kRate, kBlock), "renderer A init failed");
        iplHRTFRelease(&HrtfA);
        BranchOut A = RunBranch(RA, 0.0f, Input);
        // Branch B: polluted history (occ=1 pre-switch), then identical occ=0.
        IPLHRTF HrtfB = nullptr;
        Require(iplHRTFCreate(Ctx, &Audio, &HS, &HrtfB) == IPL_STATUS_SUCCESS && HrtfB, "HRTF B create failed");
        IM_AcousticAudioRenderer RB;
        Require(RB.Initialize(Ctx, HrtfB, kRate, kBlock), "renderer B init failed");
        iplHRTFRelease(&HrtfB);
        BranchOut B = RunBranch(RB, 1.0f, Input);
        // Params (full precision, no truncation).
        {
            std::ofstream P(Out + "/params.json");
            Require(bool(P), "cannot open params.json");
            P << "{\"rate\":" << kRate << ",\"block\":" << kBlock
              << ",\"hist_blocks\":" << kHistBlocks << ",\"post_blocks\":" << kPostBlocks
              << ",\"generation\":" << kGeneration
              << ",\"tone_amp\":" << Fmt17(kToneAmp)
              << ",\"freqs\":[" << Fmt17(kFreqs[0]) << "," << Fmt17(kFreqs[1]) << "," << Fmt17(kFreqs[2]) << "]"
              << ",\"tone_phase\":\"block-periodic-loop-1024\",\"input_note\":\"artificial-W1-multitone-not-fitted-to-655B\""
              << ",\"routes_pre\":" << kRoutesPre << ",\"routes_post\":" << kRoutesPost
              << ",\"hist_occ_a\":0.0,\"hist_occ_b\":1.0,\"post_occ\":0.0"
              << ",\"direct_flags\":" << kDirectFlags
              << ",\"direct_flags_note\":\"APPLYOCCLUSION-only-bit3-not-production-fullset\""
              << ",\"direct\":{\"flags\":" << kDirectFlags << ",\"distanceAttenuation\":1.0,\"airAbsorption\":[0.0,0.0,0.0],\"directivity\":1.0,\"transmission\":[0.0,0.0,0.0]}"
              << ",\"listener_origin\":[2.0,0.0,0.0],\"listener_dir\":[0.0,0.0,-1.0]"
              << ",\"path_eq\":[1.0,1.0,1.0],\"path_sh\":[1.0,0.0,0.0,0.0]"
              << ",\"w03_samples\":" << kW03Samples << ",\"post_samples\":" << (std::uint64_t(kPostBlocks) * kBlock)
              << ",\"quant\":\"lround-half-away *32767 clamp-int16\",\"layout\":\"f32-interleaved-stereo-post-and-pre-per-branch\"}";
            P.close();
            Require(bool(P), "cannot write params.json");
        }
        // Per-block records (full precision energies).
        for (int Br = 0; Br < 2; ++Br)
        {
            const BranchOut& O = Br ? B : A;
            std::ofstream C(Out + (Br ? "/B_blocks.csv" : "/A_blocks.csv"));
            Require(bool(C), "cannot open blocks.csv");
            C << "block,seq,occ,flags,routes,ok,fail,input_e,direct_e,path_e\n";
            char Line[320];
            for (const auto& R : O.Recs)
            {
                std::snprintf(Line, sizeof(Line), "%d,%llu,%.17g,%u,%u,%d,%d,%.17g,%.17g,%.17g\n",
                    R.Block, R.Seq, double(R.Occ), R.Flags, R.Routes, R.Ok, R.Fail, R.InputE, R.DirectE, R.PathE);
                C << Line;
            }
            C.close();
            Require(bool(C), "cannot write blocks.csv");
        }
        WriteF32(Out + "/A_pre_stereo.f32", A.PreStereo);
        WriteF32(Out + "/A_pre_direct.f32", A.PreDirect);
        WriteF32(Out + "/A_pre_path.f32", A.PrePath);
        WriteF32(Out + "/A_post_stereo.f32", A.PostStereo);
        WriteF32(Out + "/A_post_direct.f32", A.PostDirect);
        WriteF32(Out + "/A_post_path.f32", A.PostPath);
        WriteF32(Out + "/B_pre_stereo.f32", B.PreStereo);
        WriteF32(Out + "/B_pre_direct.f32", B.PreDirect);
        WriteF32(Out + "/B_pre_path.f32", B.PrePath);
        WriteF32(Out + "/B_post_stereo.f32", B.PostStereo);
        WriteF32(Out + "/B_post_direct.f32", B.PostDirect);
        WriteF32(Out + "/B_post_path.f32", B.PostPath);
        // PCM16 post-switch outputs + last-nonzero positions (exact integers).
        for (int Br = 0; Br < 2; ++Br)
        {
            const std::vector<float>& S = Br ? B.PostStereo : A.PostStereo;
            std::vector<std::int16_t> Q(S.size());
            for (std::size_t I = 0; I < S.size(); ++I) Q[I] = Quantize(S[I]);
            std::ofstream F(Out + (Br ? "/B_post.pcm16" : "/A_post.pcm16"), std::ios::binary);
            Require(bool(F), "cannot open pcm16");
            F.write(reinterpret_cast<const char*>(Q.data()), std::streamsize(Q.size() * sizeof(std::int16_t)));
            F.close();
            Require(bool(F), "cannot write pcm16");
            long long LastL = -1, LastR = -1, Nnz = 0;
            double E = 0.0;
            for (std::size_t I = 0; I < Q.size(); I += 2)
            {
                if (Q[I] != 0) { LastL = long long(I / 2); ++Nnz; E += double(Q[I]) * Q[I]; }
                if (Q[I + 1] != 0) { LastR = long long(I / 2); ++Nnz; E += double(Q[I + 1]) * Q[I + 1]; }
            }
            std::ofstream J(Out + (Br ? "/B_pcm16.json" : "/A_pcm16.json"));
            Require(bool(J), "cannot open pcm16.json");
            J << "{\"samples\":" << (Q.size() / 2) << ",\"pcm16_energy\":" << Fmt17(E)
              << ",\"nonzero_count\":" << Nnz << ",\"last_nonzero_l\":" << LastL << ",\"last_nonzero_r\":" << LastR << "}";
            J.close();
            Require(bool(J), "cannot write pcm16.json");
        }
        // Float last-nonzero positions over post-switch output (exact).
        for (int Br = 0; Br < 2; ++Br)
        {
            const std::vector<float>& S = Br ? B.PostStereo : A.PostStereo;
            long long LastL = -1, LastR = -1, Nnz = 0;
            for (std::size_t I = 0; I < S.size(); I += 2)
            {
                if (S[I] != 0.0f) { LastL = long long(I / 2); ++Nnz; }
                if (S[I + 1] != 0.0f) { LastR = long long(I / 2); ++Nnz; }
            }
            std::ofstream J(Out + (Br ? "/B_float_tail.json" : "/A_float_tail.json"));
            Require(bool(J), "cannot open float_tail.json");
            J << "{\"post_samples\":" << (S.size() / 2) << ",\"nonzero_count\":" << Nnz
              << ",\"last_nonzero_l\":" << LastL << ",\"last_nonzero_r\":" << LastR << "}";
            J.close();
            Require(bool(J), "cannot write float_tail.json");
        }
        std::cout << "D-AB done blocks=" << kTotalBlocks << " post_samples=" << (kPostBlocks * kBlock) << std::endl;
        return 0;
    } catch (const std::exception& E) {
        std::cerr << "D-AB fatal: " << E.what() << std::endl;
        return 2;
    }
}

// Legacy 2026-09-08 bake/smoke body retained below, compiled out for D-unit.
// D-unit compiles only IMAcousticAudioRenderer.cpp + IMAcousticSDKContext.cpp.
#if 0
// Preserve the actual SDK crash site in unattended test evidence, without opening
// a debugger window or converting an access violation into a successful test.
LONG WINAPI IMCrashEvidence(EXCEPTION_POINTERS* Exception)
{
    HANDLE Process=GetCurrentProcess();
    SymInitialize(Process,nullptr,TRUE);
    CONTEXT Context=*Exception->ContextRecord;
    std::cerr<<"fault_rip=0x"<<std::hex<<Context.Rip<<" rsp=0x"<<Context.Rsp<<std::dec<<std::endl;
    // A null function call has already pushed its return address. Start unwind at
    // that caller so a null SDK callback does not erase the diagnostic stack.
    if(Context.Rip==0) { Context.Rip=*reinterpret_cast<DWORD64*>(Context.Rsp);Context.Rsp+=8; }
    STACKFRAME64 Frame{};
    Frame.AddrPC={Context.Rip,0,AddrModeFlat};
    Frame.AddrFrame={Context.Rbp,0,AddrModeFlat};
    Frame.AddrStack={Context.Rsp,0,AddrModeFlat};
    std::cerr<<"exception 0x"<<std::hex<<Exception->ExceptionRecord->ExceptionCode<<std::dec<<std::endl;
    for(int I=0;I<24 && Frame.AddrPC.Offset;++I) {
        char Storage[sizeof(SYMBOL_INFO)+MAX_SYM_NAME]{};
        auto* Symbol=reinterpret_cast<SYMBOL_INFO*>(Storage);
        Symbol->SizeOfStruct=sizeof(SYMBOL_INFO);Symbol->MaxNameLen=MAX_SYM_NAME;
        DWORD64 Displacement=0;
        const DWORD64 Base=SymGetModuleBase64(Process,Frame.AddrPC.Offset);
        std::cerr<<"pc=0x"<<std::hex<<Frame.AddrPC.Offset<<" module_offset=0x"<<Frame.AddrPC.Offset-Base<<std::dec;
        if(SymFromAddr(Process,Frame.AddrPC.Offset,&Displacement,Symbol))std::cerr<<" "<<Symbol->Name<<"+"<<Displacement;
        IMAGEHLP_LINE64 Line{};Line.SizeOfStruct=sizeof(Line);DWORD Delta=0;
        if(SymGetLineFromAddr64(Process,Frame.AddrPC.Offset,&Delta,&Line))std::cerr<<" "<<Line.FileName<<":"<<Line.LineNumber;
        std::cerr<<std::endl;
        if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,Process,GetCurrentThread(),&Frame,&Context,nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr))break;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

namespace {
constexpr int Rate = 48000;
constexpr int Block = 512;
constexpr int Blocks = 188; // 2.005s, exact SDK blocks.
void Require(bool Value, const std::string& Error) { if (!Value) throw std::runtime_error(Error); }
void Box(IM_AcousticSceneInput& S, IPLVector3 Lo, IPLVector3 Hi)
{
    const int Base = static_cast<int>(S.Vertices.size());
    for (int I = 0; I < 8; ++I)
        S.Vertices.push_back({I&1 ? Hi.x : Lo.x, I&2 ? Hi.y : Lo.y, I&4 ? Hi.z : Lo.z});
    const int Faces[12][3] = {{0,2,3},{0,3,1},{4,5,7},{4,7,6},
        {0,1,5},{0,5,4},{2,6,7},{2,7,3},{0,4,6},{0,6,2},{1,3,7},{1,7,5}};
    for (const auto& F : Faces) { S.Triangles.push_back({{Base+F[0],Base+F[1],Base+F[2]}}); S.MaterialIndices.push_back(0); }
}
IM_AcousticSceneInput Scene(bool Open)
{
    IM_AcousticSceneInput S;
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
        // One off-axis doorway: source/listener z=-1.5, aperture z=0.5..2.5.
        Box(S,{-.05f,0,-3},{.05f,3,.5f});
        Box(S,{-.05f,0,2.5f},{.05f,3,3});
        Box(S,{-.05f,2.5f,.5f},{.05f,3,2.5f});
    } else Box(S,{-.05f,0,-3},{.05f,3,3});
    for (float X=-3.5f;X<4;X+=1) for(float Z=-2.5f;Z<3;Z+=1)
        S.Probes.push_back({{X,1.5f,Z},.85f});
    return S;
}
IPLCoordinateSpace3 Space(float X,float Z)
{
    return {{1,0,0},{0,1,0},{0,0,-1},{X,1.5f,Z}};
}
double Energy(const std::vector<float>& V)
{
    double E=0; for(float F:V) { Require(std::isfinite(F),"non-finite output"); E+=F*F; } return E;
}
void Wave(const std::filesystem::path& P,const std::vector<float>& V)
{
    std::ofstream F(P,std::ios::binary);
    auto U16=[&](std::uint16_t N){F.put(N&255);F.put((N>>8)&255);};
    auto U32=[&](std::uint32_t N){for(int I=0;I<4;++I)F.put((N>>(I*8))&255);};
    F.write("RIFF",4);U32(36+static_cast<uint32_t>(V.size()*4));F.write("WAVEfmt ",8);
    U32(16);U16(3);U16(2);U32(Rate);U32(Rate*8);U16(8);U16(32);
    F.write("data",4);U32(static_cast<uint32_t>(V.size()*4));F.write(reinterpret_cast<const char*>(V.data()),V.size()*4);
    Require(F.good(),"wave write failed");
}
struct Result { double Direct=0,Path=0; float Occlusion=0; bool PathValid=false; double EarBalance=0; };
Result Render(IM_AcousticAudioRenderer& Renderer,const IM_AcousticAudioFrame& Frame,
    const std::filesystem::path& Dir,const std::string& Name)
{
    Renderer.Reset();
    std::vector<float> Dry(Block), Out(Block*2), D(Block*2), P(Block*2), All, Direct, Path;
    for(int B=0;B<Blocks;++B) {
        for(int I=0;I<Block;++I) {
            const double T=double(B*Block+I)/Rate;
            // Fixed broadband multitone, same input and gain across every route/case.
            Dry[I]=float(.08*(std::sin(2*3.141592653589793*233*T)+std::sin(2*3.141592653589793*997*T)+std::sin(2*3.141592653589793*3109*T)));
        }
        Require(Renderer.Render(Dry.data(),Block,Frame,Out.data(),D.data(),P.data()),"production Render rejected frame");
        All.insert(All.end(),Out.begin(),Out.end());Direct.insert(Direct.end(),D.begin(),D.end());Path.insert(Path.end(),P.begin(),P.end());
    }
    Wave(Dir/(Name+"-sum.wav"),All);Wave(Dir/(Name+"-direct.wav"),Direct);Wave(Dir/(Name+"-path.wav"),Path);
    double Left=0,Right=0;
    for(size_t I=0;I<Path.size();I+=2) { Left+=Path[I]*Path[I];Right+=Path[I+1]*Path[I+1]; }
    return {Energy(Direct),Energy(Path),Frame.Direct.occlusion,Frame.PathValid,
        (Left+Right)>0?(Left-Right)/(Left+Right):0};
}
}
int main(int Argc,char** Argv)
{
    SetUnhandledExceptionFilter(IMCrashEvidence);
    IPLContext Context=nullptr;IPLHRTF HRTF=nullptr;
    try {
        Require(Argc==2||Argc==3,"usage: smoke evidence-directory [UE-bake-directory]");
        const std::filesystem::path Dir=Argv[1];std::filesystem::create_directories(Dir);
        Require(IM_GetAcousticSDKContext()!=nullptr,"context create");
        Context=iplContextRetain(IM_GetAcousticSDKContext());
        IPLAudioSettings A{Rate,Block};IPLHRTFSettings H{};H.type=IPL_HRTFTYPE_DEFAULT;H.volume=1;
        Require(iplHRTFCreate(Context,&A,&H,&HRTF)==IPL_STATUS_SUCCESS,"hrtf create");
        IM_AcousticAudioRenderer Renderer;
        Require(Renderer.Initialize(Context,HRTF,Rate,Block),"renderer initialize");
        if(Argc==3)
        {
            IM_AcousticBakeData Bake;
            auto Read=[&](const char* Name,std::vector<uint8_t>& Bytes)
            {
                std::ifstream F(std::filesystem::path(Argv[2])/Name,std::ios::binary);
                Require(F.good(),"UE bake file unavailable");
                Bytes.assign(std::istreambuf_iterator<char>(F),std::istreambuf_iterator<char>());
            };
            Read("scene.bin",Bake.Scene);Read("probes.bin",Bake.ProbeBatch);
            IM_AcousticSimulation Sim;std::string Error;
            Require(Sim.Load(Bake,Rate,Block,Error),Error);
            auto Source=Space(-2,0),Listener=Space(2,0);
            Source.origin.y=Listener.origin.y=0; // UE bake origin is floor+1.5m.
            Listener.ahead={-1,0,0};Listener.right={0,0,-1};
            IM_AcousticAudioFrame Frame;
            Require(Sim.Evaluate(1,1,Source,Listener,Frame,Error),Error);
            const Result R=Render(Renderer,Frame,Dir,"ue-bake");
            std::ofstream(Dir/"ue-bake-result.json")<<"{\"direct_energy\":"<<R.Direct<<",\"path_energy\":"<<R.Path<<",\"occlusion\":"<<R.Occlusion<<",\"path_valid\":"<<(R.PathValid?"true":"false")<<"}";
            Renderer.Reset();
            std::vector<float> Dry(Block),Mixed(Block*2),Direct(Block*2),Path(Block*2),Motion;
            std::ofstream Trace(Dir/"moving-pair.csv");Trace<<"time_s,source_x,source_z,listener_x,listener_z,occlusion,path_valid,sequence\n";
            bool MotionPass=true;double DirectEnergy=0,PathEnergy=0,MaxAdjacentJump=0;float Previous=0;
            constexpr int MotionBlocks=480; // 5.12 seconds at 48kHz/512 samples.
            for(int B=0;B<MotionBlocks;++B)
            {
                const double T=double(B*Block)/Rate;
                if(B%5==0) // 18.75Hz, approximately the runtime's 20Hz cadence.
                {
                    Source.origin.x=float(-2+.25*std::sin(T));Source.origin.z=float(.25*std::sin(T*.7));
                    Listener.origin.x=float(2+.25*std::cos(T));Listener.origin.z=float(.25*std::cos(T*.9));
                    Require(Sim.Evaluate(1,1,Source,Listener,Frame,Error),Error);
                    MotionPass=MotionPass&&Frame.PathValid&&Frame.Direct.occlusion<.01f;
                    Trace<<T<<','<<Source.origin.x<<','<<Source.origin.z<<','<<Listener.origin.x<<','<<Listener.origin.z<<','<<Frame.Direct.occlusion<<','<<Frame.PathValid<<','<<Frame.Sequence<<'\n';
                }
                for(int I=0;I<Block;++I)Dry[I]=float(.08*std::sin(2*3.141592653589793*233*(T+double(I)/Rate)));
                Require(Renderer.Render(Dry.data(),Block,Frame,Mixed.data(),Direct.data(),Path.data()),"moving pair renderer failed");
                DirectEnergy+=Energy(Direct);PathEnergy+=Energy(Path);
                for(int I=0;I<Block;++I){MaxAdjacentJump=(std::max)(MaxAdjacentJump,std::abs(double(Mixed[2*I]-Previous)));Previous=Mixed[2*I];}
                Motion.insert(Motion.end(),Mixed.begin(),Mixed.end());
            }
            Wave(Dir/"moving-pair.wav",Motion);
            MotionPass=MotionPass&&DirectEnergy<1e-6&&PathEnergy>1e-6;
            std::ofstream(Dir/"moving-pair-result.json")<<"{\"scope\":\"actual-UE-bake-native-moving-pair\",\"direct_energy\":"<<DirectEnergy<<",\"path_energy\":"<<PathEnergy<<",\"max_adjacent_sample_jump\":"<<MaxAdjacentJump<<",\"pass\":"<<(MotionPass?"true":"false")<<"}";
            Renderer.Shutdown();iplHRTFRelease(&HRTF);iplContextRelease(&Context);
            return R.Direct<1e-6&&R.Path>1e-6&&MotionPass?0:2;
        }
        {
            IM_AcousticSimulation Sim;auto Geometry=Scene(true);Geometry.Probes.clear();
            IPLProbeGenerationParams Params{};Params.type=IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
            Params.spacing=1;Params.height=1.5f;
            Params.transform.elements[0][0]=8;
            Params.transform.elements[1][1]=3;Params.transform.elements[1][3]=1.5f;
            Params.transform.elements[2][2]=6;
            Params.transform.elements[3][3]=1;
            std::vector<IPLSphere> Probes;std::string Error;
            Require(Sim.GenerateProbes(Geometry,Params,Probes,Error),Error);
            Require(!Probes.empty(),"SDK generated no probes");
            std::ofstream Positions(Dir/"generated-probes.csv");Positions<<"x_m,y_m,z_m,radius_m\n";
            for(const auto& P:Probes)Positions<<P.center.x<<','<<P.center.y<<','<<P.center.z<<','<<P.radius<<'\n';
            std::cout<<"generated_probes="<<Probes.size()<<std::endl;
            // A nonempty set is insufficient: the prior [0,1] matrix interpretation
            // generated half a room. Require actual connectivity after SDK generation.
            Geometry.Probes=Probes;IM_AcousticBakeData GeneratedBake;
            Require(Sim.Bake(Geometry,GeneratedBake,Error),Error);
            Require(Sim.Load(GeneratedBake,Rate,Block,Error),Error);
            IM_AcousticAudioFrame GeneratedFrame;
            Require(Sim.Evaluate(1,1,Space(-2,0),Space(2,0),GeneratedFrame,Error),Error);
            Require(GeneratedFrame.PathValid&&GeneratedFrame.Direct.occlusion<.01f,"generated probes failed open-door path coverage");
            IM_AcousticReverbSlot ReverbA,ReverbB;
            ReverbA.State.store(IM_AcousticIRState::Writing);ReverbB.State.store(IM_AcousticIRState::Writing);
            Require(Sim.EvaluateReverb(ReverbA,Space(2,0),Error),Error);
            IM_AcousticReverbRenderer Reverb;
            Require(Reverb.Initialize(Context,HRTF,Rate,Block,int(Rate*IM_AcousticRecipe::ReverbSavedDurationS)),"reverb renderer init");
            std::vector<float> Impulse(Block,0),Wet(Block*2),WetAll;
            for(int B=0;B<64;++B)
            {
                std::fill(Impulse.begin(),Impulse.end(),0);if(B==0)Impulse[0]=.5f;
                // Other simulation writes must not invalidate detached A's source/IR.
                if(B<32&&B%8==0)Require(Sim.EvaluateReverb(ReverbB,Space(-2,0),Error),Error);
                if(B==32)Sim.Shutdown(); // Detached leases and audio may outlive worker simulator shutdown.
                Require(Reverb.Render(Impulse.data(),Block,ReverbA.Params,ReverbA.Listener,Wet.data()),"frozen reverb render rejected");
                WetAll.insert(WetAll.end(),Wet.begin(),Wet.end());
            }
            Wave(Dir/"generated-probes-reverb.wav",WetAll);
            const double WetEnergy=Energy(WetAll);
            Require(WetEnergy>1e-12,"baked convolution emitted no reverb");
            std::ofstream(Dir/"reverb-result.json")<<"{\"scope\":\"native-baked-convolution-detached-source\",\"wet_energy\":"<<WetEnergy<<",\"pass\":true}";
            IM_AcousticBakeData Existing;Existing.Scene={1,2};Existing.ProbeBatch={3,4};
            std::atomic<bool> Cancelled{true};
            Require(!Sim.Bake(Scene(true),Existing,Error,&Cancelled),"cancelled bake returned success");
            Require(Existing.Scene==std::vector<uint8_t>({1,2})&&Existing.ProbeBatch==std::vector<uint8_t>({3,4}),"cancelled bake overwrote previous bytes");
            auto Closed=Scene(false);Closed.Probes.clear();
            Require(Sim.GenerateProbes(Closed,Params,Closed.Probes,Error),Error);
            IM_AcousticBakeData ClosedBake;
            Require(Sim.Bake(Closed,ClosedBake,Error),Error);Require(Sim.Load(ClosedBake,Rate,Block,Error),Error);
            IM_AcousticAudioFrame ClosedFrame;
            Require(Sim.Evaluate(1,1,Space(-2,0),Space(2,0),ClosedFrame,Error),Error);
            Require(!ClosedFrame.PathValid&&ClosedFrame.Direct.occlusion<.01f,"SDK-generated probes created a through-wall path in sealed room");
        }
        std::ofstream Report(Dir/"results.json");Report<<"{\"scope\":\"native-production-kernel-and-sdk-only\",\"cases\":[";
        bool Passed=true;
        for(int Open=0;Open<2;++Open) {
            const std::string Name=Open?"open-door":"closed-wall";
            std::cout<<"bake "<<Name<<std::endl;
            IM_AcousticSimulation Sim;IM_AcousticBakeData Bake;std::string Error;
            Require(Sim.Bake(Scene(Open!=0),Bake,Error),Error);
            std::ofstream(Dir/(Name+".scene"),std::ios::binary).write(reinterpret_cast<const char*>(Bake.Scene.data()),Bake.Scene.size());
            std::ofstream(Dir/(Name+".probes"),std::ios::binary).write(reinterpret_cast<const char*>(Bake.ProbeBatch.data()),Bake.ProbeBatch.size());
            Require(Sim.Load(Bake,Rate,Block,Error),Error);
            IM_AcousticAudioFrame Frame;
            Require(Sim.Evaluate(1,1,Space(-2,-1.5f),Space(2,-1.5f),Frame,Error),Error);
            const Result R=Render(Renderer,Frame,Dir,Name);
            const bool CasePass=R.Occlusion<.01f && R.Direct<1e-6 && (Open?(R.PathValid && R.Path>1e-6):(!R.PathValid && R.Path<1e-6));
            Passed=Passed&&CasePass;
            if(Open)Report<<',';
            Report<<"{\"name\":\""<<Name<<"\",\"direct_energy\":"<<R.Direct<<",\"path_energy\":"<<R.Path<<",\"occlusion\":"<<R.Occlusion<<",\"path_valid\":"<<(R.PathValid?"true":"false")<<",\"pass\":"<<(CasePass?"true":"false")<<"}";
            std::cout<<Name<<" direct="<<R.Direct<<" path="<<R.Path<<" occlusion="<<R.Occlusion<<" pass="<<CasePass<<std::endl;
        }
        for(int Mirror=0;Mirror<2;++Mirror) {
            const std::string Name=Mirror?"door-right":"door-left";
            auto Input=Scene(true);
            if(Mirror) {
                for(auto& V:Input.Vertices)V.z=-V.z;
                for(auto& P:Input.Probes)P.center.z=-P.center.z;
                for(auto& T:Input.Triangles)std::swap(T.indices[1],T.indices[2]);
            }
            IM_AcousticSimulation Sim;IM_AcousticBakeData Bake;std::string Error;
            Require(Sim.Bake(Input,Bake,Error),Error);Require(Sim.Load(Bake,Rate,Block,Error),Error);
            auto Listener=Space(2,0);Listener.ahead={-1,0,0};Listener.right={0,0,-1};
            IM_AcousticAudioFrame Frame;
            Require(Sim.Evaluate(1,1,Space(-2,0),Listener,Frame,Error),Error);
            const Result R=Render(Renderer,Frame,Dir,Name);
            // Mirror only the doorway. Source stays directly ahead and occluded;
            // the audible lateral cue must follow the aperture, not that source.
            const bool CasePass=R.Occlusion<.01f && R.Direct<1e-6 && R.PathValid && R.Path>1e-6
                && (Mirror?R.EarBalance<-.02:R.EarBalance>.02);
            Passed=Passed&&CasePass;
            Report<<",{\"name\":\""<<Name<<"\",\"ear_balance\":"<<R.EarBalance
                <<",\"path_energy\":"<<R.Path<<",\"pass\":"<<(CasePass?"true":"false")<<"}";
            std::cout<<Name<<" ear_balance="<<R.EarBalance<<" pass="<<CasePass<<std::endl;
        }
        Report<<"],\"pass\":"<<(Passed?"true":"false")<<",\"ue_audio_gate\":\"NOT_RUN\",\"reverb_render\":\"NATIVE_ONLY\"}\n";
        Renderer.Shutdown();iplHRTFRelease(&HRTF);iplContextRelease(&Context);
        return Passed?0:2;
    } catch(const std::exception& E) {
        std::cerr<<"FAIL "<<E.what()<<std::endl;
        if(HRTF)iplHRTFRelease(&HRTF);if(Context)iplContextRelease(&Context);return 1;
    }
}
#endif // legacy 2026-09-08 bake/smoke body, D-unit excluded
