// Ported from Minecraft.World/ByteArrayTag.h. Payload order is unchanged.
#pragma once
#include "Tag.h"
class ByteArrayTag:public Tag {
    std::unique_ptr<unsigned char[]> owned_;
public:
    // A borrowed view. load/copy own their buffers; constructors borrow the input.
    byteArray data;
    ByteArrayTag(const wstring& name):Tag(name){}
    ByteArrayTag(const wstring& name,byteArray value):Tag(name),data(value){}
    void write(DataOutput* dos) override {
        if(data.length>INT_MAX || (data.length && !data.data))throw IoError("Invalid NBT array");
        dos->writeInt(static_cast<int>(data.length));
        dos->write(data);
    }
    void load(DataInput* dis) override {
        NbtReadScope scope;
        unsigned length=NbtReadScope::arrayLength(dis->readInt(),sizeof(unsigned char));
        auto buffer=std::make_unique<unsigned char[]>(length);
        byteArray next(buffer.get(),length);
        if(!dis->readFully(next))throw EndOfStream();
        owned_=std::move(buffer);data=next;
    }
    // Transfers decoded/copied storage; a borrowed builder array remains borrowed.
    byteArray takeData() {auto result=data;owned_.release();data=byteArray();return result;}
    ::byte getId() override {return TAG_Byte_Array;}
    wstring toString() override {return L"["+std::to_wstring(data.length)+L" elements]";}
    bool equals(Tag* other) override {
        if(!Tag::equals(other))return false;
        auto* o=static_cast<ByteArrayTag*>(other);
        return data.length==o->data.length && (!data.length || (data.data && o->data.data &&
            std::memcmp(data.data,o->data.data,static_cast<std::size_t>(data.length)*sizeof(unsigned char))==0));
    }
    Tag* copy() override {
        if(data.length && !data.data)throw IoError("Invalid NBT array");
        auto result=std::make_unique<ByteArrayTag>(getName());
        result->owned_=std::make_unique<unsigned char[]>(data.length);
        if(data.length)std::copy_n(data.data,data.length,result->owned_.get());
        result->data=byteArray(result->owned_.get(),data.length);
        return result.release();
    }
};
