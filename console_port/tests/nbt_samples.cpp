#include "NbtIo.h"
#include <iostream>
#include <memory>
#include <bit>
#include <limits>
#ifdef CONSOLE_NBT_REFERENCE
// Archived array tags leave loaded/copied arrays to the caller to free.
static void freeArrays(Tag* tag){
    if(tag->getId()==Tag::TAG_Byte_Array)delete[] static_cast<ByteArrayTag*>(tag)->data.data;
    else if(tag->getId()==Tag::TAG_Int_Array)delete[] static_cast<IntArrayTag*>(tag)->data.data;
    else if(tag->getId()==Tag::TAG_List){auto* l=static_cast<ListTag<Tag>*>(tag);for(int i=0;i<l->size();++i)freeArrays(l->get(i));}
    else if(tag->getId()==Tag::TAG_Compound){std::unique_ptr<std::vector<Tag*>> children(static_cast<CompoundTag*>(tag)->getAllTags());for(auto* child:*children)freeArrays(child);}
}
#endif
int main(){
    for(int seed:{0,1,-1,345671,INT_MIN,INT_MAX}){
        unsigned char bytes[]={0,1,127,128,255,static_cast<unsigned char>(seed)};
        int ints[]={seed,INT_MIN,INT_MAX,0x12345678};
        CompoundTag root(L"Level");
        root.putByte(L"byte",255);root.putShort(L"short",-32768);root.putInt(L"int",seed);
        root.putLong(L"long",static_cast<__int64>(seed)*65537);
        root.putFloat(L"float",static_cast<float>(seed)*0.125f);root.putDouble(L"double",static_cast<double>(seed)*0.25);
        root.putBoolean(L"bool",true);root.putString(L"string",std::wstring(L"Console\0\u00e9\u6c34",10));
        root.putByteArray(L"blocks",byteArray(bytes,6));root.putIntArray(L"ints",intArray(ints,4));
        auto* nested=new CompoundTag(L"");nested->putString(L"name",L"Spawn");nested->putInt(L"x",seed);root.putCompound(L"nested",nested);
        auto* list=new ListTag<Tag>(L"");
        for(int i=0;i<4;++i)list->add(new IntTag(L"",i+100));root.put(L"list",list);
        root.put(L"empty",new ListTag<Tag>(L""));
        ByteArrayOutputStream raw;DataOutputStream out(&raw);NbtIo::write(&root,&out);
        auto payload=raw.toByteArray();ByteArrayInputStream input(payload);DataInputStream in(&input);
        std::unique_ptr<CompoundTag> read(NbtIo::read(&in));
        if(!read || !root.equals(read.get()) || input.read()!=-1)return 1;
        std::uint64_t hash=14695981039346656037ull;
        for(unsigned i=0;i<raw.size();++i)hash=(hash^raw.buf[i])*1099511628211ull;
        std::cout<<seed<<' '<<raw.size()<<' '<<hash<<' '<<read->getInt(L"int")<<' '<<read->getLong(L"long")<<' '<<read->getList(L"list")->size()<<'\n';
#ifdef CONSOLE_NBT_REFERENCE
        freeArrays(read.get());
#endif
    }
}
