#include "ConsoleGameRuleFile.h"
#include "ConsoleLzx.h"
#include "ConsoleCompression.h"
#include "DataInputStream.h"
#include "ByteArrayInputStream.h"
#include <functional>
namespace console {
ConsoleGameRuleFile ConsoleGameRuleFile::read(std::span<const unsigned char> bytes){
 if(bytes.size()>64*1024*1024)throw IoError("GRF file exceeds capacity");
 std::size_t at=0;auto version=SaveWire::read(bytes,at,2,SaveByteOrder::Big);at+=2;
 if(version>1)throw IoError("Unsupported GRF version");
 int compression=0;
 if(version==0)at=16;
 else {if(bytes.size()<11)throw IoError("Truncated GRF header");compression=bytes[at];at+=9;}
 if(at>bytes.size())throw IoError("Truncated GRF header");
 std::vector<unsigned char> decoded;
 if(compression){
  auto expected=SaveWire::read(bytes,at,4,SaveByteOrder::Big);at+=4;
  auto length=SaveWire::read(bytes,at,4,SaveByteOrder::Big);at+=4;
  if(length!=bytes.size()-at || expected>32*1024*1024)throw IoError("Invalid GRF payload size");
  if(compression==2)decoded=compression::decodeRle(decodeConsoleLzx(bytes.subspan(at,length),expected*2),expected);
  else if(compression==4)decoded=compression::decompressPs3Package(bytes.subspan(at,length),expected);
  else if(compression==1){decoded.assign(bytes.begin()+at,bytes.end());if(decoded.size()!=expected)throw IoError("GRF passthrough size mismatch");}
  else throw IoError("Unsupported GRF compression");
 }else decoded.assign(bytes.begin()+at,bytes.end());
 ByteArrayInputStream stream(byteArray(decoded.data(),decoded.size()));
 struct Detach{ByteArrayInputStream& stream;~Detach(){stream.reset();}}detach{stream};DataInputStream input(&stream);
 auto count=[&](){int value=input.readInt();if(value<0 || value>65536)throw IoError("Invalid GRF record count");return value;};
 std::vector<std::wstring> strings;for(int i=0,n=count();i<n;++i)strings.push_back(input.readUTF());
 auto name=[&]()->std::wstring{int id=input.readInt();if(id<0 || std::size_t(id)>=strings.size())throw IoError("Invalid GRF string index");return strings[id];};
 ConsoleGameRuleFile result;
 for(int i=0,n=count();i<n;++i){auto filename=input.readUTF();int length=input.readInt();
  if(length<0 || length>32*1024*1024 || std::size_t(length)>decoded.size())throw IoError("Invalid embedded schematic length");
  std::vector<unsigned char> data(length);input.readFully(byteArray(data.data(),data.size()));
  if(!result.files.emplace(filename,std::move(data)).second)throw IoError("Duplicate GRF embedded file");
 }
 unsigned nodes=0;
 std::function<ConsoleGameRuleNode(unsigned)> node=[&](unsigned depth){
  if(depth>128 || ++nodes>65536)throw IoError("GRF rule tree exceeds capacity");
  ConsoleGameRuleNode rule;rule.name=name();
  for(int i=0,n=count();i<n;++i){auto key=name();rule.attributes[key]=input.readUTF();}
  for(int i=0,n=count();i<n;++i)rule.children.push_back(node(depth+1));
  return rule;
 };
 for(int i=0,n=count();i<n;++i)result.rules.push_back(node(0));
 // Shipped GRFs include zero-filled backing-buffer capacity after the rule tree.
 // The original reader stops after the roots; reject unexplained nonzero tails.
 for(int tail=stream.read();tail!=-1;tail=stream.read())if(tail!=0)throw IoError("Nonzero trailing GRF content");
 return result;
}
}
