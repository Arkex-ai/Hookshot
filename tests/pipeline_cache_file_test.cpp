// Authored tests of the production storage helper; no Vulkan/game data.
#include "pipeline_cache_file.h"
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(int argc,char** argv){
    int checks=0;CHECK(argc==2);
    std::string path=std::string(argv[1])+"/owned-\xC3\xA9-cache.bin";
    auto valid=[](const uint8_t* h,uint64_t n){return n==128 && h[0]==42;};
    CHECK(mkwvk::PipelineCacheFile::read(path,valid).empty());
    CHECK(!mkwvk::PipelineCacheFile::read(path,valid).rejected());
    {
        auto out=mkwvk::PipelineCacheFile::writable(path,256);CHECK(!out.empty());
        memset(out.data(),42,256);CHECK(!out.publish(257));CHECK(out.publish(128));
    }
    {
        auto in=mkwvk::PipelineCacheFile::read(path,valid);CHECK(in.size()==128);
        for(size_t i=0;i<in.size();++i)CHECK(in.data()[i]==42);
        // A discarded producer leaves the old destination unchanged.
        {auto abandoned=mkwvk::PipelineCacheFile::writable(path,512);CHECK(!abandoned.empty());memset(abandoned.data(),99,512);}
        auto moved=std::move(in);CHECK(in.empty());CHECK(moved.size()==128);
        auto replaced=mkwvk::PipelineCacheFile::writable(path,256);CHECK(!replaced.empty());memset(replaced.data(),43,256);
#if defined(_WIN32)
        CHECK(!replaced.publish(128)); // Windows refuses replacing an active mapped destination.
#else
        CHECK(replaced.publish(128));
#endif
        CHECK(moved.data()[0]==42); // Existing view retains old bytes.
    }
#if defined(_WIN32)
    CHECK(!mkwvk::PipelineCacheFile::read(path,valid).empty());
    {auto retry=mkwvk::PipelineCacheFile::writable(path,128);CHECK(!retry.empty());memset(retry.data(),43,128);CHECK(retry.publish(128));}
#endif
    CHECK(mkwvk::PipelineCacheFile::read(path,valid).empty());
    CHECK(mkwvk::PipelineCacheFile::read(path,valid).rejected());
    auto current=[](const uint8_t* h,uint64_t n){return n==128&&h[0]==43;};
    CHECK(!mkwvk::PipelineCacheFile::read(path,current).empty());
    CHECK(mkwvk::PipelineCacheFile::writable(path,0).empty());
    CHECK(mkwvk::PipelineCacheFile::read(std::string(argv[1]),current).empty());
#if !defined(_WIN32)
    {
        auto snapshot=mkwvk::PipelineCacheFile::read(path,current);CHECK(!snapshot.empty());
        CHECK(truncate(path.c_str(),0)==0);CHECK(snapshot.data()[127]==43);
    }
    CHECK(mkwvk::PipelineCacheFile::read(path,current).empty());
    std::string link=path+".link";CHECK(symlink(path.c_str(),link.c_str())==0);
    CHECK(mkwvk::PipelineCacheFile::read(link,current).empty());CHECK(unlink(link.c_str())==0);
#endif
    size_t leaked=0;
    for(const auto& entry:std::filesystem::directory_iterator(argv[1]))if(entry.path().filename().string().find(".tmp-")!=std::string::npos)++leaked;
    CHECK(leaked==0);
    printf("{\"assertions\":%d,\"productionHelper\":true,\"passed\":true}\n",checks);
    return 0;
}
