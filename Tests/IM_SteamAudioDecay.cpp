// Native SDK decay-counterexample fixture (W2 falsification probe).
//
// Reuses production IM_AcousticSimulation / IM_AcousticReverbRenderer / shared
// SDK context / IM_AcousticReverbData (header-only) with the same native-cl
// compile method as Tests/IM_SteamAudioSmoke.cpp. No UE, no UBT, no Editor.
// No SDK API is invented here; every Steam Audio call mirrors the production
// .cpp or the smoke fixture. No UE types are used, so no native fake headers
// are needed.
//
// Four fixed cases under the current production recipe (dimensions read from
// IMAcousticBakeRecipe.h at compile time; Bake() owns the parameters, this
// file only reports them and never overrides):
//   closed-low  8x3x6 m sealed room, all-material absorption 0.1 per band
//   closed-high same sealed room,    all-material absorption 0.8 per band
//   corridor    20x3x2 m sealed duct, absorption 0.1 per band
//   outdoor     20x3x20 m walled floor without ceiling, absorption 0.1
// Only the two closed-room runs differ, and only in absorption. Scattering 0.5,
// transmission 0, probe generation (UNIFORMFLOOR, 1 m spacing, 1.5 m height),
// impulse gain, and render length are fixed everywhere.
//
// Per case: SDK auto GenerateProbes -> Bake -> Load -> EvaluateReverb, then the
// production reverb renderer turns one 0.5-amplitude impulse plus zeros into a
// stereo WAV (full saved IR plus 0.5 s silence, rounded up to whole 512-sample
// blocks at 48 kHz). Analysis records per-10 ms energy, the Schroeder EDC, the
// T20 slope over the -5..-25 dB interval with
// fit sample count, total energy, and the last time above peak-60 dB. When the
// -5/-25 dB interval does not exist, t20_s is null and no RT60 is fabricated.
//
// Exit codes: 0 = harness complete, every fit usable, and low-T20 exceeds
// high-T20 by at least 10% (robustness gate against Monte-Carlo noise; the raw
// gap is always reported, so tightening never hides data); 2 = harness complete
// but counterexample observed (any fit unusable or low/high indistinguishable);
// 1 = harness/SDK error. Exit 0 is harness success only; it never claims UE
// routing, recording, or audition PASS.
#include "IMAcousticSimulation.h"
#include "IMAcousticBakeRecipe.h"
#include "IMAcousticReverbRenderer.h"
#include "IMAcousticReverbData.h"
#include "IMAcousticSDKContext.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#define NOMINMAX
#include <Windows.h>
#include <DbgHelp.h>

// Same unattended-crash evidence helper as the smoke fixture: preserve the
// fault site without opening a debugger or converting a crash into a pass.
LONG WINAPI IMCrashEvidence(EXCEPTION_POINTERS* Exception)
{
    HANDLE Process=GetCurrentProcess();
    SymInitialize(Process,nullptr,TRUE);
    CONTEXT Context=*Exception->ContextRecord;
    std::cerr<<"fault_rip=0x"<<std::hex<<Context.Rip<<" rsp=0x"<<Context.Rsp<<std::dec<<std::endl;
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
// Render length derives from the live recipe: full saved IR plus 0.5 s of
// settled tail, rounded up to whole blocks. No baked tail is cut.
constexpr int IRSizeSamples = static_cast<int>(Rate * IM_AcousticRecipe::ReverbSavedDurationS + 0.5f);
constexpr int TailSilenceSamples = Rate / 2;
constexpr int RenderBlocks = (IRSizeSamples + TailSilenceSamples + Block - 1) / Block;
constexpr float ImpulseAmp = 0.5f; // same gain as the smoke reverb check.
constexpr int WindowFrames = 480; // 10 ms at 48 kHz.
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
void OneMaterial(IM_AcousticSceneInput& S, float Absorption)
{
    IPLMaterial M{};
    for (int B = 0; B < IPL_NUM_BANDS; ++B) { M.absorption[B] = Absorption; M.transmission[B] = 0.0f; }
    M.scattering = 0.5f;
    S.Materials.push_back(M);
}
// Sealed 8x3x6 m room interior X[-4,4] Y[0,3] Z[-3,3], wall thickness 0.1.
IM_AcousticSceneInput ClosedScene(float Absorption)
{
    IM_AcousticSceneInput S; OneMaterial(S, Absorption);
    Box(S,{-4.1f,-.1f,-3.1f},{4.1f,0,3.1f});
    Box(S,{-4.1f,3,-3.1f},{4.1f,3.1f,3.1f});
    Box(S,{-4.1f,0,-3.1f},{-4,3,3.1f});
    Box(S,{4,0,-3.1f},{4.1f,3,3.1f});
    Box(S,{-4,0,-3.1f},{4,3,-3});
    Box(S,{-4,0,3},{4,3,3.1f});
    return S;
}
// Sealed 20x3x2 m duct interior X[-10,10] Y[0,3] Z[-1,1], thickness 0.1.
IM_AcousticSceneInput CorridorScene(float Absorption)
{
    IM_AcousticSceneInput S; OneMaterial(S, Absorption);
    Box(S,{-10.1f,-.1f,-1.1f},{10.1f,0,1.1f});
    Box(S,{-10.1f,3,-1.1f},{10.1f,3.1f,1.1f});
    Box(S,{-10.1f,0,-1.1f},{-10,3,1.1f});
    Box(S,{10,0,-1.1f},{10.1f,3,1.1f});
    Box(S,{-10,0,-1.1f},{10,3,-1});
    Box(S,{-10,0,1},{10,3,1.1f});
    return S;
}
// Walled 20x3x20 m floor without ceiling: interior X[-10,10] Z[-10,10].
IM_AcousticSceneInput OutdoorScene(float Absorption)
{
    IM_AcousticSceneInput S; OneMaterial(S, Absorption);
    Box(S,{-10.1f,-.1f,-10.1f},{10.1f,0,10.1f});
    Box(S,{-10.1f,0,-10.1f},{-10,3,10.1f});
    Box(S,{10,0,-10.1f},{10.1f,3,10.1f});
    Box(S,{-10,0,-10.1f},{10,3,-10});
    Box(S,{-10,0,10},{10,3,10.1f});
    return S;
}
IPLCoordinateSpace3 ListenerAt(float X, float Z)
{
    IPLCoordinateSpace3 L{};
    L.right = {0,0,-1}; L.up = {0,1,0}; L.ahead = {-1,0,0};
    L.origin = {X,1.5f,Z};
    return L;
}
void Wave(const std::filesystem::path& P, const std::vector<float>& V)
{
    std::ofstream F(P, std::ios::binary);
    auto U16=[&](std::uint16_t N){F.put(N&255);F.put((N>>8)&255);};
    auto U32=[&](std::uint32_t N){for(int I=0;I<4;++I)F.put((N>>(I*8))&255);};
    F.write("RIFF",4);U32(36+static_cast<uint32_t>(V.size()*4));F.write("WAVEfmt ",8);
    U32(16);U16(3);U16(2);U32(Rate);U32(Rate*8);U16(8);U16(32);
    F.write("data",4);U32(static_cast<uint32_t>(V.size()*4));F.write(reinterpret_cast<const char*>(V.data()),V.size()*4);
    Require(F.good(),"wave write failed");
}
struct DecayMetrics
{
    double TotalEnergy = 0;
    bool HasFit = false;
    int FitSamples = 0;
    int StartIdx = -1;
    int EndIdx = -1;
    double SlopeDbPerS = 0; // valid only when HasFit.
    double T20S = 0;        // valid only when HasFit.
    bool HasLast60 = false;
    double LastAboveMinus60S = 0;
    std::vector<double> WindowEnergy;
    std::vector<double> WindowEdcDb;
};
DecayMetrics Analyze(const std::vector<float>& Stereo)
{
    DecayMetrics M;
    Require(Stereo.size() % 2 == 0, "stereo buffer not interleaved pairs");
    const int N = static_cast<int>(Stereo.size() / 2);
    Require(N > 0, "empty render");
    std::vector<double> E(static_cast<size_t>(N), 0);
    for (int I = 0; I < N; ++I) {
        const double L = Stereo[2*I], R = Stereo[2*I+1];
        Require(std::isfinite(L) && std::isfinite(R), "non-finite renderer output");
        E[static_cast<size_t>(I)] = L*L + R*R;
    }
    double Total = 0; for (double V : E) Total += V;
    M.TotalEnergy = Total;
    if (!(Total > 0) || !std::isfinite(Total)) return M;
    std::vector<double> Edc(static_cast<size_t>(N), 0);
    double Suffix = 0;
    for (int I = N - 1; I >= 0; --I) {
        Suffix += E[static_cast<size_t>(I)];
        const double Ratio = Suffix / Total;
        Edc[static_cast<size_t>(I)] = 10 * std::log10((std::max)(Ratio, 1e-30));
    }
    int Start = -1, End = -1, Last60 = -1;
    for (int I = 0; I < N; ++I) {
        if (Start < 0 && Edc[static_cast<size_t>(I)] <= -5) Start = I;
        if (End < 0 && Edc[static_cast<size_t>(I)] <= -25) End = I;
        if (Edc[static_cast<size_t>(I)] >= -60) Last60 = I;
    }
    M.StartIdx = Start; M.EndIdx = End;
    if (Last60 >= 0) { M.HasLast60 = true; M.LastAboveMinus60S = double(Last60) / Rate; }
    const int NW = (N + WindowFrames - 1) / WindowFrames;
    M.WindowEnergy.assign(static_cast<size_t>(NW), 0);
    M.WindowEdcDb.assign(static_cast<size_t>(NW), 0);
    for (int W = 0; W < NW; ++W) {
        double WE = 0;
        for (int I = W * WindowFrames; I < (std::min)((W+1)*WindowFrames, N); ++I)
            WE += E[static_cast<size_t>(I)];
        M.WindowEnergy[static_cast<size_t>(W)] = WE;
        M.WindowEdcDb[static_cast<size_t>(W)] = Edc[static_cast<size_t>(W*WindowFrames)];
    }
    // T20 exists only when the -5..-25 dB interval exists. No interval means
    // no T20: the caller writes null instead of fabricating an RT60.
    if (Start >= 0 && End > Start) {
        const int Count = End - Start + 1;
        double SumT = 0, SumD = 0, SumTT = 0, SumTD = 0;
        for (int I = Start; I <= End; ++I) {
            const double T = double(I) / Rate, D = Edc[static_cast<size_t>(I)];
            SumT += T; SumD += D; SumTT += T*T; SumTD += T*D;
        }
        const double Den = Count * SumTT - SumT * SumT;
        if (std::isfinite(Den) && Den != 0) {
            const double Slope = (Count * SumTD - SumT * SumD) / Den;
            if (std::isfinite(Slope) && Slope < 0) {
                M.HasFit = true; M.FitSamples = Count;
                M.SlopeDbPerS = Slope; M.T20S = -60 / Slope;
            }
        }
    }
    return M;
}
struct CaseDef
{
    const char* Name;
    const char* Geometry;
    int Kind; // 0 closed, 1 corridor, 2 outdoor.
    float Absorption;
    float ListenerX;
    float ListenerZ;
    float ExtentX;
    float ExtentZ;
};
}
int main(int Argc, char** Argv)
{
    SetUnhandledExceptionFilter(IMCrashEvidence);
    IPLContext Context = nullptr; IPLHRTF HRTF = nullptr;
    try {
        Require(Argc == 2, "usage: decay evidence-directory");
        const std::filesystem::path Dir = Argv[1]; std::filesystem::create_directories(Dir);
        Require(IM_GetAcousticSDKContext() != nullptr, "shared SDK context unavailable");
        Context = iplContextRetain(IM_GetAcousticSDKContext());
        IPLAudioSettings A{Rate,Block}; IPLHRTFSettings H{}; H.type=IPL_HRTFTYPE_DEFAULT; H.volume=1;
        Require(iplHRTFCreate(Context,&A,&H,&HRTF)==IPL_STATUS_SUCCESS,"hrtf create");
        IM_AcousticReverbRenderer Reverb;
        Require(Reverb.Initialize(Context,HRTF,Rate,Block,IRSizeSamples),"reverb renderer init");
        const CaseDef Cases[4] = {
            {"closed-low","sealed-room-8x3x6",0,0.1f,2,0,8,6},
            {"closed-high","sealed-room-8x3x6",0,0.8f,2,0,8,6},
            {"corridor","sealed-duct-20x3x2",1,0.1f,6,0,20,2},
            {"outdoor-notop","walled-floor-20x20-notop",2,0.1f,2,0,20,20},
        };
        struct Done { std::string Name; DecayMetrics M; int Probes; };
        std::vector<Done> Finished;
        for (const CaseDef& C : Cases) {
            std::cout<<"case "<<C.Name<<std::endl;
            IM_AcousticSceneInput Geo;
            if (C.Kind == 0) Geo = ClosedScene(C.Absorption);
            else if (C.Kind == 1) Geo = CorridorScene(C.Absorption);
            else Geo = OutdoorScene(C.Absorption);
            IPLProbeGenerationParams Params{}; Params.type=IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
            Params.spacing=1; Params.height=1.5f;
            Params.transform.elements[0][0]=C.ExtentX;
            Params.transform.elements[1][1]=3; Params.transform.elements[1][3]=1.5f;
            Params.transform.elements[2][2]=C.ExtentZ;
            Params.transform.elements[3][3]=1;
            IM_AcousticSimulation Sim; std::string Error;
            std::vector<IPLSphere> Probes;
            Require(Sim.GenerateProbes(Geo,Params,Probes,Error),Error);
            Require(!Probes.empty(),"SDK generated no probes");
            {
                std::ofstream P(Dir/(std::string(C.Name)+"-probes.csv"));
                P<<"x_m,y_m,z_m,radius_m\n";
                for (const auto& Pr : Probes) P<<Pr.center.x<<','<<Pr.center.y<<','<<Pr.center.z<<','<<Pr.radius<<'\n';
                Require(P.good(),"probes csv write failed");
            }
            Geo.Probes = Probes;
            IM_AcousticBakeData Bake;
            Require(Sim.Bake(Geo,Bake,Error),Error);
            {
                std::ofstream S(Dir/(std::string(C.Name)+".scene"),std::ios::binary);
                S.write(reinterpret_cast<const char*>(Bake.Scene.data()),Bake.Scene.size());
                std::ofstream B(Dir/(std::string(C.Name)+".probes"),std::ios::binary);
                B.write(reinterpret_cast<const char*>(Bake.ProbeBatch.data()),Bake.ProbeBatch.size());
                Require(S.good() && B.good(),"bake bytes write failed");
            }
            Require(Sim.Load(Bake,Rate,Block,Error),Error);
            const IPLCoordinateSpace3 Listener = ListenerAt(C.ListenerX, C.ListenerZ);
            IM_AcousticReverbSlot Slot;
            Slot.State.store(IM_AcousticIRState::Writing);
            Require(Sim.EvaluateReverb(Slot,Listener,Error),Error);
            Require(Slot.Params.ir != nullptr,"baked convolution IR is null");
            Require(Slot.Params.type == IPL_REFLECTIONEFFECTTYPE_CONVOLUTION,"baked IR is not convolution");
            Require(Slot.Params.numChannels == IM_AcousticAudioFrame::Coefficients,"baked IR channel mismatch");
            Require(Slot.Params.irSize == IRSizeSamples,"baked IR length mismatch vs current recipe");
            Reverb.Reset();
            std::vector<float> Mono(Block,0), Stereo(Block*2,0), All;
            All.reserve(static_cast<size_t>(RenderBlocks)*Block*2);
            for (int B = 0; B < RenderBlocks; ++B) {
                std::fill(Mono.begin(),Mono.end(),0.0f); if (B == 0) Mono[0] = ImpulseAmp;
                Require(Reverb.Render(Mono.data(),Block,Slot.Params,Slot.Listener,Stereo.data()),"production reverb render rejected");
                All.insert(All.end(),Stereo.begin(),Stereo.end());
            }
            Wave(Dir/(std::string(C.Name)+"-impulse.wav"),All);
            const DecayMetrics M = Analyze(All);
            {
                std::ofstream E(Dir/(std::string(C.Name)+"-energy-10ms.csv"));
                E<<"time_s,energy,edc_db\n";
                E<<std::setprecision(10);
                for (size_t W = 0; W < M.WindowEnergy.size(); ++W)
                    E<<(W*0.01)<<','<<M.WindowEnergy[W]<<','<<M.WindowEdcDb[W]<<'\n';
                Require(E.good(),"energy csv write failed");
            }
            {
                std::ofstream J(Dir/(std::string(C.Name)+"-decay.json"));
                J<<std::setprecision(17);
                J<<"{\"name\":\""<<C.Name<<"\",\"geometry\":\""<<C.Geometry<<"\",";
                J<<"\"absorption_3band\":["<<C.Absorption<<','<<C.Absorption<<','<<C.Absorption<<"],";
                J<<"\"scattering\":0.5,\"transmission_3band\":[0,0,0],";
                J<<"\"recipe_version\":"<<IM_AcousticRecipe::Version<<",";
                J<<"\"recipe\":{\"num_rays\":"<<IM_AcousticRecipe::ReverbNumRays<<",\"num_bounces\":"<<IM_AcousticRecipe::ReverbNumBounces<<",\"sim_duration_s\":"<<IM_AcousticRecipe::ReverbSimDurationS<<",\"saved_ir_s\":"<<IM_AcousticRecipe::ReverbSavedDurationS<<"},";
                J<<"\"probe_generation\":{\"type\":\"UNIFORMFLOOR\",\"spacing_m\":1,\"height_m\":1.5},";
                J<<"\"probes_generated\":"<<Probes.size()<<",";
                J<<"\"wav\":\""<<C.Name<<"-impulse.wav\",";
                J<<"\"sample_rate_hz\":"<<Rate<<",\"block_frames\":"<<Block<<",\"render_blocks\":"<<RenderBlocks<<",\"impulse_amplitude\":"<<ImpulseAmp<<",";
                J<<"\"total_energy\":"<<M.TotalEnergy<<",\"window_s\":0.01,";
                J<<"\"last_above_minus60_s\":"<<(M.HasLast60?std::to_string(M.LastAboveMinus60S):std::string("null"))<<",";
                J<<"\"edc_minus5_idx\":"<<(M.StartIdx>=0?std::to_string(M.StartIdx):std::string("null"))<<",";
                J<<"\"edc_minus25_idx\":"<<(M.EndIdx>=0?std::to_string(M.EndIdx):std::string("null"))<<",";
                J<<"\"fit_samples\":"<<M.FitSamples<<",";
                J<<"\"slope_db_per_s\":"<<(M.HasFit?std::to_string(M.SlopeDbPerS):std::string("null"))<<",";
                J<<"\"t20_s\":"<<(M.HasFit?std::to_string(M.T20S):std::string("null"))<<",";
                J<<"\"decay_fit_usable\":"<<(M.HasFit?"true":"false")<<",";
                J<<"\"scope\":\"native-sdk-baked-convolution-impulse\"}\n";
                Require(J.good(),"decay json write failed");
            }
            std::cout<<C.Name<<" probes="<<Probes.size()<<" total_energy="<<M.TotalEnergy
                <<" fit="<<(M.HasFit?"yes":"no")<<" t20_s="<<(M.HasFit?std::to_string(M.T20S):std::string("null"))<<std::endl;
            Finished.push_back({C.Name, M, static_cast<int>(Probes.size())});
            Reverb.Reset();
            Sim.Shutdown();
        }
        const DecayMetrics& Low = Finished[0].M;
        const DecayMetrics& High = Finished[1].M;
        const bool AllUsable = Finished[0].M.HasFit && Finished[1].M.HasFit && Finished[2].M.HasFit && Finished[3].M.HasFit;
        const bool PairUsable = Low.HasFit && High.HasFit;
        const bool LowSlower = PairUsable && (Low.T20S > High.T20S);
        const double GapRel = PairUsable && High.T20S > 0 ? (Low.T20S - High.T20S) / High.T20S : 0;
        // A bare low>high bit is not discrimination: sub-percent gaps on
        // ray-stochastic baked IRs are noise. The 10% gate only tightens.
        const bool Distinguishable = PairUsable && (GapRel >= 0.10);
        const bool Counterexample = !(AllUsable && Distinguishable);
        {
            std::ofstream S(Dir/"summary.json"); S<<std::setprecision(17);
            S<<"{\"scope\":\"native-sdk-decay-counterexample\",";
            S<<"\"recipe_version\":"<<IM_AcousticRecipe::Version<<",";
            S<<"\"recipe\":{\"num_rays\":"<<IM_AcousticRecipe::ReverbNumRays<<",\"num_bounces\":"<<IM_AcousticRecipe::ReverbNumBounces<<",\"sim_duration_s\":"<<IM_AcousticRecipe::ReverbSimDurationS<<",\"saved_ir_s\":"<<IM_AcousticRecipe::ReverbSavedDurationS<<"},";
            S<<"\"fixed_config\":{\"scattering\":0.5,\"transmission\":0,\"probe_spacing_m\":1,\"probe_height_m\":1.5,\"impulse\":0.5,\"render_s\":"<<(double(RenderBlocks*Block)/Rate)<<",";
            S<<"\"cases\":[";
            for (size_t I = 0; I < Finished.size(); ++I) {
                if (I) S<<',';
                S<<"{\"name\":\""<<Finished[I].Name<<"\",\"probes\":"<<Finished[I].Probes<<",\"total_energy\":"<<Finished[I].M.TotalEnergy<<",\"t20_s\":"<<(Finished[I].M.HasFit?std::to_string(Finished[I].M.T20S):std::string("null"))<<",\"fit_samples\":"<<Finished[I].M.FitSamples<<",\"decay_fit_usable\":"<<(Finished[I].M.HasFit?"true":"false")<<"}";
            }
            S<<"],\"verdicts\":{\"all_decay_fit_usable\":"<<(AllUsable?"true":"false")<<",\"low_high_both_usable\":"<<(PairUsable?"true":"false")<<",";
            S<<"\"t20_closed_low_s\":"<<(Low.HasFit?std::to_string(Low.T20S):std::string("null"))<<",\"t20_closed_high_s\":"<<(High.HasFit?std::to_string(High.T20S):std::string("null"))<<",";
            S<<"\"t20_gap_rel\":"<<GapRel<<",\"distinguish_gate_rel\":0.10,";
            S<<"\"low_slower_than_high\":"<<(LowSlower?"true":"false")<<",\"low_high_distinguishable\":"<<(Distinguishable?"true":"false")<<"},";
            S<<"\"counterexample_observed\":"<<(Counterexample?"true":"false")<<",\"ue_audio_gate\":\"NOT_RUN\",";
            S<<"\"unvalidated\":[\"UE routing\",\"editor audition\",\"listener motion\",\"recipe change\",\"absorption outside 0.1/0.8\"]}\n";
            Require(S.good(),"summary write failed");
        }
        iplHRTFRelease(&HRTF); iplContextRelease(&Context);
        std::cout<<"counterexample_observed="<<(Counterexample?"yes":"no")<<std::endl;
        return Counterexample ? 2 : 0;
    } catch (const std::exception& E) {
        std::cerr<<"FAIL "<<E.what()<<std::endl;
        if (HRTF) iplHRTFRelease(&HRTF); if (Context) iplContextRelease(&Context); return 1;
    }
}
