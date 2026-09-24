// Ported from Minecraft.World/IntArrayTag.h. Payload order is unchanged.
#pragma once
#include "Tag.h"
class IntArrayTag:public Tag {
    std::unique_ptr<int[]> owned_;
public:
    // A borrowed view. load/copy own their buffers; constructors borrow the input.
    intArray data;
    IntArrayTag(const wstring& name):Tag(name){}
    IntArrayTag(const wstring& name,intArray value):Tag(name),data(value){}
    void write(DataOutput* dos) override {
        if(data.length>INT_MAX || (data.length && !data.data))throw IoError("Invalid NBT array");
        dos->writeInt(static_cast<int>(data.length));
        for(unsigned i=0;i<data.length;++i)dos->writeInt(data[i]);
    }
    void load(DataInput* dis) override {
        NbtReadScope scope;
        unsigned length=NbtReadScope::arrayLength(dis->readInt(),sizeof(int));
        auto buffer=std::make_unique<int[]>(length);
        intArray next(buffer.get(),length);
        for(unsigned i=0;i<length;++i)next[i]=dis->readInt();
        owned_=std::move(buffer);data=next;
    }
    // Transfers decoded/copied storage; a borrowed builder array remains borrowed.
    intArray takeData() {auto result=data;owned_.release();data=intArray();return result;}
    ::byte getId() override {return TAG_Int_Array;}
    wstring toString() override {return L"["+std::to_wstring(data.length)+L" elements]";}
    bool equals(Tag* other) override {
        if(!Tag::equals(other))return false;
        auto* o=static_cast<IntArrayTag*>(other);
        return data.length==o->data.length && (!data.length || (data.data && o->data.data &&
            std::memcmp(data.data,o->data.data,static_cast<std::size_t>(data.length)*sizeof(int))==0));
    }
    Tag* copy() override {
        if(data.length && !data.data)throw IoError("Invalid NBT array");
        auto result=std::make_unique<IntArrayTag>(getName());
        result->owned_=std::make_unique<int[]>(data.length);
        if(data.length)std::copy_n(data.data,data.length,result->owned_.get());
        result->data=intArray(result->owned_.get(),data.length);
        return result.release();
    }
};
