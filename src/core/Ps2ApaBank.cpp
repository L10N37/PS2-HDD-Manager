#include "Ps2ApaBank.h"
#include "Ps2Apa.h"
#include "Ps2HddLayout.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#ifdef __linux__
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif
namespace {
void le16(unsigned char *p, std::uint16_t v){p[0]=v;p[1]=v>>8;}
void le32(unsigned char *p, std::uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
std::array<unsigned char,Ps2::Apa::HeaderSize> makeMbr(){
    std::array<unsigned char,Ps2::Apa::HeaderSize> h{};
    le32(h.data()+4,Ps2::Apa::Magic); std::memcpy(h.data()+16,"__mbr",5);
    le32(h.data()+64,0); le32(h.data()+68,0x40000U);
    le16(h.data()+72,Ps2::Apa::MbrPartitionType); le16(h.data()+74,0);
    le32(h.data()+76,0); le32(h.data()+96,0x201U);
    std::memcpy(h.data()+256,"Sony Computer Entertainment Inc.",32);
    le32(h.data()+288,2U); le32(h.data()+292,0); le32(h.data()+304,0); le32(h.data()+308,0);
    le32(h.data(),Ps2::Apa::CalculateChecksum(h.data(),h.size())); return h;
}
#ifdef __linux__
void readAt(int fd,void *buf,std::size_t size,std::uint64_t off,const std::string &path){
    auto *b=(unsigned char*)buf; std::size_t done=0; while(done<size){
        ssize_t n=pread(fd,b+done,size-done,(off_t)(off+done));
        if(n<0) throw std::runtime_error("Reading "+path+" failed: "+std::strerror(errno));
        if (n == 0)
            throw std::runtime_error("Incomplete APA bank read from " + path);
        done += static_cast<std::size_t>(n);
    }
}
void writeAt(int fd,const void *buf,std::size_t size,std::uint64_t off,const std::string &path){
    auto *b=(const unsigned char*)buf; std::size_t done=0; while(done<size){
        ssize_t n=pwrite(fd,b+done,size-done,(off_t)(off+done));
        if(n<0) throw std::runtime_error("Writing "+path+" failed: "+std::strerror(errno));
        if (n == 0)
            throw std::runtime_error("Zero-length APA bank write to " + path);
        done += static_cast<std::size_t>(n);
    }
}
#endif
}
namespace Ps2 {
void ApaBank::InitializeGamesOnly(const std::string &path,std::uint64_t bytes,std::uint32_t bank,bool overwrite){
    if(bank==0) throw std::invalid_argument("Games-only initialization refuses Bank 0.");
    if(bank>=HddLayoutPlanner::MaximumBankCount) throw std::invalid_argument("Bank index exceeds manager safety limit.");
    if(bytes%HddLayoutPlanner::SectorSize) throw std::invalid_argument("Disk size is not 512-byte aligned.");
    const std::uint64_t total=bytes/HddLayoutPlanner::SectorSize;
    const std::uint64_t base=(std::uint64_t)bank*HddLayoutPlanner::BankBoundarySectors;
    if(base>=total) throw std::invalid_argument("Requested bank is outside the disk.");
    const std::uint64_t count=std::min(HddLayoutPlanner::BankBoundarySectors,total-base);
    if(count<HddLayoutPlanner::MinimumBankSizeSectors) throw std::invalid_argument("Upper bank is smaller than 128 MiB.");
#ifdef __linux__
    int fd=open(path.c_str(),O_RDWR|O_CLOEXEC|O_SYNC); if(fd<0) throw std::runtime_error("Opening "+path+" failed: "+std::strerror(errno));
    try{
        const std::uint64_t off=base*HddLayoutPlanner::SectorSize;
        std::array<unsigned char,Apa::HeaderSize> old{}; readAt(fd,old.data(),old.size(),off,path);
        auto oldInfo=Apa::ParseHeader(old.data(),old.size(),true);
        if(oldInfo.state==ApaHeaderState::Valid&&!overwrite) throw std::runtime_error("Bank already has a valid APA MBR.");
        auto h=makeMbr(); writeAt(fd,h.data(),h.size(),off,path); if(fsync(fd)!=0) throw std::runtime_error("fsync failed after bank initialization.");
        std::array<unsigned char,Apa::HeaderSize> verify{}; readAt(fd,verify.data(),verify.size(),off,path);
        auto info=Apa::ParseHeader(verify.data(),verify.size(),true);
        if(info.state!=ApaHeaderState::Valid||info.id!="__mbr"||info.start!=0||info.length!=0x40000U||info.next!=0||info.previous!=0)
            throw std::runtime_error("Games-only APA bank verification failed.");
    }catch(...){close(fd);throw;} close(fd);
#else
    (void)path;(void)bytes;(void)bank;(void)overwrite; throw std::runtime_error("Bank initialization is Linux-only.");
#endif
}}
