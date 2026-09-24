// Ported from Minecraft.World/CompoundTag.h. Named children followed by TAG_End.
#pragma once
#include "Tag.h"
#include "ListTag.h"
#include "ByteTag.h"
#include "DoubleTag.h"
#include "FloatTag.h"
#include "IntTag.h"
#include "LongTag.h"
#include "ShortTag.h"
#include "StringTag.h"
#include "ByteArrayTag.h"
#include "IntArrayTag.h"
class CompoundTag:public Tag {
    std::unordered_map<wstring,std::unique_ptr<Tag>> tags;
    template<class T> T* typed(const wchar_t* name){
        auto* tag=get(name); if(!tag)return nullptr;
        auto* value=dynamic_cast<T*>(tag);if(!value)throw IoError("Unexpected NBT compound value type");return value;
    }
    void putOwned(const wchar_t* name,std::unique_ptr<Tag> tag){put(name,tag.get());tag.release();}
public:
    CompoundTag():Tag(L""){}
    explicit CompoundTag(const wstring& name):Tag(name){}
    void write(DataOutput* dos) override {
        for(auto& [name,tag]:tags)Tag::writeNamedTag(tag.get(),dos);
        dos->writeByte(TAG_End);
    }
    void load(DataInput* dis) override {
        NbtReadScope scope;
        CompoundTag next(getName());
        for(;;){
            std::unique_ptr<Tag> tag(Tag::readNamedTag(dis));
            if(tag->getId()==TAG_End)break;
            const auto name=tag->getName();next.putOwned(name.c_str(),std::move(tag));
        }
        tags.swap(next.tags);
        for(auto& [name,tag]:tags)tag->parent_=this;
    }
    std::vector<Tag*>* getAllTags(){
        auto result=std::make_unique<std::vector<Tag*>>();
        result->reserve(tags.size());for(auto& [name,tag]:tags)result->push_back(tag.get());return result.release();
    }
    ::byte getId() override {return TAG_Compound;}
    // Ownership passes on success; replacing a value destroys the old child.
    void put(const wchar_t* name,Tag* tag){
        if(!name || !tag || tag->getId()==TAG_End)throw IoError("Invalid NBT compound child");
        auto it=tags.find(name);if(it!=tags.end() && it->second.get()==tag)return;
        tag->requireAdoptable(this);tag->setName(name);
        auto [slot,inserted]=tags.try_emplace(name,nullptr);
        slot->second.reset(tag);tag->parent_=this;
    }
    void putByte(const wchar_t* name,::byte value){putOwned(name,std::make_unique<ByteTag>(name,value));}
    void putShort(const wchar_t* name,short value){putOwned(name,std::make_unique<ShortTag>(name,value));}
    void putInt(const wchar_t* name,int value){putOwned(name,std::make_unique<IntTag>(name,value));}
    void putLong(const wchar_t* name,__int64 value){putOwned(name,std::make_unique<LongTag>(name,value));}
    void putFloat(const wchar_t* name,float value){putOwned(name,std::make_unique<FloatTag>(name,value));}
    void putDouble(const wchar_t* name,double value){putOwned(name,std::make_unique<DoubleTag>(name,value));}
    void putString(const wchar_t* name,const wstring& value){putOwned(name,std::make_unique<StringTag>(name,value));}
    void putByteArray(const wchar_t* name,byteArray value){putOwned(name,std::make_unique<ByteArrayTag>(name,value));}
    void putIntArray(const wchar_t* name,intArray value){putOwned(name,std::make_unique<IntArrayTag>(name,value));}
    void putCompound(const wchar_t* name,CompoundTag* value){put(name,value);}
    void putBoolean(const wchar_t* name,bool value){putByte(name,value?1:0);}
    Tag* get(const wchar_t* name){if(!name)throw IoError("Null NBT name");auto it=tags.find(name);return it==tags.end()?nullptr:it->second.get();}
    bool contains(const wchar_t* name){return get(name)!=nullptr;}
    ::byte getByte(const wchar_t* name){auto* tag=typed<ByteTag>(name);return tag?tag->data:0;}
    short getShort(const wchar_t* name){auto* tag=typed<ShortTag>(name);return tag?tag->data:0;}
    int getInt(const wchar_t* name){auto* tag=typed<IntTag>(name);return tag?tag->data:0;}
    __int64 getLong(const wchar_t* name){auto* tag=typed<LongTag>(name);return tag?tag->data:0;}
    float getFloat(const wchar_t* name){auto* tag=typed<FloatTag>(name);return tag?tag->data:0;}
    double getDouble(const wchar_t* name){auto* tag=typed<DoubleTag>(name);return tag?tag->data:0;}
    wstring getString(const wchar_t* name){auto* tag=typed<StringTag>(name);return tag?tag->data:L"";}
    // Views remain valid while the tag owns the buffer. Use take* to transfer it.
    byteArray getByteArray(const wchar_t* name){auto* tag=typed<ByteArrayTag>(name);return tag?tag->data:byteArray();}
    intArray getIntArray(const wchar_t* name){auto* tag=typed<IntArrayTag>(name);return tag?tag->data:intArray();}
    byteArray takeByteArray(const wchar_t* name){auto* tag=typed<ByteArrayTag>(name);return tag?tag->takeData():byteArray();}
    intArray takeIntArray(const wchar_t* name){auto* tag=typed<IntArrayTag>(name);return tag?tag->takeData():intArray();}
    // Preserve the original missing-container convention: the caller owns the fallback.
    CompoundTag* getCompound(const wchar_t* name){auto* tag=typed<CompoundTag>(name);return tag?tag:new CompoundTag(name);}
    TagList* getList(const wchar_t* name){auto* tag=typed<TagList>(name);return tag?tag:new ListTag<Tag>(name);}
    bool getBoolean(const wchar_t* name){return getByte(name)!=0;}
    Tag* take(const wstring& name){auto it=tags.find(name);if(it==tags.end())return nullptr;auto* tag=it->second.release();tags.erase(it);tag->parent_=nullptr;return tag;}
    void remove(const wstring& name){tags.erase(name);}
    wstring toString() override {return std::to_wstring(tags.size())+L" entries";}
    void print(const char* prefix,std::wostream& out) override {
        Tag::print(prefix,out);out<<prefix<<L"{\n";std::string next=std::string(prefix)+"   ";
        for(auto& [name,tag]:tags)tag->print(next.c_str(),out);out<<prefix<<L"}\n";
    }
    bool isEmpty(){return tags.empty();}
    Tag* copy() override {
        auto result=std::make_unique<CompoundTag>(getName());
        for(auto& [name,tag]:tags)result->putOwned(name.c_str(),std::unique_ptr<Tag>(tag->copy()));return result.release();
    }
    bool equals(Tag* obj) override {
        if(!Tag::equals(obj))return false;
        auto* other=static_cast<CompoundTag*>(obj);if(tags.size()!=other->tags.size())return false;
        for(auto& [name,tag]:tags){auto it=other->tags.find(name);if(it==other->tags.end() || !tag->equals(it->second.get()))return false;}return true;
    }
};
