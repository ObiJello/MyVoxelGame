#ifdef CONSOLE_ARCHIVE_REFERENCE
#include "FileHeaderWire.h"
#else
#include "ConsoleSaveArchive.h"
#endif
#include <iostream>
#include <vector>
#include <array>
#include <string>
int main(){
    std::vector<std::u16string> names={u"level.dat",u"region/r.0.0.mcr",u"maps/\U0001f30d.dat"};
    for(unsigned size:{0u,1u,257u,65536u}){
        std::vector<std::vector<unsigned char>> data(3);for(unsigned i=0;i<3;++i){data[i].resize(size+i);for(unsigned j=0;j<data[i].size();++j)data[i][j]=static_cast<unsigned char>(j*157+i);}
        std::vector<unsigned char> bytes;
#ifdef CONSOLE_ARCHIVE_REFERENCE
        FileHeaderWire header;std::array<FileEntry,3> entries;
        for(unsigned i=0;i<3;++i){std::copy(names[i].begin(),names[i].end(),entries[i].data.filename);entries[i].data.length=data[i].size();entries[i].data.startOffset=header.GetStartOfNextData();entries[i].data.lastModifiedTime=1234567890ll+i;header.fileTable.push_back(&entries[i]);}
        bytes.resize(header.GetFileSize());for(unsigned i=0;i<3;++i)std::copy(data[i].begin(),data[i].end(),bytes.begin()+entries[i].data.startOffset);header.WriteHeader(bytes.data());
#else
        ConsoleSaveArchive archive;for(unsigned i=0;i<3;++i){std::vector<std::uint16_t> units(names[i].begin(),names[i].end());archive.put(console::io::nativeString(units),data[i],1234567890ll+i);}bytes=archive.serialize(SAVE_FILE_PLATFORM_WIN64);
#endif
        std::uint64_t h=14695981039346656037ull;for(auto byte:bytes)h=(h^byte)*1099511628211ull;std::cout<<size<<' '<<bytes.size()<<' '<<h<<'\n';
    }
}
