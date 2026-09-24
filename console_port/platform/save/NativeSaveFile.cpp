#include "NativeSaveFile.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <system_error>
namespace console {
namespace {
[[noreturn]] void fail(const char* operation){int error=errno;throw IoError(std::string(operation)+": "+std::error_code(error,std::generic_category()).message());}
struct Descriptor {
    int fd=-1;
    explicit Descriptor(int value):fd(value){}
    ~Descriptor(){if(fd>=0)::close(fd);}
    Descriptor(const Descriptor&)=delete;
};
struct TemporaryFile {
    std::vector<char> name;
    int fd=-1;
    bool committed=false;
    TemporaryFile()=default;
    TemporaryFile(const TemporaryFile&)=delete;
    ~TemporaryFile(){if(fd>=0){::close(fd);if(!committed)::unlink(name.data());}}
};
void validPath(const std::filesystem::path& path){
    if(path.empty() || path.filename().empty() || path.native().find('\0')!=std::string::npos)throw IoError("Invalid native save path");
}
int sync(int fd){int result;do{result=::fsync(fd);}while(result<0 && errno==EINTR);return result;}
}
std::vector<unsigned char> NativeSaveFile::read(const std::filesystem::path& path){
    validPath(path);
    // Nonblocking open lets us reject a FIFO/device without waiting on it.
    Descriptor file(::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NONBLOCK));if(file.fd<0)fail("Opening console save");
    struct stat stat{};if(::fstat(file.fd,&stat)!=0)fail("Inspecting console save");
    if(!S_ISREG(stat.st_mode) || stat.st_size<12 || static_cast<std::uint64_t>(stat.st_size)>PS3_MAX_SAVE_BYTES)throw IoError("Invalid native console save file size or type");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(stat.st_size));
    std::size_t at=0;
    while(at<bytes.size()){
        auto count=::read(file.fd,bytes.data()+at,bytes.size()-at);
        if(count<0){if(errno==EINTR)continue;fail("Reading console save");}
        if(!count)throw IoError("Console save truncated during read");at+=static_cast<std::size_t>(count);
    }
    unsigned char trailing;ssize_t count;do{count=::read(file.fd,&trailing,1);}while(count<0 && errno==EINTR);
    if(count<0)fail("Finishing console save read");if(count)throw IoError("Console save grew during read");
    return bytes;
}
NativeSaveCommit NativeSaveFile::replace(const std::filesystem::path& path,std::span<const unsigned char> bytes){
    validPath(path);if(bytes.size()<12 || bytes.size()>PS3_MAX_SAVE_BYTES)throw IoError("Invalid native console save size");
    auto parent=path.parent_path();if(parent.empty())parent=".";
    // A fixed-length temp name also supports destination names near NAME_MAX.
    auto pattern=(parent/".console-save-XXXXXX").string();TemporaryFile temporary;
    temporary.name.assign(pattern.begin(),pattern.end());temporary.name.push_back(0);
    temporary.fd=::mkstemp(temporary.name.data());if(temporary.fd<0)fail("Creating temporary console save");
    if(::fcntl(temporary.fd,F_SETFD,FD_CLOEXEC)<0)fail("Securing temporary console save descriptor");
    std::size_t at=0;
    while(at<bytes.size()){
        auto count=::write(temporary.fd,bytes.data()+at,bytes.size()-at);
        if(count<0){if(errno==EINTR)continue;fail("Writing temporary console save");}
        if(!count)throw IoError("Console save write made no progress");at+=static_cast<std::size_t>(count);
    }
    if(sync(temporary.fd)!=0)fail("Syncing console save contents");
    if(::rename(temporary.name.data(),path.c_str())!=0)fail("Committing console save");
    temporary.committed=true;
    Descriptor directory(::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC));
    return {directory.fd>=0 && sync(directory.fd)==0};
}
}
