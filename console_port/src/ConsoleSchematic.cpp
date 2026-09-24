#include "ConsoleSchematic.h"
#include "ConsoleLzx.h"
#include "ConsoleCompression.h"
#include "NbtIo.h"
#include <algorithm>
#include <cmath>
namespace console {
ConsoleSchematic ConsoleSchematic::read(std::span<const unsigned char> bytes){
 if(bytes.size()>64*1024*1024)throw IoError("Schematic file too large");
 std::size_t at=0;auto integer=[&](){auto n=SaveWire::read(bytes,at,4,SaveByteOrder::Big);at+=4;return n;};
 auto version=integer();if(version<1 || version>2)throw IoError("Unsupported schematic version");
 int compression=2;if(version==2){if(at==bytes.size())throw IoError("Missing schematic compression");compression=bytes[at++];}
 ConsoleSchematic result;result.width_=integer();result.height_=integer();result.depth_=integer();
 if(result.width_<=0 || result.width_>864 || result.depth_<=0 || result.depth_>864 || result.height_<=0 || result.height_>256 || result.height_%2)
  throw IoError("Invalid schematic dimensions");
 const std::size_t count=std::size_t(result.width_)*result.height_*result.depth_,expected=count*3/2;
 if(expected>64*1024*1024)throw IoError("Schematic exceeds output capacity");
 auto length=integer();if(length>bytes.size()-at)throw IoError("Truncated schematic payload");auto payload=bytes.subspan(at,length);at+=length;
 if(compression==0){result.bytes_.assign(payload.begin(),payload.end());if(result.bytes_.size()!=expected)throw IoError("Schematic size mismatch");}
 else if(compression==1)result.bytes_=compression::decodeRle(payload,expected);
 else if(compression==2)result.bytes_=compression::decodeRle(decodeConsoleLzx(payload,expected*2),expected);
 else if(compression==4)result.bytes_=compression::decompressPs3Package(payload,expected);
 else throw IoError("Unsupported schematic compression");
 auto remaining=bytes.subspan(at);ByteArrayInputStream stream(byteArray(const_cast<unsigned char*>(remaining.data()),remaining.size()));
 struct Detach{ByteArrayInputStream& stream;~Detach(){stream.reset();}}detach{stream};DataInputStream input(&stream);
 result.tags.reset(NbtIo::read(&input));if(!result.tags || stream.read()!=-1)throw IoError("Invalid schematic NBT trailer");
 return result;
}
std::unique_ptr<CompoundTag> ConsoleSchematic::tagsForChunk(int cx,int cz,int ox,int oy,int oz)const{
 if(!tags || cx < -1000000 || cx>1000000 || cz < -1000000 || cz>1000000 ||
    ox < -30000000 || ox>30000000 || oz < -30000000 || oz>30000000 || oy<0 || oy>256)
  throw std::out_of_range("Schematic entity placement coordinate");
 auto result=std::make_unique<CompoundTag>();
 for(const auto* name:{L"TileEntities",L"Entities"}){
  auto output=std::make_unique<TagList>();bool tile=std::wstring(name)==L"TileEntities";
  if(tags->contains(name)){
   auto* input=tags->getList(name);
   for(int i=0;i<input->size();++i){
    auto* source=dynamic_cast<CompoundTag*>(input->get(i));if(!source)throw IoError("Schematic entity must be an NBT compound");
    const auto id=source->getString(L"id");bool hanging=!tile && (id==L"Painting" || id==L"ItemFrame");
    double x,y,z;TagList* pos=nullptr;
    if(tile){x=source->getInt(L"x");y=source->getInt(L"y");z=source->getInt(L"z");}
    else {
     if(!source->contains(L"Pos"))throw IoError("Schematic entity has no position");pos=source->getList(L"Pos");
     if(pos->size()!=3)throw IoError("Schematic entity position must contain three doubles");
     double xyz[3];for(int n=0;n<3;++n){auto* value=dynamic_cast<DoubleTag*>(pos->get(n));
      if(!value || !std::isfinite(value->data) || std::abs(value->data)>30000000)throw IoError("Invalid schematic entity position");xyz[n]=value->data;}
     x=xyz[0];y=xyz[1];z=xyz[2];
     if(hanging){x=source->getInt(L"TileX");y=source->getInt(L"TileY");z=source->getInt(L"TileZ");}
    }
    x+=ox;y+=oy;z+=oz;
    // Match the original chunk ownership test, including its entity-only epsilon.
    const double epsilon=tile?0:.01;
    if(x+epsilon<cx*16 || x+epsilon>=cx*16+16 || z+epsilon<cz*16 || z+epsilon>=cz*16+16 || y+epsilon<0 || y+epsilon>=256)continue;
    std::unique_ptr<CompoundTag> copy(static_cast<CompoundTag*>(source->copy()));
    if(tile){copy->putInt(L"x",int(x));copy->putInt(L"y",int(y));copy->putInt(L"z",int(z));}
    else {
     auto shifted=std::make_unique<TagList>();const int offset[]={ox,oy,oz};
     for(int n=0;n<3;++n){auto value=std::make_unique<DoubleTag>(L"",static_cast<DoubleTag*>(pos->get(n))->data+offset[n]);shifted->add(value.get());value.release();}
     copy->put(L"Pos",shifted.get());shifted.release();
     if(hanging){copy->putInt(L"TileX",int(x));copy->putInt(L"TileY",int(y));copy->putInt(L"TileZ",int(z));}
    }
    output->add(copy.get());copy.release();
   }
  }
  result->put(name,output.get());output.release();
 }
 return result;
}
int ConsoleSchematic::block(int x,int y,int z)const{
 if(x<0 || x>=width_ || y<0 || y>=height_ || z<0 || z>=depth_)throw std::out_of_range("Schematic coordinate");
 return bytes_[(x*depth_+z)*height_+y];
}
int ConsoleSchematic::data(int x,int y,int z)const{
 (void)block(x,y,z);const int index=(x*depth_+z)*height_+y;return (bytes_[width_*depth_*height_+index/2]>>((index&1)*4))&15;
}
void ConsoleSchematic::apply(ChunkStorage& chunk,int cx,int cz,int ox,int oy,int oz)const{
 if(cx < -1000000 || cx>1000000 || cz < -1000000 || cz>1000000 || ox < -30000000 || ox>30000000 || oz < -30000000 || oz>30000000 || oy<0 || oy>256)
  throw std::out_of_range("Schematic placement coordinate");
 const int x0=std::max(ox,cx*16),x1=std::min(ox+width_,cx*16+16),z0=std::max(oz,cz*16),z1=std::min(oz+depth_,cz*16+16);
 for(int x=x0;x<x1;++x)for(int z=z0;z<z1;++z)for(int y=oy;y<std::min(256,oy+height_);++y)
 {
  const int lx=x-cx*16,lz=z-cz*16;
  chunk.blocks[(lx*16+lz)*256+y]=block(x-ox,y-oy,z-oz);
  chunk.metadata.set(lx,y,lz,data(x-ox,y-oy,z-oz));
  chunk.unsaved=true;
 }
 // Original schematic application copies block/data arrays first. The caller
 // must prepare lighting/heightmaps after all structures have been applied.
 chunk.hasGapsToCheck=true;
}
}
