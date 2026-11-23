// Template matching on one-second averaged, full-resolution luma ROIs.
// References are either full-frame pictures or exact-size crops of the ROI.
// Configuration distinguishes station logos from programme-only keep markers.
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <unistd.h>
#include <vector>
#include "cm_overlay_template.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
// NVDEC gate, identical policy to cm-scan: enabled by the service, single
// session, healthy and cool card, software as the universal fallback.
struct GpuGate{bool allowed=false;int fd=-1;GpuGate()=default;GpuGate(GpuGate&&o)noexcept:allowed(o.allowed),fd(o.fd){o.fd=-1;}GpuGate&operator=(GpuGate&&o)noexcept{if(this!=&o){if(fd>=0)close(fd);allowed=o.allowed;fd=o.fd;o.fd=-1;}return *this;}GpuGate(const GpuGate&)=delete;GpuGate&operator=(const GpuGate&)=delete;~GpuGate(){if(fd>=0)close(fd);}};
static GpuGate acquire_gpu(){
    GpuGate gate;
    if(!std::getenv("CM_GPU_DECODE"))return gate;
    gate.fd=open("/dev/shm/cm-gpu.lock",O_CREAT|O_RDWR,0666);
    if(gate.fd<0){fprintf(stderr,"gpu gate: cannot open lock file: %s; using software\n",strerror(errno));gate.fd=-1;return gate;}
    if(flock(gate.fd,LOCK_EX|LOCK_NB)!=0){fprintf(stderr,"gpu gate: another decoder holds the GPU; using software\n");close(gate.fd);gate.fd=-1;return gate;}
    int worst=-1;
    if(FILE* probe=popen("nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits 2>/dev/null","r")){char line[32];while(fgets(line,sizeof(line),probe))worst=std::max(worst,atoi(line));pclose(probe);}
    int limit=85;
    if(const char* limit_text=std::getenv("CM_GPU_MAX_TEMP")){const int value=atoi(limit_text);if(value>0)limit=value;}
    if(worst<0){fprintf(stderr,"gpu gate: GPU does not answer; using software\n");close(gate.fd);gate.fd=-1;return gate;}
    if(worst>=limit){fprintf(stderr,"gpu gate: GPU at %dC >= %dC; using software\n",worst,limit);close(gate.fd);gate.fd=-1;return gate;}
    gate.allowed=true;return gate;
}
struct Image {int w,h;std::vector<unsigned char> y;};
static Image read_pgm(const char* path){
    std::ifstream f(path,std::ios::binary);std::string magic;int max;Image im;
    f>>magic>>im.w>>im.h>>max;check(magic=="P5"&&max==255,"reference requires 8-bit P5 PGM");f.get();
    im.y.resize(im.w*im.h);f.read((char*)im.y.data(),im.y.size());check(bool(f),"reference image truncated");return im;
}
struct Matcher {
    int x,y,w,h,pad=6,refw,refh,bin=-1,count=0;
    std::vector<double> reference,accum;
    double template_energy=0;int mask_count=0;
    FILE* output;
    Matcher(const std::vector<Image>& images,int ox,int oy,int ow,int oh,int width,int height,const char* output_path):refw(width),refh(height){
        x=ox;y=oy;w=ow;h=oh;
        check(x>=pad&&y>=pad&&x+w+pad<width&&y+h+pad<height,"ROI outside frame");
        reference.resize(w*h*2);accum.resize((w+2*pad)*(h+2*pad));
        // A reference may be a full-frame picture (scaled to the frame) or an
        // exact-size crop of this ROI, as produced by the overlay learner.
        auto at=[&](const Image& im,int px,int py){
            if(im.w==w&&im.h==h){int lx=std::clamp(px-x,0,w-1),ly=std::clamp(py-y,0,h-1);return double(im.y[ly*w+lx]);}
            int sx=std::min(im.w-1,int((px+0.5)*im.w/width));int sy=std::min(im.h-1,int((py+0.5)*im.h/height));return double(im.y[sy*im.w+sx]);};
        std::vector<OverlayPatch> patches;patches.reserve(images.size());
        for(const auto& im:images){OverlayPatch patch;patch.w=w;patch.h=h;patch.y.resize(size_t(w)*h);
            for(int j=0;j<h;++j)for(int i=0;i<w;++i)
                patch.y[size_t(j)*w+i]=(unsigned char)std::clamp(at(im,x+i,y+j),0.0,255.0);
            patches.push_back(std::move(patch));}
        mask_count=build_overlay_template(patches,w,h,reference,template_energy);
        // build_overlay_template has already zeroed uniform rows and columns.
        std::vector<unsigned char> mask(w*h);
        for(int p=0;p<w*h;++p){double e=reference[p*2]*reference[p*2]+reference[p*2+1]*reference[p*2+1];mask[p]=std::min(255,int(std::sqrt(e)*8));}
        check(mask_count>=kMinLogoMaskPixels&&template_energy>0,"insufficient reference logo edges");
        output=fopen(output_path,"w");check(output,"output file");
        std::string maskpath=std::string(output_path)+".mask.pgm";FILE* mp=fopen(maskpath.c_str(),"wb");check(mp,"mask output");fprintf(mp,"P5\n%d %d\n255\n",w,h);fwrite(mask.data(),1,mask.size(),mp);fclose(mp);
        fprintf(output,"bin_start,samples,correlation,peak_margin,gradient_ratio\n");
        fprintf(stderr,"ROI=%d,%d,%d,%d reference_mask_pixels=%d\n",x,y,w,h,mask_count);
    }
    double correlate(int dx,int dy,double* ratio=nullptr){
        double dot=0,energy=0;int stride=w+2*pad;
        for(int j=1;j<h-1;++j)for(int i=1;i<w-1;++i){int p=j*w+i;double tx=reference[2*p],ty=reference[2*p+1];if(tx==0&&ty==0)continue;
            int k=(j+pad+dy)*stride+i+pad+dx;
            double gx=(accum[k+1]-accum[k-1])/count,gy=(accum[k+stride]-accum[k-stride])/count;
            dot+=gx*tx+gy*ty;energy+=gx*gx+gy*gy;
        }
        if(ratio)*ratio=std::sqrt(energy/template_energy);
        return energy>1e-9?dot/std::sqrt(energy*template_energy):0;
    }
    void flush(){
        if(!count)return;
        double ratio;double on=correlate(0,0,&ratio),other=-1;
        for(auto delta:std::vector<std::pair<int,int>>{{-3,0},{3,0},{0,-3},{0,3},{-6,0},{6,0},{0,-6},{0,6}})other=std::max(other,correlate(delta.first,delta.second));
        fprintf(output,"%d,%d,%.8f,%.8f,%.8f\n",bin,count,on,on-other,ratio);
        std::fill(accum.begin(),accum.end(),0);count=0;
    }
    void frame(double pts,const AVFrame* f){
        check(f->width==refw&&f->height==refh,"resolution changed");check(f->format==AV_PIX_FMT_YUV420P||f->format==AV_PIX_FMT_NV12,"8-bit yuv420p or nv12 required");
        int next=int(std::floor(pts));if(next!=bin){flush();bin=next;}
        int stride=w+2*pad;
        for(int j=0;j<h+2*pad;++j){const unsigned char* row=f->data[0]+(y-pad+j)*f->linesize[0]+x-pad;for(int i=0;i<stride;++i)accum[j*stride+i]+=row[i];}
        ++count;
    }
    ~Matcher(){flush();fclose(output);}
};
int main(int argc,char** argv){try{
    check(argc>=8&&(argc-2)%6==0,"usage: logo-probe input.mp4 reference[;reference2] x y width height output.csv [more groups]");
    AVFormatContext* fmt=nullptr;check(avformat_open_input(&fmt,argv[1],nullptr,nullptr)>=0,"open input");check(avformat_find_stream_info(fmt,nullptr)>=0,"stream info");
    const char* origin_text=std::getenv("CM_TIME_ORIGIN");double time_origin=origin_text?std::stod(origin_text):0;
    const char* windows_path=std::getenv("CM_REVIEW_WINDOWS");std::vector<std::pair<double,double>> windows;
    if(windows_path){std::ifstream f(windows_path);check(bool(f),"open review windows");double a,b;while(f>>a>>b){check(b>a,"invalid review window");windows.emplace_back(a,b);}}
    int vi=av_find_best_stream(fmt,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);check(vi>=0,"video stream");auto* par=fmt->streams[vi]->codecpar;
    std::vector<std::unique_ptr<Matcher>> matchers;
    for(int i=2;i<argc;i+=6){
        // A weak candidate may have no consistent edges at all. That is a
        // property of the candidate, not of the recording, so keep the other
        // candidates running and leave an empty measurement behind.
        try{
            std::vector<Image> refs;std::stringstream paths(argv[i]);std::string path;
            while(std::getline(paths,path,';'))refs.push_back(read_pgm(path.c_str()));
            check(!refs.empty(),"missing references");
            matchers.emplace_back(std::make_unique<Matcher>(refs,atoi(argv[i+1]),atoi(argv[i+2]),atoi(argv[i+3]),atoi(argv[i+4]),par->width,par->height,argv[i+5]));
        }catch(const std::exception& e){
            fprintf(stderr,"candidate %s skipped: %s\n",argv[i+5],e.what());
            FILE* empty=fopen(argv[i+5],"w");
            if(empty){fprintf(empty,"bin_start,samples,correlation,peak_margin,gradient_ratio\n");fclose(empty);}
        }
    }
    // NVDEC offload: the *_cuvid decoders decode on the GPU and output NV12
    // system-memory frames; the gate decides whether the GPU is used at all,
    // and software remains the fallback for every refusal reason.
    AVCodecID vid=par->codec_id;
    static const std::map<AVCodecID,const char*> kCuvid={{AV_CODEC_ID_H264,"h264_cuvid"},{AV_CODEC_ID_HEVC,"hevc_cuvid"},{AV_CODEC_ID_MPEG2VIDEO,"mpeg2_cuvid"},{AV_CODEC_ID_VC1,"vc1_cuvid"}};
    AVCodecContext* ctx=nullptr;bool hw=false;
    auto cuvid=kCuvid.find(vid);
    std::unique_ptr<GpuGate> gpu;
    if(cuvid!=kCuvid.end()){gpu.reset(new GpuGate(acquire_gpu()));}
    if(cuvid!=kCuvid.end()&&gpu->allowed){const AVCodec* hwcodec=avcodec_find_decoder_by_name(cuvid->second);
        if(hwcodec){ctx=avcodec_alloc_context3(hwcodec);if(ctx&&avcodec_parameters_to_context(ctx,par)>=0&&avcodec_open2(ctx,hwcodec,nullptr)>=0)hw=true;else avcodec_free_context(&ctx);}}
    if(!hw){const AVCodec* codec=avcodec_find_decoder(vid);check(codec,"decoder");ctx=avcodec_alloc_context3(codec);avcodec_parameters_to_context(ctx,par);ctx->thread_count=4;check(avcodec_open2(ctx,codec,nullptr)>=0,"open codec");}
    else fprintf(stderr,"video decoder: %s (NVDEC)\n",cuvid->second);
    AVPacket* packet=av_packet_alloc();AVFrame* frame=av_frame_alloc();long count=0,scored=0;
    auto drain=[&](){int rc;while((rc=avcodec_receive_frame(ctx,frame))>=0){double pts=frame->best_effort_timestamp*av_q2d(fmt->streams[vi]->time_base)-time_origin;bool selected=!windows_path||std::any_of(windows.begin(),windows.end(),[&](auto span){return span.first<=pts&&pts<span.second;});if(selected){for(auto& matcher:matchers)matcher->frame(pts,frame);++scored;}++count;av_frame_unref(frame);}if(rc!=AVERROR(EAGAIN)&&rc!=AVERROR_EOF)fprintf(stderr,"decoder reported an error while draining\n");};
    int rc;while((rc=av_read_frame(fmt,packet))>=0){if(packet->stream_index==vi){
        // Damage in the tail of a recording must not lose the measurement.
        if(avcodec_send_packet(ctx,packet)>=0)drain();else fprintf(stderr,"skipped a damaged video packet\n");
    }av_packet_unref(packet);}check(rc==AVERROR_EOF,"read frame");avcodec_send_packet(ctx,nullptr);drain();
    fprintf(stderr,"decoded_video_frames=%ld analysed_video_frames=%ld\n",count,scored);av_frame_free(&frame);av_packet_free(&packet);avcodec_free_context(&ctx);avformat_close_input(&fmt);return 0;
}catch(const std::exception& e){fprintf(stderr,"ERROR: %s\n",e.what());return 1;}}
