#include "ConsoleDlcPack.h"
#include <stdexcept>
#include <set>
namespace console {
namespace {
// DLCManager::processDLCDataFile walks two variable-length structure tables,
// then parameter tables and data blobs. Console structs include four trailing
// bytes beyond the counted UTF-16 units; records are not rounded to alignment.
struct Reader {
    std::span<const unsigned char> bytes;std::size_t offset=0;bool big=true;
    std::span<const unsigned char> take(std::size_t length){
        if(length>bytes.size()-offset)throw std::runtime_error("Truncated console DLC pack");
        auto result=bytes.subspan(offset,length);offset+=length;return result;
    }
    std::uint32_t integer(){auto b=take(4);std::uint32_t value=0;
        for(int i=0;i<4;++i)value=(value<<8)|b[big?i:3-i];return value;
    }
    std::uint32_t count(){auto n=integer();if(n>65536)throw std::runtime_error("Oversized DLC table");return n;}
    std::u16string string(){auto length=count();auto b=take(std::size_t(length)*2+4);std::u16string result;
        result.reserve(length);
        for(std::size_t i=0;i<length;++i){auto a=b[i*2],c=b[i*2+1];char16_t unit=big?(a<<8)|c:(c<<8)|a;
            if(!unit)throw std::runtime_error("Embedded NUL in DLC string");result+=unit;
        }
        if(b[length*2] || b[length*2+1])throw std::runtime_error("Unterminated DLC string");
        // Retain UTF-16 code units, but reject unpaired surrogates.
        for(std::size_t i=0;i<result.size();++i){auto c=result[i];
            if(c>=0xd800 && c<=0xdbff){if(++i>=result.size() || result[i]<0xdc00 || result[i]>0xdfff)throw std::runtime_error("Invalid DLC UTF-16");}
            else if(c>=0xdc00 && c<=0xdfff)throw std::runtime_error("Invalid DLC UTF-16");
        }
        return result;
    }
    std::map<std::uint32_t,std::u16string> parameters(){std::map<std::uint32_t,std::u16string> result;
        auto n=count();for(std::uint32_t i=0;i<n;++i){auto id=integer();auto value=string();
            if(!result.emplace(id,std::move(value)).second)throw std::runtime_error("Duplicate DLC parameter");
        }return result;
    }
};
}
ConsoleDlcPack ConsoleDlcPack::read(std::span<const unsigned char> bytes){
    if(bytes.size()<12 || bytes.size()>64*1024*1024)throw std::runtime_error("Invalid DLC pack size");
    Reader reader{bytes};
    auto version=reader.integer();
    if(version==0x03000000){reader.big=false;version=3;}
    if(version!=3)throw std::runtime_error("Unsupported DLC pack version");
    ConsoleDlcPack result;result.parameterNames=reader.parameters();
    auto count=reader.count();std::vector<std::uint32_t> sizes;std::set<std::u16string> names;
    for(std::uint32_t i=0;i<count;++i){
        auto size=reader.integer(),type=reader.integer();auto name=reader.string();
        if(name.empty() || !names.insert(name).second)throw std::runtime_error("Empty or duplicate DLC entry name");
        sizes.push_back(size);result.entries.push_back({type,std::move(name),{}, {}});
    }
    for(std::size_t i=0;i<result.entries.size();++i){auto& entry=result.entries[i];entry.parameters=reader.parameters();
        auto body=reader.take(sizes[i]);entry.bytes.assign(body.begin(),body.end());
    }
    if(reader.offset!=bytes.size())throw std::runtime_error("Trailing data in DLC pack");
    return result;
}
}
