#include "ConsoleCompression.h"
#include "ConsoleLzx.h"
#include <zlib.h>
namespace console::compression {
std::vector<unsigned char> decodeRle(std::span<const unsigned char> source,std::size_t expectedBytes){
    if(expectedBytes>PS3_MAX_SAVE_BYTES || source.size()>PS3_MAX_SAVE_BYTES*2)throw IoError("RLE data exceeds save capacity");
    std::vector<unsigned char> result;result.reserve(expectedBytes);
    for(std::size_t at=0;at<source.size();){
        unsigned char value=source[at++];unsigned count=1;
        if(value==255){
            if(at==source.size())throw IoError("Truncated RLE escape");
            count=static_cast<unsigned>(source[at++])+1;
            if(count>=4){if(at==source.size())throw IoError("Truncated RLE run");value=source[at++];}
        }
        if(count>expectedBytes-result.size())throw IoError("RLE output exceeds declared size");result.insert(result.end(),count,value);
    }
    if(result.size()!=expectedBytes)throw IoError("RLE output does not match declared size");return result;
}
namespace {
int windowBits(ESavePlatform platform){
    switch(platform){
    case SAVE_FILE_PLATFORM_PS3:return -15;
    case SAVE_FILE_PLATFORM_WIN64:case SAVE_FILE_PLATFORM_XBONE:case SAVE_FILE_PLATFORM_PS4:case SAVE_FILE_PLATFORM_PSVITA:return 15;
    case SAVE_FILE_PLATFORM_X360:throw IoError("Xbox LZX compression is not ported");
    default:throw IoError("Unsupported compression platform");
    }
}
std::vector<unsigned char> deflateBytes(std::span<const unsigned char> source,int window){
    z_stream stream{};if(deflateInit2(&stream,Z_DEFAULT_COMPRESSION,Z_DEFLATED,window,8,Z_DEFAULT_STRATEGY)!=Z_OK)throw IoError("Cannot initialize console deflate stream");
    struct End{z_stream& stream;~End(){deflateEnd(&stream);}} end{stream};
    std::vector<unsigned char> result(deflateBound(&stream,source.size()));
    stream.next_in=const_cast<Bytef*>(source.data());stream.avail_in=static_cast<uInt>(source.size());stream.next_out=result.data();stream.avail_out=static_cast<uInt>(result.size());
    if(deflate(&stream,Z_FINISH)!=Z_STREAM_END)throw IoError("Console deflate failed");result.resize(stream.total_out);return result;
}
std::vector<unsigned char> inflateBytes(std::span<const unsigned char> source,std::size_t maxBytes,int window){
    z_stream stream{};if(inflateInit2(&stream,window)!=Z_OK)throw IoError("Cannot initialize console inflate stream");
    struct End{z_stream& stream;~End(){inflateEnd(&stream);}} end{stream};
    // One extra byte permits detection of an oversized stream and completion of
    // the zero-length stream without treating a full output buffer as success.
    std::vector<unsigned char> result(maxBytes+1);
    stream.next_in=const_cast<Bytef*>(source.data());stream.avail_in=static_cast<uInt>(source.size());stream.next_out=result.data();stream.avail_out=static_cast<uInt>(result.size());
    int status=inflate(&stream,Z_FINISH);
    if(status!=Z_STREAM_END || stream.avail_in || stream.total_out>maxBytes)throw IoError("Invalid, truncated or oversized console deflate data");
    result.resize(stream.total_out);return result;
}
}
std::vector<unsigned char> compressChunk(std::span<const unsigned char> source,ESavePlatform platform){
    const int window=windowBits(platform);auto rle=encodeRle(source);auto result=deflateBytes(rle,window);
    if(platform==SAVE_FILE_PLATFORM_PS3){
        // EdgeZLib::Compress writes a big-endian size then raw DEFLATE.
        result.insert(result.begin(),4,0);SaveWire::write(result,0,4,rle.size(),SaveByteOrder::Big);
    }
    return result;
}
std::vector<unsigned char> decompressChunk(std::span<const unsigned char> source,std::size_t expectedBytes,ESavePlatform platform,bool useRle){
    if(expectedBytes>PS3_MAX_SAVE_BYTES || source.size()>PS3_MAX_SAVE_BYTES*2+4096)throw IoError("Compressed data exceeds save capacity");
    if(platform==SAVE_FILE_PLATFORM_X360){
        // The source's Xbox region files use framed XMem LZX. The optional
        // high-bit length flag adds its byte-run layer after LZX decoding.
        try{
            auto decoded=decodeConsoleLzx(source,useRle?expectedBytes*2:expectedBytes);
            if(useRle)return decodeRle(decoded,expectedBytes);
            if(decoded.size()!=expectedBytes)throw IoError("Xbox LZX output size mismatch");
            return decoded;
        }catch(const std::runtime_error& error){throw IoError(error.what());}
    }
    const int window=windowBits(platform);std::size_t limit=useRle?expectedBytes*2:expectedBytes;
    std::size_t declared=0;
    if(platform==SAVE_FILE_PLATFORM_PS3){declared=SaveWire::read(source,0,4,SaveByteOrder::Big);if(declared>limit)throw IoError("PS3 deflate header exceeds output capacity");source=source.subspan(4);limit=declared;}
    auto decoded=inflateBytes(source,limit,window);
    if(platform==SAVE_FILE_PLATFORM_PS3 && decoded.size()!=declared)throw IoError("PS3 deflate size mismatch");
    if(useRle)return decodeRle(decoded,expectedBytes);
    if(decoded.size()!=expectedBytes)throw IoError("Chunk decompression size mismatch");return decoded;
}
std::vector<unsigned char> decompressPs3Package(std::span<const unsigned char> source,std::size_t expectedBytes){
    try{return decompressChunk(source,expectedBytes,SAVE_FILE_PLATFORM_PS3);}
    catch(const IoError&){
        if(source.empty() || source.back()!=0)throw;
        return decompressChunk(source.first(source.size()-1),expectedBytes,SAVE_FILE_PLATFORM_PS3);
    }
}
}
