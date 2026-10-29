// Executes the deployed DLL, not a copy of its DSP equations. Windows x64 only.
// Constant (1,1) input is a coefficient probe, not a listening stimulus.
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#pragma warning(push)
#pragma warning(disable: 4201) // The unmodified Unity SDK ABI uses an anonymous struct/union.
#include "../../NativeCode/AudioPluginInterface.h"
#pragma warning(pop)
#include "../../NativeCode/SpatializerParameters.h"

namespace fs = std::filesystem;
constexpr double Pi = 3.14159265358979323846;
using Pair = std::array<double, 2>;
using Snapshot = int (__cdecl*)(float*, int);
UnityAudioEffectDefinition* definition = nullptr;
Snapshot snapshot = nullptr;
fs::path outputDirectory;
std::ofstream checks;
int failures = 0, checkCount = 0;
volatile double benchmarkSink = 0;

std::ofstream Csv(const char* name, const char* header)
{
    std::ofstream file(outputDirectory / name);
    if (!file) throw std::runtime_error(std::string("Cannot write ") + name);
    file << std::setprecision(12) << header << '\n';
    return file;
}
void Check(const std::string& name, double actual, double limit, const char* detail)
{
    const bool pass = std::isfinite(actual) && actual <= limit;
    checks << name << ',' << (pass ? "PASS" : "FAIL") << ',' << actual << ',' << limit << ',' << detail << '\n';
    ++checkCount;
    if (!pass) { ++failures; std::cerr << "FAIL " << name << ": " << actual << " > " << limit << '\n'; }
}
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void Identity(float* matrix)
{
    std::fill(matrix, matrix + 16, 0.0f);
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}
Pair OracleGains(double x, double z)
{
    const double horizontal = std::hypot(x, z);
    const double pan = horizontal <= 1e-6 ? 0 : std::clamp(x / horizontal, -1.0, 1.0);
    const double angle = (pan + 1) * Pi / 4;
    return { std::cos(angle), std::sin(angle) };
}
class Source
{
public:
    UnityAudioEffectState state{};
    UnityAudioSpatializerData spatial{};
    explicit Source(int sampleRate = 48000)
    {
        Identity(spatial.listenermatrix); Identity(spatial.sourcematrix);
        spatial.spatialblend = 1; spatial.minDistance = 1; spatial.maxDistance = 500;
        state.structsize = sizeof(state); state.samplerate = sampleRate;
        state.hostapiversion = UNITY_AUDIO_PLUGIN_API_VERSION;
        state.flags = UnityAudioEffectStateFlags_IsPlaying;
        state.internal = this; // Non-null synthetic host token; plugin must never dereference it.
        state.spatializerdata = &spatial; state.dspbuffersize = 1024;
        Require(definition->create(&state) == UNITY_AUDIODSP_OK, "create failed");
        Require(state.effectdata != nullptr, "missing effect state");
        Set(Spatializer::P_GAIN, 1);
    }
    ~Source() { if (state.effectdata) definition->release(&state); }
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    void Set(int parameter, float value)
    {
        Require(definition->setfloatparameter(&state, parameter, value) == UNITY_AUDIODSP_OK, "set parameter failed");
    }
    float Get(int parameter)
    {
        float value = 0;
        Require(definition->getfloatparameter(&state, parameter, &value, nullptr) == UNITY_AUDIODSP_OK, "get parameter failed");
        return value;
    }
    void Position(double x, double y, double z)
    {
        spatial.sourcematrix[12] = static_cast<float>(x);
        spatial.sourcematrix[13] = static_cast<float>(y);
        spatial.sourcematrix[14] = static_cast<float>(z);
    }
    void Pan(double pan) { Position(pan, 0, std::sqrt(std::max(0.0, 1 - pan * pan))); }
    void Process(float* input, float* output, int frames, int inChannels = 2, int outChannels = 2)
    {
        state.dspbuffersize = frames;
        Require(definition->process(&state, input, output, frames, inChannels, outChannels) == UNITY_AUDIODSP_OK, "process failed");
        state.prevdsptick = state.currdsptick;
        state.currdsptick += frames;
    }
    Pair Probe()
    {
        float input[2] = { 1, 1 }, output[2] = {};
        Process(input, output, 1);
        return { output[0], output[1] };
    }
    void Reset() { Require(definition->reset(&state) == UNITY_AUDIODSP_OK, "reset failed"); }
};

void ParameterChecks()
{
    auto file = Csv("parameters.csv", "index,name,minimum,maximum,default_value");
    Check("parameter_count", std::abs(static_cast<int>(definition->numparameters) - Spatializer::P_NUM), 0, "ABI regression");
    Require(definition->numparameters == Spatializer::P_NUM, "DLL parameter ABI differs from source; rebuild the subject DLL");
    Source source;
    for (int i = 0; i < Spatializer::P_NUM; ++i)
    {
        const auto& actual = definition->paramdefs[i];
        const auto& expected = Spatializer::Parameters[i];
        file << i << ',' << actual.name << ',' << actual.min << ',' << actual.max << ',' << actual.defaultval << '\n';
        const double error = std::max({ std::abs(actual.min - expected.minimum), std::abs(actual.max - expected.maximum), std::abs(actual.defaultval - expected.initial) });
        Check("parameter_definition_" + std::to_string(i), error, 0, "DLL metadata vs source contract");
    }
    const float original = source.Get(Spatializer::P_GAIN);
    for (float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() })
    {
        const int result = definition->setfloatparameter(&source.state, Spatializer::P_GAIN, bad);
        Check("reject_nonfinite_parameter", result == UNITY_AUDIODSP_ERR_UNSUPPORTED && source.Get(Spatializer::P_GAIN) == original ? 0 : 1, 0, "rejected setter must preserve previous value");
    }
    Check("reject_negative_parameter_index", definition->setfloatparameter(&source.state, -1, 1) == UNITY_AUDIODSP_ERR_UNSUPPORTED ? 0 : 1, 0, "invalid ABI index");
    Check("reject_past_end_parameter_index", definition->setfloatparameter(&source.state, Spatializer::P_NUM, 1) == UNITY_AUDIODSP_ERR_UNSUPPORTED ? 0 : 1, 0, "invalid ABI index");
    source.Set(Spatializer::P_GAIN, -1); Check("gain_lower_clamp", source.Get(Spatializer::P_GAIN), 0, "clamp to zero");
    source.Set(Spatializer::P_GAIN, 99); Check("gain_upper_clamp", std::abs(source.Get(Spatializer::P_GAIN) - 4), 0, "clamp to four");
}

void GeometryChecks()
{
    auto file = Csv("geometry.csv", "case_id,listener_yaw_deg,expected_x,expected_y,expected_z,actual_x,actual_y,actual_z,position_max_error,azimuth_error_deg,elevation_error_deg");
    const std::array<std::array<double, 3>, 7> locations = {{ {-2,0,0},{0,0,2},{0,0,-2},{1,2,3},{0,3,0},{0,0,0},{-4,-2,-3} }};
    double maxPosition = 0, maxAzimuth = 0, maxElevation = 0;
    int id = 0;
    for (double yaw : {0.,45.,90.,180.,270.}) for (int translated : {0,1}) for (const auto& local : locations)
    {
        Source source; source.Set(Spatializer::P_MOTION_SMOOTHING_MS, 0);
        const double a = yaw * Pi / 180, c = std::cos(a), s = std::sin(a);
        const double tx = translated ? -3 : 0, ty = translated ? 1.5 : 0, tz = translated ? 4 : 0;
        auto* m = source.spatial.listenermatrix;
        m[0] = static_cast<float>(c); m[8] = static_cast<float>(-s);
        m[2] = static_cast<float>(s); m[10] = static_cast<float>(c);
        m[12] = static_cast<float>(-c*tx+s*tz); m[13] = static_cast<float>(-ty); m[14] = static_cast<float>(-s*tx-c*tz);
        source.Position(tx+c*local[0]+s*local[2], ty+local[1], tz-s*local[0]+c*local[2]);
        source.Probe();
        float values[5]{};
        Require(snapshot(values, 5) == 1, "snapshot read failed");
        double positionError = 0;
        for (int k=0; k<3; ++k) positionError = std::max(positionError, std::abs(values[k]-local[k]));
        const double horizontal = std::hypot(local[0],local[2]);
        double azimuth = horizontal <= 1e-6 ? 0 : std::atan2(local[0],local[2])*180/Pi;
        if (azimuth < 0) azimuth += 360;
        double azimuthError = std::abs(values[3]-azimuth); azimuthError = std::min(azimuthError, 360-azimuthError);
        const double elevation = std::atan2(local[1],horizontal)*180/Pi;
        // At coincident positions the angle is undefined; don't grade amplified float cancellation.
        const bool defined = std::hypot(horizontal, local[1]) > 1e-5;
        const double elevationError = defined ? std::abs(values[4]-elevation) : 0;
        if (defined) { maxAzimuth=std::max(maxAzimuth,azimuthError); maxElevation=std::max(maxElevation,elevationError); }
        maxPosition=std::max(maxPosition,positionError);
        file << id++ << ',' << yaw << ',' << local[0] << ',' << local[1] << ',' << local[2] << ',' << values[0] << ',' << values[1] << ',' << values[2] << ',' << positionError << ',' << azimuthError << ',' << elevationError << '\n';
    }
    Check("geometry_position_max",maxPosition,1e-5,"synthetic translated and rotated listener matrices; Unity-unit error");
    Check("geometry_azimuth_max_deg",maxAzimuth,1e-3,"circular angle difference; coincident positions excluded");
    Check("geometry_elevation_max_deg",maxElevation,1e-3,"coincident positions excluded");
}

void StaticPanningChecks()
{
    auto file=Csv("panning.csv","azimuth_deg,elevation_deg,blend,actual_left,actual_right,expected_left,expected_right,max_coefficient_error,power_error");
    double maxError=0,maxPower=0;
    for (int azimuth=-180;azimuth<=180;azimuth+=5) for(int elevation : {-60,0,60}) for(float blend : {0.f,0.5f,1.f})
    {
        Source source; source.Set(Spatializer::P_MOTION_SMOOTHING_MS,0); source.spatial.spatialblend=blend;
        const double a=azimuth*Pi/180, e=elevation*Pi/180;
        source.Position(4*std::cos(e)*std::sin(a),4*std::sin(e),4*std::cos(e)*std::cos(a));
        const auto actual=source.Probe();
        const auto g=OracleGains(source.spatial.sourcematrix[12],source.spatial.sourcematrix[14]);
        const Pair expected={1-blend+blend*g[0],1-blend+blend*g[1]};
        const double error=std::max(std::abs(actual[0]-expected[0]),std::abs(actual[1]-expected[1]));
        const double power=std::abs(actual[0]*actual[0]+actual[1]*actual[1]-1);
        maxError=std::max(maxError,error); if(blend==1) maxPower=std::max(maxPower,power);
        file<<azimuth<<','<<elevation<<','<<blend<<','<<actual[0]<<','<<actual[1]<<','<<expected[0]<<','<<expected[1]<<','<<error<<','<<power<<'\n';
    }
    Check("panning_max_coefficient_error",maxError,2e-6,"double-precision analytic oracle; includes spatial blend");
    Check("static_equal_power_max_error",maxPower,2e-6,"only blend=1 and output gain=1; mono coefficient probe");
}

void MotionMeasurements()
{
    auto summary=Csv("motion_summary.csv","trajectory,sample_rate,smoothing_ms,frames,control_period_samples,trace_stride,max_coefficient_delta,tracking_rmse,max_power_error,step_reached_after_samples,expected_step_samples,nonfinite_outputs");
    auto trace=Csv("motion_trace.csv","trajectory,sample_rate,smoothing_ms,sample_index,time_ms,target_left,target_right,actual_left,actual_right");
    for(int rate : {44100,48000,96000}) for(float milliseconds : {0.f,5.f,10.f,20.f}) for(int circular : {0,1})
    {
        Source source(rate); source.Set(Spatializer::P_MOTION_SMOOTHING_MS,milliseconds); source.Pan(-1);
        Pair previous=source.Probe();
        const int frames=circular?rate*2:rate/10;
        const int period=static_cast<int>(std::lround(rate*0.01));
        const int stride=circular?48:1;
        const int expectedSamples=milliseconds==0?1:static_cast<int>(std::lround(rate*milliseconds/1000));
        std::vector<float> input(2*period,1),output(2*period);
        double maxDelta=0,sumSquared=0,maxPower=0; int reached=-1,nonfinite=0;
        Pair target{};
        for(int start=0;start<frames;start+=period)
        {
            const double angle=-Pi/2+2*Pi*start/rate;
            if(circular) source.Position(std::sin(angle),0,std::cos(angle)); else source.Pan(1);
            target=OracleGains(source.spatial.sourcematrix[12],source.spatial.sourcematrix[14]);
            const int count=std::min(period,frames-start);
            source.Process(input.data(),output.data(),count);
            for(int n=0;n<count;++n)
            {
                const int index=start+n; Pair actual={output[2*n],output[2*n+1]};
                for(int c=0;c<2;++c)
                {
                    if(!std::isfinite(actual[c])) ++nonfinite;
                    maxDelta=std::max(maxDelta,std::abs(actual[c]-previous[c]));
                    sumSquared+=(actual[c]-target[c])*(actual[c]-target[c]);
                }
                maxPower=std::max(maxPower,std::abs(actual[0]*actual[0]+actual[1]*actual[1]-1));
                if(!circular && reached<0 && std::abs(actual[0])<1e-7 && std::abs(actual[1]-1)<1e-7) reached=index+1;
                if(rate==48000 && (index%stride==0 || (!circular && index+1==expectedSamples)))
                    trace<<(circular?"circle_1rev_per_sec":"step_left_to_right")<<','<<rate<<','<<milliseconds<<','<<index<<','<<(index+1)*1000.0/rate<<','<<target[0]<<','<<target[1]<<','<<actual[0]<<','<<actual[1]<<'\n';
                previous=actual;
            }
        }
        summary<<(circular?"circle_1rev_per_sec":"step_left_to_right")<<','<<rate<<','<<milliseconds<<','<<frames<<','<<period<<','<<stride<<','<<maxDelta<<','<<std::sqrt(sumSquared/(2*frames))<<','<<maxPower<<','<<reached<<','<<(circular?-1:expectedSamples)<<','<<nonfinite<<'\n';
        const std::string id=std::to_string(rate)+"_"+std::to_string(static_cast<int>(milliseconds))+"_"+(circular?"circle":"step");
        Check("motion_power_"+id,maxPower,2e-6,"unit mono probe; this is not total mixed audio power");
        Check("motion_finite_"+id,nonfinite,0,"two output channels");
        if(!circular) Check("motion_reach_"+id,std::abs(reached-expectedSamples),0,"samples from first accepted target; not device latency");
    }
}

struct Event { int sample; double pan; float gain; };
const std::array<Event,7> Events={{{0,-1,1},{311,1,0.8f},{919,-0.25,1.2f},{1801,0.75,0.6f},{2713,-0.9,1},{5003,0.1,1},{7901,1,1}}};
std::vector<float> PartitionRender(int blockSize,float milliseconds,bool multiband)
{
    Source source; source.Set(Spatializer::P_MOTION_SMOOTHING_MS,milliseconds); source.Set(Spatializer::P_MULTIBAND,multiband?1.f:0.f);
    constexpr int frames=12000;
    std::vector<float> input(frames*2),output(frames*2);
    std::uint32_t seed=0x12345678;
    for(float& value:input) { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; value=static_cast<float>((static_cast<double>(seed)/4294967295.0-0.5)*0.25); }
    int event=0;
    for(int n=0;n<frames;)
    {
        if(event<static_cast<int>(Events.size()) && Events[event].sample==n)
        { source.Pan(Events[event].pan);source.Set(Spatializer::P_GAIN,Events[event].gain);++event; }
        const int end=event<static_cast<int>(Events.size())?Events[event].sample:frames;
        const int count=std::min({blockSize,frames-n,end-n});
        source.Process(input.data()+2*n,output.data()+2*n,count);n+=count;
    }
    return output;
}
void PartitionChecks()
{
    auto schedule=Csv("partition_events.csv","sample_index,pan,output_gain");
    for(const auto& event:Events) schedule<<event.sample<<','<<event.pan<<','<<event.gain<<'\n';
    auto file=Csv("partition.csv","sample_rate,smoothing_ms,multiband,reference_block_frames,test_block_frames,frames,max_abs_output_error,output_rmse");
    for(float ms:{0.f,5.f,20.f}) for(bool bands:{false,true})
    {
        const auto reference=PartitionRender(1,ms,bands);
        for(int block:{37,256,512,1024})
        {
            const auto actual=PartitionRender(block,ms,bands);double maximum=0,squared=0;
            for(size_t i=0;i<reference.size();++i) { const double d=actual[i]-reference[i];maximum=std::max(maximum,std::abs(d));squared+=d*d; }
            file<<48000<<','<<ms<<','<<bands<<",1,"<<block<<",12000,"<<maximum<<','<<std::sqrt(squared/reference.size())<<'\n';
            Check("partition_"+std::to_string(static_cast<int>(ms))+"_"+std::to_string(bands)+"_"+std::to_string(block),maximum,2e-6,"same sample-timestamped events; blocks split at event boundaries");
        }
    }
}

void AttenuationChecks()
{
    auto file=Csv("attenuation_callback.csv","mode,distance,attenuation_in,expected_callback_gain,actual_callback_gain,absolute_error,scope");
    double maximum=0;
    for(int mode:{0,1}) for(double distance:{0.,0.5,1.,2.,4.,8.,16.,64.,500.})
    {
        Source source;Require(source.spatial.distanceattenuationcallback!=nullptr,"distance callback missing");
        if(mode==1) { source.Set(Spatializer::P_AUDIOSRCATTN,0);source.Set(Spatializer::P_CUSTOMFALLOFF,1); }
        constexpr float incoming=0.37f;float actual=0;
        Require(source.spatial.distanceattenuationcallback(&source.state,static_cast<float>(distance),incoming,&actual)==UNITY_AUDIODSP_OK,"attenuation callback failed");
        const double expected=mode==0?incoming:1/std::max(1.0,distance);
        const double error=std::abs(actual-expected);maximum=std::max(maximum,error);
        file<<(mode==0?"default_host_passthrough":"existing_custom_inverse_enabled")<<','<<distance<<','<<incoming<<','<<expected<<','<<actual<<','<<error<<",callback_only_not_unity_rendered_audio\n";
    }
    Check("attenuation_callback_error",maximum,1e-7,"tests existing callback only; no custom Linear model or Unity gain application claimed");
}

void RegressionChecks()
{
    Source source;source.Pan(-1);source.Probe();source.Pan(1);
    source.Reset();auto out=source.Probe();
    Check("reset_snaps_to_target",std::abs(out[0])+std::abs(out[1]-1),1e-7,"no stale ramp after reset");
    source.Pan(-1);source.state.currdsptick+=100;out=source.Probe();
    Check("dsp_gap_snaps_to_target",std::abs(out[0]-1)+std::abs(out[1]),1e-7,"discontinuous synthetic DSP tick");
    source.Pan(1);source.state.samplerate=96000;out=source.Probe();
    Check("sample_rate_change_snaps",std::abs(out[0])+std::abs(out[1]-1),1e-7,"sample rate reinitialization");
    source.spatial.sourcematrix[12]=std::numeric_limits<float>::quiet_NaN();out=source.Probe();
    Check("invalid_transform_bypasses",std::abs(out[0]-1)+std::abs(out[1]-1),0,"safe stereo bypass");
    source.Pan(-1);out=source.Probe();
    Check("invalid_transform_recovers",std::abs(out[0]-1)+std::abs(out[1]),1e-7,"normal transform after bypass");
    float in[2]={0.25f,-0.25f},actual[2]{};
    source.Process(in,actual,1);
    Check("antiphase_stereo_cancels_at_blend1",std::abs(actual[0])+std::abs(actual[1]),0,"documents current stereo-to-mono mean; not a quality benefit");
    source.spatial.spatialblend=0;source.Reset();source.Process(in,actual,1);
    Check("blend0_preserves_stereo",std::abs(actual[0]-in[0])+std::abs(actual[1]-in[1]),0,"4-band off and unity output gain");
    float guard[2]={12,34};source.Process(in,guard,0);
    Check("zero_frames_preserves_output",std::abs(guard[0]-12)+std::abs(guard[1]-34),0,"zero-length callback");
    source.Process(in,actual,1,1,2);
    Check("unsupported_mono_safe_bypass",std::abs(actual[0]-in[0])+std::abs(actual[1]),0,"channel fallback");
    Source left,right;left.Pan(-1);right.Pan(1);
    const auto l=left.Probe(),r=right.Probe();
    Check("independent_source_states",std::abs(l[0]-1)+std::abs(l[1])+std::abs(r[0])+std::abs(r[1]-1),1e-7,"interleaved independent source callbacks");
    float sentinel[4]={1,2,3,4};
    Check("snapshot_short_buffer_rejected",snapshot(sentinel,4)==-1 && sentinel[0]==1 && sentinel[3]==4?0:1,0,"snapshot ABI bounds");
    // Finite out-of-order crossover requests must remain safe under stress.
    Source stress;stress.spatial.spatialblend=0;stress.Set(Spatializer::P_MULTIBAND,1);
    stress.Set(Spatializer::P_CROSSOVER_LOW,20000);stress.Set(Spatializer::P_CROSSOVER_MID,20);stress.Set(Spatializer::P_CROSSOVER_HIGH,20);
    std::vector<float> input(2048,0.1f),output(2048);int bad=0;
    for(int block=0;block<32;++block) {stress.Process(input.data(),output.data(),1024);for(float v:output) if(!std::isfinite(v)) ++bad;}
    Check("extreme_crossovers_finite",bad,0,"finite output with clamped out-of-order crossovers");
}

void FrequencyMeasurements()
{
    auto file=Csv("frequency_response.csv","sample_rate,frequency_hz,mode,warmup_frames,measurement_frames,input_amplitude,output_left_rms,gain_db,left_right_max_error,nonfinite_outputs");
    double neutralMaximum=0,channelMaximum=0;int allBad=0;
    for(int rate:{8000,16000,44100,48000,96000,192000}) for(int mode=-1;mode<4;++mode)
    {
        if(rate!=48000 && mode!=-1) continue;
        for(int frequency:{40,100,248,500,1000,2000,4000,6000,10000,16000})
        {
            if(frequency>=rate*0.45) continue;
            Source source(rate);source.spatial.spatialblend=0;source.Set(Spatializer::P_MULTIBAND,1);
            if(mode>=0) for(int b=0;b<4;++b) source.Set(Spatializer::P_BAND_GAIN_LOW+b,b==mode?1.f:0.f);
            const int warmup=rate/2,measured=rate/2,total=warmup+measured;
            std::vector<float> input(2048),output(2048);double sum=0,lr=0;int bad=0;
            for(int start=0;start<total;)
            {
                const int count=std::min(1024,total-start);
                for(int i=0;i<count;++i) input[2*i]=input[2*i+1]=static_cast<float>(0.125*std::sin(2*Pi*frequency*(start+i)/rate));
                source.Process(input.data(),output.data(),count);
                for(int i=0;i<count;++i) if(start+i>=warmup)
                {
                    sum+=static_cast<double>(output[2*i])*output[2*i];lr=std::max(lr,std::abs(static_cast<double>(output[2*i])-output[2*i+1]));
                    if(!std::isfinite(output[2*i])||!std::isfinite(output[2*i+1])) ++bad;
                }
                start+=count;
            }
            const double rms=std::sqrt(sum/measured),db=20*std::log10(std::max(rms/(0.125/std::sqrt(2.0)),1e-30));
            file<<rate<<','<<frequency<<','<<(mode<0?"neutral":"band_"+std::to_string(mode))<<','<<warmup<<','<<measured<<",0.125,"<<rms<<','<<db<<','<<lr<<','<<bad<<'\n';
            if(mode<0) neutralMaximum=std::max(neutralMaximum,std::abs(db));channelMaximum=std::max(channelMaximum,lr);allBad+=bad;
        }
    }
    Check("four_band_neutral_max_abs_db",neutralMaximum,0.005,"steady sine RMS; engineering regression tolerance not a perceptual threshold");
    Check("four_band_identical_channels",channelMaximum,0,"identical stereo inputs");
    Check("four_band_finite_outputs",allBad,0,"all frequency-response runs");
}

void Benchmark(int repeats,int blocks)
{
    auto file=Csv("benchmark.csv","multiband,moving,smoothing_ms,sources,sample_rate,block_frames,repeat,measured_blocks,mean_group_ms,p95_group_ms,max_group_ms,ns_per_source_frame,serial_wall_time_to_audio_duration_ratio,output_checksum");
    auto raw=Csv("benchmark_blocks.csv","multiband,moving,sources,block_frames,repeat,block_index,group_wall_time_ms");
    for(int blockFrames:{256,1024}) for(int count:{1,8,16,32}) for(int bands:{0,1}) for(int moving:{0,1})
    {
        std::vector<std::unique_ptr<Source>> sources;
        for(int s=0;s<count;++s)
        {
            auto source=std::make_unique<Source>();source->Set(Spatializer::P_MULTIBAND,static_cast<float>(bands));source->Set(Spatializer::P_MOTION_SMOOTHING_MS,5);source->Pan(0.2);sources.push_back(std::move(source));
        }
        std::vector<float> input(blockFrames*2),output(blockFrames*2);
        for(int n=0;n<blockFrames;++n) input[2*n]=input[2*n+1]=static_cast<float>(0.125*std::sin(2*Pi*1000*n/48000));
        for(int warmup=0;warmup<32;++warmup) for(auto& source:sources) source->Process(input.data(),output.data(),blockFrames);
        for(int repeat=0;repeat<repeats;++repeat)
        {
            std::vector<double> durations;durations.reserve(blocks);double checksum=0;
            for(int block=0;block<blocks;++block)
            {
                if(moving) for(int s=0;s<count;++s) sources[s]->Pan(std::sin((repeat*blocks+block)*0.13+s*0.1));
                // No allocation, logging, disk IO or input generation in the measured region.
                const auto start=std::chrono::steady_clock::now();
                for(auto& source:sources) source->Process(input.data(),output.data(),blockFrames);
                const auto stop=std::chrono::steady_clock::now();
                durations.push_back(std::chrono::duration<double,std::milli>(stop-start).count());
                checksum+=output[2*(blockFrames/3)];
            }
            double total=0;
            for(int b=0;b<blocks;++b) {total+=durations[b];raw<<bands<<','<<moving<<','<<count<<','<<blockFrames<<','<<repeat<<','<<b<<','<<durations[b]<<'\n';}
            std::sort(durations.begin(),durations.end());
            const double p95=durations[static_cast<size_t>(std::ceil(0.95*blocks))-1];
            benchmarkSink+=checksum;
            file<<bands<<','<<moving<<",5,"<<count<<",48000,"<<blockFrames<<','<<repeat<<','<<blocks<<','<<total/blocks<<','<<p95<<','<<durations.back()<<','<<total*1e6/(static_cast<double>(blocks)*blockFrames*count)<<','<<total/(1000.0*blocks*blockFrames/48000)<<','<<checksum<<'\n';
        }
    }
}

int wmain(int argc,wchar_t** argv)
{
    HMODULE library=nullptr;
    try
    {
        Require(argc==5,"usage: SpatializerEvaluation.exe dll output_directory repeats blocks");
        outputDirectory=fs::path(argv[2]);fs::create_directories(outputDirectory);
        checks=Csv("validations.csv","test,status,actual_error_or_failure_count,limit,detail");
        library=LoadLibraryW(argv[1]);Require(library!=nullptr,"LoadLibraryW failed");
        using GetDefinitions=int (AUDIO_CALLING_CONVENTION*)(UnityAudioEffectDefinition***);
        auto getDefinitions=reinterpret_cast<GetDefinitions>(GetProcAddress(library,"UnityGetAudioEffectDefinitions"));
        snapshot=reinterpret_cast<Snapshot>(GetProcAddress(library,"Spatializer_GetCurrentSpatialization"));
        Require(getDefinitions && snapshot,"required DLL exports missing");
        UnityAudioEffectDefinition** definitions=nullptr;const int count=getDefinitions(&definitions);
        for(int i=0;i<count;++i) if(std::strcmp(definitions[i]->name,"Demo Spatializer")==0) definition=definitions[i];
        Require(definition!=nullptr,"Demo Spatializer not found");
        Require(definition->create&&definition->release&&definition->process&&definition->reset&&definition->setfloatparameter&&definition->getfloatparameter,"required callbacks missing");
        std::cout<<"Subject: "<<definition->name<<"; API="<<definition->apiversion<<"; parameters="<<definition->numparameters<<"; MSVC="<<_MSC_FULL_VER<<'\n';
        ParameterChecks();std::cout<<"Parameter ABI and validation complete\n";
        GeometryChecks();StaticPanningChecks();std::cout<<"Geometry and static panning complete\n";
        MotionMeasurements();PartitionChecks();std::cout<<"Motion and partition measurements complete\n";
        AttenuationChecks();RegressionChecks();FrequencyMeasurements();std::cout<<"Attenuation callback and DSP regression complete\n";
        checks.flush();
        Benchmark(std::stoi(argv[3]),std::stoi(argv[4]));
        std::cout<<"Checks: "<<checkCount<<"; failures: "<<failures<<"; benchmark checksum: "<<benchmarkSink<<'\n';
        checks.close();FreeLibrary(library);return failures?1:0;
    }
    catch(const std::exception& e)
    {
        std::cerr<<"FATAL: "<<e.what()<<'\n';
        if(checks) {checks<<"fatal,FAIL,1,0,aborted; inspect execution.txt\n";checks.flush();}
        if(library) FreeLibrary(library);return 2;
    }
}
