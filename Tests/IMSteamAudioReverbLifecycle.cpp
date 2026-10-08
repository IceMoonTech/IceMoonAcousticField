// Native counterexamples against production Simulation/ReverbRenderer/AudioRenderer.
// Serial owner only; SDK-generated probes and production Bake/Load/Render are reused.
// Slot transitions mirror worker/audio ownership; only successful Render sets Applied.
// A failed case stops immediately, while independent cases retain their own receipts.
// Exit 0: all four native cases pass; 2: counterexample; 1: harness/SDK setup error.
#include "IMAcousticSimulation.h"
#include "IMAcousticReverbRenderer.h"
#include "IMAcousticSDKContext.h"
#include "IMAcousticBakeRecipe.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#define NOMINMAX
#include <Windows.h>
#include <DbgHelp.h>

LONG WINAPI CrashEvidence(EXCEPTION_POINTERS* Exception)
{
	HANDLE Process=GetCurrentProcess();SymInitialize(Process,nullptr,TRUE);
	CONTEXT Context=*Exception->ContextRecord;
	std::cerr<<"exception="<<Exception->ExceptionRecord->ExceptionCode<<" rip="<<Context.Rip<<std::endl;
	if(Context.Rip==0){Context.Rip=*reinterpret_cast<DWORD64*>(Context.Rsp);Context.Rsp+=8;}
	STACKFRAME64 Frame{};Frame.AddrPC={Context.Rip,0,AddrModeFlat};
	Frame.AddrFrame={Context.Rbp,0,AddrModeFlat};Frame.AddrStack={Context.Rsp,0,AddrModeFlat};
	for(int I=0;I<24&&Frame.AddrPC.Offset;++I){
		char Storage[sizeof(SYMBOL_INFO)+MAX_SYM_NAME]{};
		auto* Symbol=reinterpret_cast<SYMBOL_INFO*>(Storage);Symbol->SizeOfStruct=sizeof(SYMBOL_INFO);Symbol->MaxNameLen=MAX_SYM_NAME;
		DWORD64 Offset=0;std::cerr<<"pc="<<Frame.AddrPC.Offset;
		if(SymFromAddr(Process,Frame.AddrPC.Offset,&Offset,Symbol))std::cerr<<" "<<Symbol->Name<<"+"<<Offset;
		std::cerr<<std::endl;
		if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,Process,GetCurrentThread(),&Frame,&Context,nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr))break;
	}
	return EXCEPTION_EXECUTE_HANDLER;
}
namespace IMSteamAudioReverbLifecyclePrivate
{
// Test-executable setting only. The W2 runner can match the UE device block;
// the original four-room-case fixture keeps its default 512-frame contract.
#ifndef REVERBTESTBLOCKFRAMES
#define REVERBTESTBLOCKFRAMES 512
#endif
constexpr int Rate=48000,BlockLocal=REVERBTESTBLOCKFRAMES;
static_assert(BlockLocal==512||BlockLocal==1024,"Supported native test block sizes: 512 or 1024");
constexpr int IRSamples=int(Rate*IMAcousticRecipe::ReverbSavedDurationS);
constexpr int IRBlocks=(IRSamples+Rate/2+BlockLocal-1)/BlockLocal;
// Same baked IR and identical input should agree to float roundoff. These gates
// are fixed before running; A/B separation prevents an all-zero equality pass.
constexpr double MaxAbsTolerance=1.e-6,RelativeTolerance=1.e-5;
struct Counterexample:std::runtime_error{using std::runtime_error::runtime_error;};
void Require(bool Ok,const std::string& Why){if(!Ok)throw std::runtime_error(Why);}
void Gate(bool Ok,const std::string& Why){if(!Ok)throw Counterexample(Why);}
struct SDKScope {
	IPLContext Context=nullptr;IPLHRTF HRTF=nullptr;
	~SDKScope(){if(HRTF)iplHRTFRelease(&HRTF);if(Context)iplContextRelease(&Context);}
};
struct CaseResult {
	std::string Name,Status="NOT_RUN",Detail;
	std::map<std::string,double> Metrics;
};
void Box(FIMAcousticSceneInput& S,IPLVector3 Lo,IPLVector3 Hi)
{
	const int Base=int(S.Vertices.size());
	for(int I=0;I<8;++I)S.Vertices.push_back({I&1?Hi.x:Lo.x,I&2?Hi.y:Lo.y,I&4?Hi.z:Lo.z});
	const int Faces[12][3]={{0,2,3},{0,3,1},{4,5,7},{4,7,6},{0,1,5},{0,5,4},
		{2,6,7},{2,7,3},{0,4,6},{0,6,2},{1,3,7},{1,7,5}};
	for(const auto& F:Faces){S.Triangles.push_back({{Base+F[0],Base+F[1],Base+F[2]}});S.MaterialIndices.push_back(0);}
}
FIMAcousticSceneInput BuildRoomScene(bool Door)
{
	FIMAcousticSceneInput S;IPLMaterial M{};
	for(int I=0;I<3;++I){M.absorption[I]=.25f;M.transmission[I]=0;}
	M.scattering=.5f;S.Materials.push_back(M);
	// SDK meters: sealed interior x[-4,4], y[0,3], z[-3,3], 0.1m walls.
	Box(S,{-4.1f,-.1f,-3.1f},{4.1f,0,3.1f});Box(S,{-4.1f,3,-3.1f},{4.1f,3.1f,3.1f});
	Box(S,{-4.1f,0,-3.1f},{-4,3,3.1f});Box(S,{4,0,-3.1f},{4.1f,3,3.1f});
	Box(S,{-4,0,-3.1f},{4,3,-3});Box(S,{-4,0,3},{4,3,3.1f});
	if(Door){
		Box(S,{-.05f,0,-3},{.05f,3,.5f});Box(S,{-.05f,0,2.5f},{.05f,3,3});
		Box(S,{-.05f,2.5f,.5f},{.05f,3,2.5f});
	}
	return S;
}
IPLCoordinateSpace3 Space(float X,float Z){return {{1,0,0},{0,1,0},{0,0,-1},{X,1.5f,Z}};}
double SumEnergy(const std::vector<float>& V)
{
	double E=0;for(float X:V){Require(std::isfinite(X),"non-finite native output");E+=double(X)*X;}return E;
}
void Wave(const std::filesystem::path& P,const std::vector<float>& V)
{
	std::ofstream F(P,std::ios::binary);
	auto U16=[&](std::uint16_t N){F.put(N&255);F.put((N>>8)&255);};
	auto U32=[&](std::uint32_t N){for(int I=0;I<4;++I)F.put((N>>(8*I))&255);};
	F.write("RIFF",4);U32(36+uint32_t(V.size()*4));F.write("WAVEfmt ",8);
	U32(16);U16(3);U16(2);U32(Rate);U32(Rate*8);U16(8);U16(32);
	F.write("data",4);U32(uint32_t(V.size()*4));F.write(reinterpret_cast<const char*>(V.data()),V.size()*4);
	Require(F.good(),"WAV write failed");
}
void BakeRoom(bool Door,FIMAcousticBakeData& Bake,const std::filesystem::path& Dir,const std::string& Name)
{
	auto Geometry=BuildRoomScene(Door);FIMAcousticSimulation Baker;std::string Error;
	IPLProbeGenerationParams P{};P.type=IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;P.spacing=1;P.height=1.5f;
	P.transform.elements[0][0]=8;P.transform.elements[1][1]=3;P.transform.elements[1][3]=1.5f;
	P.transform.elements[2][2]=6;P.transform.elements[3][3]=1;
	Require(Baker.GenerateProbes(Geometry,P,Geometry.Probes,Error),"GenerateProbes: "+Error);
	Require(!Geometry.Probes.empty(),"real room generated no probes");
	Require(Baker.Bake(Geometry,Bake,Error),"Bake: "+Error);
	Require(Bake.CoverageProbes.size()==Geometry.Probes.size(),"Bake omitted coverage spheres");
	std::ofstream CSV(Dir/(Name+"-probes.csv"));CSV<<std::setprecision(12)<<"index,x_m,y_m,z_m,radius_m\n";
	for(size_t I=0;I<Bake.CoverageProbes.size();++I){const auto& S=Bake.CoverageProbes[I];
		CSV<<I<<','<<S.center.x<<','<<S.center.y<<','<<S.center.z<<','<<S.radius<<'\n';}
	std::ofstream(Dir/(Name+"-scene.bin"),std::ios::binary).write(reinterpret_cast<const char*>(Bake.Scene.data()),Bake.Scene.size());
	std::ofstream(Dir/(Name+"-probes.bin"),std::ios::binary).write(reinterpret_cast<const char*>(Bake.ProbeBatch.data()),Bake.ProbeBatch.size());
	std::cout<<Name<<" probes="<<Bake.CoverageProbes.size()<<" triangles="<<Geometry.Triangles.size()<<std::endl;
}
std::vector<size_t> FindOverlaps(const FIMAcousticBakeData& B,IPLVector3 L)
{
	std::vector<size_t> Result;
	for(size_t I=0;I<B.CoverageProbes.size();++I){const auto& P=B.CoverageProbes[I];
		const double X=double(L.x)-P.center.x,Y=double(L.y)-P.center.y,Z=double(L.z)-P.center.z;
		if(X*X+Y*Y+Z*Z<=double(P.radius)*P.radius)Result.push_back(I);}
	return Result;
}
void Transition(FIMAcousticReverbSlot& S,EIMAcousticIRState From,EIMAcousticIRState To,
	std::ofstream& Trace,const std::string& Event)
{
	auto Expected=From;Require(S.State.compare_exchange_strong(Expected,To),"fixture violated slot ownership: "+Event);
	Trace<<Event<<','<<int(From)<<','<<int(To)<<','<<S.Applied<<','<<(S.Source!=nullptr)<<'\n';Trace.flush();
}
std::vector<float> Impulse(FIMAcousticReverbSlot& Slot,SDKScope& SDK,std::ofstream& Trace,const std::string& Name)
{
	FIMAcousticReverbRenderer Renderer;
	Require(Renderer.Initialize(SDK.Context,SDK.HRTF,Rate,BlockLocal,IRSamples),"ReverbRenderer init failed");
	Transition(Slot,EIMAcousticIRState::Writing,EIMAcousticIRState::Ready,Trace,Name+"-publish");
	Transition(Slot,EIMAcousticIRState::Ready,EIMAcousticIRState::Reading,Trace,Name+"-claim");
	std::vector<float> Dry(BlockLocal),Wet(BlockLocal*2),All;All.reserve(IRBlocks*BlockLocal*2);
	for(int B=0;B<IRBlocks;++B){
		std::fill(Dry.begin(),Dry.end(),0.f);if(B==0)Dry[0]=.5f;
		Gate(Renderer.Render(Dry.data(),BlockLocal,Slot.Params,Slot.Listener,Wet.data()),Name+": production reverb Render failed");
		Slot.Applied=true; // Same acknowledgement as UE audio: only AFTER successful Apply.
		All.insert(All.end(),Wet.begin(),Wet.end());
	}
	Renderer.Reset(); // Release histories before publishing the last IR lease Free.
	Transition(Slot,EIMAcousticIRState::Reading,EIMAcousticIRState::Free,Trace,Name+"-last-apply");
	return All;
}
struct FIMDifference {double Max=0,Relative=0;};
FIMDifference Compare(const std::vector<float>& A,const std::vector<float>& B)
{
	Require(A.size()==B.size(),"comparison length mismatch");double D=0,E=SumEnergy(B);FIMDifference R;
	SumEnergy(A);
	for(size_t I=0;I<A.size();++I){const double X=double(A[I])-B[I];D+=X*X;R.Max=std::max(R.Max,std::abs(X));}
	R.Relative=E>0?std::sqrt(D/E):(D==0?0:1.e30);return R;
}
void MaskCase(FIMAcousticSimulation& Sim,SDKScope& SDK,const std::filesystem::path& Dir,
	CaseResult& Result,bool Path)
{
	const std::string Name=Path?"path":"direct";std::string Error;FIMAcousticAudioFrame Frame;
	Require(Sim.Evaluate(1,1,Space(-2,-1.5f),Space(Path?2:-1,-1.5f),Frame,Error),Error);
	Gate(Frame.DirectValid&&(Path?(Frame.PathValid&&Frame.Direct.occlusion<.01f):(!Frame.PathValid&&Frame.Direct.occlusion>.99f)),Name+": required real SDK route unavailable");
	FIMAcousticAudioRenderer Ref,Muted;
	Require(Ref.Initialize(SDK.Context,SDK.HRTF,Rate,BlockLocal)&&Muted.Initialize(SDK.Context,SDK.HRTF,Rate,BlockLocal),"AudioRenderer init failed");
	std::vector<float> Dry(BlockLocal),R(BlockLocal*2),RD(BlockLocal*2),RP(BlockLocal*2),M(BlockLocal*2),MD(BlockLocal*2),MP(BlockLocal*2),AllR,AllM;
	std::ofstream Trace(Dir/("mask-"+Name+"-blocks.csv"));
	Trace<<std::setprecision(15)<<"block,routes,reference_energy,output_energy,max_expected_output_error,max_restored_error\n";
	double MaxError=0,MaxRestored=0,MutedEnergy=0,ActiveReferenceEnergy=0,RecoveryReferenceEnergy=0;
	int Completed=0;
	for(int B=0;B<128;++B){
		// Whole-source mute (0), recovery, then only the tested route muted.
		const std::uint32_t Mask=(B>=16&&B<48)?0:((B>=64&&B<96)?(Path?1:2):3);
		for(int I=0;I<BlockLocal;++I){const double T=double(B*BlockLocal+I)/Rate;
			Dry[I]=float(.08*(std::sin(6.283185307179586*233*T)+std::sin(6.283185307179586*997*T)+std::sin(6.283185307179586*3109*T)));}
		if(B==47||B==95)Dry[BlockLocal-1]+=.25f; // Excite history immediately before each unmute.
		Frame.Sequence=B+1;
		Gate(Ref.Render(Dry.data(),BlockLocal,Frame,R.data(),RD.data(),RP.data()),Name+": reference Render failed");
		Gate(Muted.Render(Dry.data(),BlockLocal,Frame,M.data(),MD.data(),MP.data(),nullptr,Mask),Name+": masked Render failed");
		double BlockError=0,Restored=0;
		for(size_t I=0;I<M.size();++I){
			const double D=(Mask&1)?RD[I]:0,P=(Mask&2)?RP[I]:0;
			BlockError=std::max({BlockError,std::abs(double(M[I])-(D+P)),std::abs(double(MD[I])-D),std::abs(double(MP[I])-P)});
			if((B>=48&&B<64)||B>=96)Restored=std::max(Restored,std::abs(double(M[I])-R[I]));
		}
		const double RE=SumEnergy(R),ME=SumEnergy(M);
		if(!(Mask&(Path?2:1)))MutedEnergy+=SumEnergy(Path?MP:MD);
		ActiveReferenceEnergy+=SumEnergy(Path?RP:RD);
		if((B>=48&&B<64)||B>=96)RecoveryReferenceEnergy+=RE;
		MaxError=std::max(MaxError,BlockError);MaxRestored=std::max(MaxRestored,Restored);
		AllR.insert(AllR.end(),R.begin(),R.end());AllM.insert(AllM.end(),M.begin(),M.end());
		Trace<<B<<','<<Mask<<','<<RE<<','<<ME<<','<<BlockError<<','<<Restored<<'\n';Trace.flush();++Completed;
		// Preserve raw audio up to the first discrepancy, then stop this case.
		if(BlockError>MaxAbsTolerance||Restored>MaxAbsTolerance){
			Wave(Dir/("mask-"+Name+"-reference.wav"),AllR);Wave(Dir/("mask-"+Name+"-output.wav"),AllM);
			Gate(false,Name+": masked DSP differs from continuous-input reference at block "+std::to_string(B));
		}
	}
	Wave(Dir/("mask-"+Name+"-reference.wav"),AllR);Wave(Dir/("mask-"+Name+"-output.wav"),AllM);
	Result.Metrics[Name+"_blocks"]=Completed;Result.Metrics[Name+"_max_output_error"]=MaxError;
	Result.Metrics[Name+"_max_restored_error"]=MaxRestored;Result.Metrics[Name+"_muted_stem_energy"]=MutedEnergy;
	Result.Metrics[Name+"_reference_energy"]=ActiveReferenceEnergy;Result.Metrics[Name+"_recovery_reference_energy"]=RecoveryReferenceEnergy;
	Gate(ActiveReferenceEnergy>1.e-6&&RecoveryReferenceEnergy>1.e-6,Name+": zero-signal comparison is invalid");
	Gate(MutedEnergy==0,Name+": muted route leaked output");
}
int W2Cold(SDKScope& SDK,const std::filesystem::path& Dir,const std::filesystem::path& InputDir)
{
	FIMAcousticBakeData Bake;
	auto ReadBytes=[&](const char* Name,std::vector<std::uint8_t>& Bytes){
		std::ifstream F(InputDir/Name,std::ios::binary);Require(F.good(),std::string("W2 missing ")+Name);
		Bytes.assign(std::istreambuf_iterator<char>(F),std::istreambuf_iterator<char>());
		Require(!Bytes.empty(),std::string("W2 empty ")+Name);
	};
	ReadBytes("scene.bin",Bake.Scene);ReadBytes("probes.bin",Bake.ProbeBatch);
	std::ifstream Input(InputDir/"coverage-points.txt");
	int ProbeCount=0,PointCount=0,Order=-1;double Duration=0,Origin[3]{};
	Require(bool(Input>>ProbeCount>>PointCount>>Duration>>Order)&&ProbeCount>0&&ProbeCount<=1000000
		&&PointCount==6&&Order==FIMAcousticAudioFrame::Order&&std::abs(Duration-IMAcousticRecipe::ReverbSavedDurationS)<1.e-6,
		"W2 metadata dimensions/recipe differ from this renderer");
	for(double& V:Origin)Require(bool(Input>>V)&&std::isfinite(V),"W2 origin invalid");
	Bake.CoverageProbes.resize(ProbeCount);
	for(auto& P:Bake.CoverageProbes){
		Require(bool(Input>>P.center.x>>P.center.y>>P.center.z>>P.radius),"W2 coverage row missing");
		Require(std::isfinite(P.center.x)&&std::isfinite(P.center.y)&&std::isfinite(P.center.z)
			&&std::isfinite(P.radius)&&P.radius>0,"W2 coverage row invalid");
	}
	struct FIMW2Point{std::string Name;double UE[3]{};IPLCoordinateSpace3 Listener{};};
	std::vector<FIMW2Point> Points;const double ExpectedX[]{550,700,900,1000,1100,1250};
	for(int I=0;I<PointCount;++I){
		FIMW2Point P;P.Listener=Space(0,0);double SDKPosition[3]{};
		Require(bool(Input>>P.Name>>P.UE[0]>>P.UE[1]>>P.UE[2]>>SDKPosition[0]>>SDKPosition[1]>>SDKPosition[2]),"W2 point missing");
		Require(P.UE[0]==ExpectedX[I]&&P.UE[1]==300&&P.UE[2]==150,"W2 point order/UE coordinates changed");
		const double Converted[]{(P.UE[1]-Origin[1])*.01,(P.UE[2]-Origin[2])*.01,-(P.UE[0]-Origin[0])*.01};
		for(int Axis=0;Axis<3;++Axis)Require(std::isfinite(SDKPosition[Axis])&&std::abs(SDKPosition[Axis]-Converted[Axis])<1.e-10,"W2 UE-to-SDK mismatch");
		P.Listener.origin={float(Converted[0]),float(Converted[1]),float(Converted[2])};Points.push_back(P);
	}
	std::string Extra;Require(!(Input>>Extra),"W2 trailing metadata rows");
	FIMAcousticSimulation Sim;std::string Error;
	const bool Loaded=Sim.Load(Bake,Rate,BlockLocal,Error);Require(Loaded,"W2 production Load failed: "+Error);
	FIMAcousticReverbSlot Reused; // Retain this source across ALL six positions.
	std::ofstream States(Dir/"slot-states.csv"),Cases(Dir/"case-results.tsv"),Metrics(Dir/"metrics.csv");
	std::ofstream PCM(Dir/"w2-impulse-blocks.csv"),PointsCSV(Dir/"w2-per-point.csv"),Coverage(Dir/"w2-nearby-probes.csv");
	States<<"event,from_state,to_state,applied,has_source\n";Cases<<"case\tstatus\tdetail\n";
	Metrics<<std::setprecision(17)<<"case,metric,value\n";
	PCM<<std::setprecision(17)<<"case,route,block,time_s,energy,max_abs\n";
	PointsCSV<<std::setprecision(17)<<"case,ue_x_cm,ue_y_cm,ue_z_cm,sdk_x_m,sdk_y_m,sdk_z_m,reused_query,reused_rendered,reused_energy,fresh_query,fresh_rendered,fresh_energy,max_abs_difference,relative_l2_difference,status\n";
	Coverage<<std::setprecision(17)<<"case,probe,x_m,y_m,z_m,radius_m,radius_squared_minus_distance_squared,strict_double_overlap\n";
	bool Failed=false;
	for(const auto& P:Points){
		CaseResult Result;Result.Name=P.Name;
		Result.Metrics["block_frames"]=BlockLocal;
		Result.Metrics["strict_double_overlaps"]=FindOverlaps(Bake,P.Listener.origin).size();
		Result.Metrics["reused_applied_before_query"]=Reused.Applied;
		for(size_t I=0;I<Bake.CoverageProbes.size();++I){const auto& S=Bake.CoverageProbes[I];
			const double X=double(P.Listener.origin.x)-S.center.x,Y=double(P.Listener.origin.y)-S.center.y,Z=double(P.Listener.origin.z)-S.center.z;
			const double Margin=double(S.radius)*S.radius-X*X-Y*Y-Z*Z;
			// Diagnostic only, not a substitute for production conservative coverage.
			// Include near-boundary candidates to expose float-vs-double containment.
			if(Margin>=-1.e-4)Coverage<<P.Name<<','<<I<<','<<S.center.x<<','<<S.center.y<<','<<S.center.z<<','<<S.radius<<','<<Margin<<','<<(Margin>=0)<<'\n';
		}
		auto RenderAt=[&](FIMAcousticReverbSlot& Slot,const std::string& Route,bool& Query,bool& Rendered,std::vector<float>& Output){
			const std::string Name=P.Name+"-"+Route;std::string QueryError;
			Transition(Slot,EIMAcousticIRState::Free,EIMAcousticIRState::Writing,States,Name+"-write");
			Query=Sim.EvaluateReverb(Slot,P.Listener,QueryError);
			Result.Metrics[Route+"_query_accepted"]=Query;
			if(!Query){
				std::ofstream(Dir/(Name+"-error.txt"))<<QueryError<<'\n';Result.Detail+=Route+" QUERY_REJECTED: "+QueryError+"; ";
				Transition(Slot,EIMAcousticIRState::Writing,EIMAcousticIRState::Free,States,Name+"-rejected");return;
			}
			try{
				// New Renderer each call: same reset history, same 0.5 impulse,
				// same full tail. IMImpulse acknowledges every successful Apply.
				Output=Impulse(Slot,SDK,States,Name);Rendered=true;
				const double Energy=SumEnergy(Output);Result.Metrics[Route+"_energy"]=Energy;
				Result.Metrics[Route+"_zero_ir"]=Energy==0;
				Result.Metrics[Route+"_applied_after_render"]=Slot.Applied;
				Wave(Dir/(Name+".wav"),Output);
				int NonzeroBlocks=0,FirstNonzero=-1;
				for(int B=0;B<IRBlocks;++B){double BlockEnergy=0,Peak=0;
					for(int I=0;I<BlockLocal*2;++I){const double V=Output[B*BlockLocal*2+I];BlockEnergy+=V*V;Peak=std::max(Peak,std::abs(V));}
					PCM<<P.Name<<','<<Route<<','<<B<<','<<double(B*BlockLocal)/Rate<<','<<BlockEnergy<<','<<Peak<<'\n';
					if(B==0)Result.Metrics[Route+"_first_block_energy"]=BlockEnergy;
					if(BlockEnergy>0){++NonzeroBlocks;if(FirstNonzero<0)FirstNonzero=B;}
				}
				Result.Metrics[Route+"_nonzero_blocks"]=NonzeroBlocks;
				// -1 means no nonzero block in the entire captured impulse/tail.
				Result.Metrics[Route+"_first_nonzero_block"]=FirstNonzero;
				if(Energy<=1.e-6)Result.Detail+=Route+(Energy==0?" ZERO_IR; ":" ENERGY_BELOW_NATIVE_SIGNAL_GATE; ");
			}catch(const std::exception& E){
				Rendered=false;Output.clear();Result.Detail+=Route+" RENDER_REJECTED: "+E.what()+"; ";
				std::ofstream(Dir/(Name+"-error.txt"))<<E.what()<<'\n';
				// IMImpulse's local renderer has been destroyed on unwind; no
				// reader remains. Preserve Applied truth; do not force an ACK.
				const auto State=Slot.State.load();
				if(State!=EIMAcousticIRState::Free)Transition(Slot,State,EIMAcousticIRState::Free,States,Name+"-failed-render-release");
			}
			Result.Metrics[Route+"_rendered"]=Rendered;
		};
		bool ReusedQuery=false,FreshQuery=false,ReusedRendered=false,FreshRendered=false;
		std::vector<float> Actual,Expected;
		RenderAt(Reused,"reused",ReusedQuery,ReusedRendered,Actual);
		FIMAcousticReverbSlot Fresh; // New detached SDK source for this point only.
		RenderAt(Fresh,"fresh",FreshQuery,FreshRendered,Expected);
		// A stationary listener still needs a valid IR after effect reset and
		// slot reuse. Different-position queries alone do not cover this case.
		bool RepeatQuery=false,RepeatRendered=false;std::vector<float> Repeated;
		RenderAt(Reused,"static-repeat",RepeatQuery,RepeatRendered,Repeated);
		FIMDifference RepeatDifference;
		if(RepeatRendered&&FreshRendered)RepeatDifference=Compare(Repeated,Expected);
		Result.Metrics["static_repeat_max_abs_difference"]=RepeatDifference.Max;
		Result.Metrics["static_repeat_relative_l2_difference"]=RepeatDifference.Relative;
		FIMDifference Difference;
		if(ReusedRendered&&FreshRendered){
			Difference=Compare(Actual,Expected);
			Result.Metrics["max_abs_difference"]=Difference.Max;Result.Metrics["relative_l2_difference"]=Difference.Relative;
			if(Difference.Max>MaxAbsTolerance||Difference.Relative>RelativeTolerance)Result.Detail+="REUSED_FRESH_IR_MISMATCH; ";
		}
		Result.Metrics["signal_energy_gate"]=1.e-6;Result.Metrics["max_abs_tolerance"]=MaxAbsTolerance;
		Result.Metrics["relative_l2_tolerance"]=RelativeTolerance;
		const bool Pass=RepeatRendered&&Result.Metrics["static-repeat_energy"]>1.e-6
			&&RepeatDifference.Max<=MaxAbsTolerance&&RepeatDifference.Relative<=RelativeTolerance
			&&ReusedRendered&&FreshRendered&&Result.Metrics["reused_energy"]>1.e-6&&Result.Metrics["fresh_energy"]>1.e-6
			&&Difference.Max<=MaxAbsTolerance&&Difference.Relative<=RelativeTolerance;
		Result.Status=Pass?"PASS":"COUNTEREXAMPLE";if(!Pass)Failed=true;
		if(Pass)Result.Detail="both impulses nonzero and reused IR agrees with corresponding fresh slot";
		for(char& C:Result.Detail)if(C=='\n'||C=='\r'||C=='\t')C=' ';
		Cases<<P.Name<<'\t'<<Result.Status<<'\t'<<Result.Detail<<'\n';Cases.flush();
		for(const auto& V:Result.Metrics)Metrics<<P.Name<<','<<V.first<<','<<V.second<<'\n';Metrics.flush();
		PointsCSV<<P.Name<<','<<P.UE[0]<<','<<P.UE[1]<<','<<P.UE[2]<<','<<P.Listener.origin.x<<','<<P.Listener.origin.y<<','<<P.Listener.origin.z
			<<','<<ReusedQuery<<','<<ReusedRendered<<',';
		if(ReusedRendered)PointsCSV<<Result.Metrics["reused_energy"];
		PointsCSV<<','<<FreshQuery<<','<<FreshRendered<<',';if(FreshRendered)PointsCSV<<Result.Metrics["fresh_energy"];
		PointsCSV<<',';if(ReusedRendered&&FreshRendered)PointsCSV<<Difference.Max;
		PointsCSV<<',';if(ReusedRendered&&FreshRendered)PointsCSV<<Difference.Relative;
		PointsCSV<<','<<Result.Status<<'\n';PointsCSV.flush();PCM.flush();Coverage.flush();
		std::cout<<std::setprecision(17)<<"W2-POINT "<<P.Name<<" "<<Result.Status;
		if(ReusedRendered)std::cout<<" reused_E="<<Result.Metrics["reused_energy"];
		if(FreshRendered)std::cout<<" fresh_E="<<Result.Metrics["fresh_energy"];
		std::cout<<" "<<Result.Detail<<std::endl;
		// Each fixed point is reported once. Continue the requested movement
		// sequence without retries, rebaking, threshold changes or substituting IRs.
	}
	return Failed?2:0;
}
} // namespace

int main(int Argc,char** Argv)
{
	SetUnhandledExceptionFilter(CrashEvidence);
	try {
		IMSteamAudioReverbLifecyclePrivate::Require(Argc==2||Argc==3,"usage: reverb-lifecycle unique-evidence-directory [frozen-W2-directory]");
		IMSteamAudioReverbLifecyclePrivate::Require(Argc==3||BlockLocal==512,"1024-frame selection is limited to W2 cold-data mode");
		const std::filesystem::path Dir=Argv[1];IMSteamAudioReverbLifecyclePrivate::Require(std::filesystem::is_directory(Dir),"evidence directory missing");
		IMSteamAudioReverbLifecyclePrivate::SDKScope SDK;SDK.Context=iplContextRetain(IMAcousticSDKContext::GetAcousticSDKContext());IMSteamAudioReverbLifecyclePrivate::Require(SDK.Context!=nullptr,"SDK context unavailable");
		IPLAudioSettings AS{IMSteamAudioReverbLifecyclePrivate::Rate,BlockLocal};IPLHRTFSettings HS{};HS.type=IPL_HRTFTYPE_DEFAULT;HS.volume=1;
		IMSteamAudioReverbLifecyclePrivate::Require(iplHRTFCreate(SDK.Context,&AS,&HS,&SDK.HRTF)==IPL_STATUS_SUCCESS,"HRTF create failed");
		// Cold W2 mode never generates or bakes geometry and never repeats the
		// four already-closed room cases. It consumes exactly the frozen bytes.
		if(Argc==3)return IMSteamAudioReverbLifecyclePrivate::W2Cold(SDK,Dir,Argv[2]);
		FIMAcousticBakeData Room;IMSteamAudioReverbLifecyclePrivate::BakeRoom(false,Room,Dir,"room");
		std::ofstream States(Dir/"slot-states.csv");States<<"event,from_state,to_state,applied,has_source\n";
		std::ofstream Cases(Dir/"case-results.tsv"),Metrics(Dir/"metrics.csv");
		Cases<<"case\tstatus\tdetail\n";Metrics<<std::setprecision(15)<<"case,metric,value\n";
		bool Failed=false,HarnessError=false;
		auto Run=[&](const std::string& Name,auto Body){
			IMSteamAudioReverbLifecyclePrivate::CaseResult R;R.Name=Name;std::cout<<"CASE-BEGIN "<<Name<<std::endl;
			try{Body(R);R.Status="PASS";R.Detail="native gates passed";}
			catch(const IMSteamAudioReverbLifecyclePrivate::Counterexample& E){R.Status="COUNTEREXAMPLE";R.Detail=E.what();Failed=true;}
			catch(const std::exception& E){R.Status="HARNESS_ERROR";R.Detail=E.what();HarnessError=true;}
			for(char& C:R.Detail)if(C=='\n'||C=='\r'||C=='\t')C=' ';
			Cases<<Name<<'\t'<<R.Status<<'\t'<<R.Detail<<'\n';Cases.flush();
			for(const auto& V:R.Metrics)Metrics<<Name<<','<<V.first<<','<<V.second<<'\n';Metrics.flush();
			std::cout<<"CASE-END "<<Name<<" "<<R.Status<<" "<<R.Detail<<std::endl;
		};
		Run("no-sphere-coverage",[&](IMSteamAudioReverbLifecyclePrivate::CaseResult& R){
			FIMAcousticSimulation Sim;std::string Error;IMSteamAudioReverbLifecyclePrivate::Require(Sim.Load(Room,IMSteamAudioReverbLifecyclePrivate::Rate,BlockLocal,Error),Error);
			FIMAcousticReverbSlot Slot;auto A=IMSteamAudioReverbLifecyclePrivate::Space(0,0),Outside=IMSteamAudioReverbLifecyclePrivate::Space(30,0);
			R.Metrics["valid_overlaps"]=IMSteamAudioReverbLifecyclePrivate::FindOverlaps(Room,A.origin).size();R.Metrics["invalid_overlaps"]=IMSteamAudioReverbLifecyclePrivate::FindOverlaps(Room,Outside.origin).size();
			IMSteamAudioReverbLifecyclePrivate::Require(R.Metrics["valid_overlaps"]>0&&R.Metrics["invalid_overlaps"]==0,"coverage fixture invalid");
			IMSteamAudioReverbLifecyclePrivate::Transition(Slot,EIMAcousticIRState::Free,EIMAcousticIRState::Writing,States,"no-coverage-prime");
			IMSteamAudioReverbLifecyclePrivate::Gate(Sim.EvaluateReverb(Slot,A,Error),"valid listener rejected: "+Error);
			auto Wet=IMSteamAudioReverbLifecyclePrivate::Impulse(Slot,SDK,States,"no-coverage-valid");R.Metrics["prime_energy"]=IMSteamAudioReverbLifecyclePrivate::SumEnergy(Wet);
			IMSteamAudioReverbLifecyclePrivate::Wave(Dir/"no-coverage-valid.wav",Wet);IMSteamAudioReverbLifecyclePrivate::Gate(R.Metrics["prime_energy"]>1.e-6,"valid listener IR silent");
			IMSteamAudioReverbLifecyclePrivate::Transition(Slot,EIMAcousticIRState::Free,EIMAcousticIRState::Writing,States,"no-coverage-reuse");
			const bool Accepted=Sim.EvaluateReverb(Slot,Outside,Error);R.Metrics["invalid_accepted"]=Accepted;
			std::ofstream(Dir/"no-coverage-error.txt")<<Error<<'\n';
			IMSteamAudioReverbLifecyclePrivate::Gate(!Accepted,"same slot accepted listener with zero overlapping probe spheres");
			IMSteamAudioReverbLifecyclePrivate::Transition(Slot,EIMAcousticIRState::Writing,EIMAcousticIRState::Free,States,"no-coverage-rejected");
		});
		Run("all-overlaps-wall-blocked",[&](IMSteamAudioReverbLifecyclePrivate::CaseResult& R){
			FIMAcousticSimulation Sim;std::string Error;IMSteamAudioReverbLifecyclePrivate::Require(Sim.Load(Room,IMSteamAudioReverbLifecyclePrivate::Rate,BlockLocal,Error),Error);
			const auto Outside=IMSteamAudioReverbLifecyclePrivate::Space(4.5f,0);const auto Overlaps=IMSteamAudioReverbLifecyclePrivate::FindOverlaps(Room,Outside.origin);
			R.Metrics["overlapping_spheres"]=Overlaps.size();IMSteamAudioReverbLifecyclePrivate::Require(!Overlaps.empty(),"wall case has no sphere overlap; cannot substitute uncovered listener");
			std::ofstream Rays(Dir/"blocked-overlap-rays.csv");Rays<<std::setprecision(12)<<"probe,x_m,y_m,z_m,radius_m,listener_x_m,listener_y_m,listener_z_m,wall_interior_crossing,direct_valid,occlusion\n";
			int Blocked=0;
			for(size_t I:Overlaps){const auto& P=Room.CoverageProbes[I];auto Probe=Outside;Probe.origin=P.center;
				// Independent geometry witness: every ray crosses x=4.05 within
				// the solid right wall (x=4..4.1, y=0..3, z=-3.1..3.1).
				const double T=(4.05-P.center.x)/(Outside.origin.x-P.center.x);
				const double Y=P.center.y+T*(Outside.origin.y-P.center.y),Z=P.center.z+T*(Outside.origin.z-P.center.z);
				const bool Interior=P.center.x<=4&&T>0&&T<1&&Y>0&&Y<3&&Z>-3.1&&Z<3.1;
				FIMAcousticAudioFrame F;IMSteamAudioReverbLifecyclePrivate::Require(Sim.Evaluate(1,1,Probe,Outside,F,Error),Error);
				Rays<<I<<','<<P.center.x<<','<<P.center.y<<','<<P.center.z<<','<<P.radius<<",4.5,1.5,0,"<<Interior<<','<<F.DirectValid<<','<<F.Direct.occlusion<<'\n';Rays.flush();
				IMSteamAudioReverbLifecyclePrivate::Require(Interior,"overlapping probe ray does not cross solid wall interior");
				IMSteamAudioReverbLifecyclePrivate::Gate(F.DirectValid&&F.Direct.occlusion<=.01f,"overlapping probe is not wall-blocked in actual SDK direct query");++Blocked;
			}
			R.Metrics["blocked_overlaps"]=Blocked;
			FIMAcousticReverbSlot Slot;
			IMSteamAudioReverbLifecyclePrivate::Transition(Slot,EIMAcousticIRState::Free,EIMAcousticIRState::Writing,States,"wall-prime");
			IMSteamAudioReverbLifecyclePrivate::Gate(Sim.EvaluateReverb(Slot,IMSteamAudioReverbLifecyclePrivate::Space(0,0),Error),"wall case valid prime rejected: "+Error);
			auto Wet=IMSteamAudioReverbLifecyclePrivate::Impulse(Slot,SDK,States,"wall-valid");IMSteamAudioReverbLifecyclePrivate::Gate(IMSteamAudioReverbLifecyclePrivate::SumEnergy(Wet)>1.e-6,"wall valid prime silent");
			IMSteamAudioReverbLifecyclePrivate::Transition(Slot,EIMAcousticIRState::Free,EIMAcousticIRState::Writing,States,"wall-reuse");
			const bool Accepted=Sim.EvaluateReverb(Slot,Outside,Error);R.Metrics["blocked_listener_accepted"]=Accepted;
			std::ofstream(Dir/"blocked-overlap-error.txt")<<Error<<'\n';
			IMSteamAudioReverbLifecyclePrivate::Gate(!Accepted,"reverb accepted positive sphere overlap with every probe wall-blocked");
			IMSteamAudioReverbLifecyclePrivate::Transition(Slot,EIMAcousticIRState::Writing,EIMAcousticIRState::Free,States,"wall-rejected");
		});
		Run("discard-pending-a-then-b",[&](IMSteamAudioReverbLifecyclePrivate::CaseResult& R){
			FIMAcousticSimulation Sim;std::string Error;IMSteamAudioReverbLifecyclePrivate::Require(Sim.Load(Room,IMSteamAudioReverbLifecyclePrivate::Rate,BlockLocal,Error),Error);
			FIMAcousticReverbSlot Reused,FreshB,FreshA;const auto A=IMSteamAudioReverbLifecyclePrivate::Space(0,0),B=IMSteamAudioReverbLifecyclePrivate::Space(3.4f,2.4f);
			IMSteamAudioReverbLifecyclePrivate::Transition(Reused,EIMAcousticIRState::Free,EIMAcousticIRState::Writing,States,"discard-a-write");
			IMSteamAudioReverbLifecyclePrivate::Gate(Sim.EvaluateReverb(Reused,A,Error),"pending A rejected: "+Error);
			IMSteamAudioReverbLifecyclePrivate::Require(!Reused.Applied,"fixture applied pending A unexpectedly");
			IMSteamAudioReverbLifecyclePrivate::Transition(Reused,EIMAcousticIRState::Writing,EIMAcousticIRState::Ready,States,"discard-a-publish");
			IMSteamAudioReverbLifecyclePrivate::Transition(Reused,EIMAcousticIRState::Ready,EIMAcousticIRState::Free,States,"discard-a-without-apply");
			IMSteamAudioReverbLifecyclePrivate::Transition(Reused,EIMAcousticIRState::Free,EIMAcousticIRState::Writing,States,"discard-b-reuse");
			IMSteamAudioReverbLifecyclePrivate::Gate(Sim.EvaluateReverb(Reused,B,Error),"reused B rejected: "+Error);
			auto Actual=IMSteamAudioReverbLifecyclePrivate::Impulse(Reused,SDK,States,"reused-b");IMSteamAudioReverbLifecyclePrivate::Wave(Dir/"reused-b.wav",Actual);
			IMSteamAudioReverbLifecyclePrivate::Transition(FreshB,EIMAcousticIRState::Free,EIMAcousticIRState::Writing,States,"fresh-b-write");
			IMSteamAudioReverbLifecyclePrivate::Gate(Sim.EvaluateReverb(FreshB,B,Error),"fresh B rejected: "+Error);
			auto Expected=IMSteamAudioReverbLifecyclePrivate::Impulse(FreshB,SDK,States,"fresh-b");IMSteamAudioReverbLifecyclePrivate::Wave(Dir/"fresh-b.wav",Expected);
			IMSteamAudioReverbLifecyclePrivate::Transition(FreshA,EIMAcousticIRState::Free,EIMAcousticIRState::Writing,States,"fresh-a-write");
			IMSteamAudioReverbLifecyclePrivate::Gate(Sim.EvaluateReverb(FreshA,A,Error),"fresh A rejected: "+Error);
			auto Different=IMSteamAudioReverbLifecyclePrivate::Impulse(FreshA,SDK,States,"fresh-a");IMSteamAudioReverbLifecyclePrivate::Wave(Dir/"fresh-a.wav",Different);
			const auto Reuse=IMSteamAudioReverbLifecyclePrivate::Compare(Actual,Expected),AB=IMSteamAudioReverbLifecyclePrivate::Compare(Different,Expected);
			R.Metrics["fresh_b_energy"]=IMSteamAudioReverbLifecyclePrivate::SumEnergy(Expected);R.Metrics["fresh_a_energy"]=IMSteamAudioReverbLifecyclePrivate::SumEnergy(Different);
			R.Metrics["reused_vs_fresh_b_max_abs"]=Reuse.Max;R.Metrics["reused_vs_fresh_b_relative_l2"]=Reuse.Relative;
			R.Metrics["fresh_a_vs_b_max_abs"]=AB.Max;R.Metrics["fresh_a_vs_b_relative_l2"]=AB.Relative;
			R.Metrics["max_abs_tolerance"]=IMSteamAudioReverbLifecyclePrivate::MaxAbsTolerance;R.Metrics["relative_l2_tolerance"]=RelativeTolerance;
			IMSteamAudioReverbLifecyclePrivate::Require(IMSteamAudioReverbLifecyclePrivate::SumEnergy(Expected)>1.e-6&&IMSteamAudioReverbLifecyclePrivate::SumEnergy(Different)>1.e-6&&AB.Max>1.e-6&&AB.Relative>1.e-3,"A/B fixture cannot discriminate a stale IR");
			IMSteamAudioReverbLifecyclePrivate::Gate(Reuse.Max<=IMSteamAudioReverbLifecyclePrivate::MaxAbsTolerance&&Reuse.Relative<=RelativeTolerance,"discarded pending A contaminated reused B output");
		});
		Run("audible-routes-output-only",[&](IMSteamAudioReverbLifecyclePrivate::CaseResult& R){
			FIMAcousticBakeData Door;IMSteamAudioReverbLifecyclePrivate::BakeRoom(true,Door,Dir,"door");
			FIMAcousticSimulation Sim;std::string Error;IMSteamAudioReverbLifecyclePrivate::Require(Sim.Load(Door,IMSteamAudioReverbLifecyclePrivate::Rate,BlockLocal,Error),Error);
			IMSteamAudioReverbLifecyclePrivate::MaskCase(Sim,SDK,Dir,R,false);IMSteamAudioReverbLifecyclePrivate::MaskCase(Sim,SDK,Dir,R,true);
		});
		return HarnessError?1:(Failed?2:0);
	} catch(const std::exception& E){std::cerr<<"HARNESS_ERROR "<<E.what()<<std::endl;return 1;}
}
