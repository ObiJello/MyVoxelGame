// Ported from Minecraft.World/ListTag.h. The element type, count, and unnamed
// payload sequence retain the original format; all specializations share storage.
#pragma once
#include "Tag.h"
class TagList:public Tag {
    std::vector<std::unique_ptr<Tag>> list;
    ::byte type=TAG_Byte;
public:
    TagList():Tag(L""){}
    explicit TagList(const wstring& name):Tag(name){}
    void write(DataOutput* dos) override {
        if(list.size()>INT_MAX)throw IoError("NBT list too large");
        type=list.empty()?TAG_Byte:list[0]->getId();
        dos->writeByte(type);dos->writeInt(static_cast<int>(list.size()));
        for(auto& tag:list)tag->write(dos);
    }
    void load(DataInput* dis) override {
        NbtReadScope scope;
        ::byte nextType=dis->readByte();
        unsigned count=NbtReadScope::arrayLength(dis->readInt(),sizeof(std::unique_ptr<Tag>));
        if(nextType>TAG_Int_Array || (count && nextType==TAG_End))throw IoError("Invalid NBT list element type");
        NbtReadScope::charge(static_cast<std::size_t>(count)*128,count);
        std::vector<std::unique_ptr<Tag>> next;
        next.reserve(count);
        for(unsigned i=0;i<count;++i){
            std::unique_ptr<Tag> tag(Tag::newTag(nextType,L""));
            tag->load(dis);tag->parent_=this;next.push_back(std::move(tag));
        }
        list.swap(next);type=nextType;
    }
    ::byte getId() override {return TAG_List;}
    wstring toString() override {return std::to_wstring(list.size())+L" entries of type "+Tag::getTagName(type);}
    void print(const char* prefix,std::wostream& out) override {
        Tag::print(prefix,out);out<<prefix<<L"{\n";
        std::string next=std::string(prefix)+"   ";
        for(auto& tag:list)tag->print(next.c_str(),out);
        out<<prefix<<L"}\n";
    }
    // Ownership passes on success. A rejected tag remains with its caller.
    void add(Tag* tag){
        if(!tag || tag->getId()==TAG_End || (!list.empty() && tag->getId()!=type))throw IoError("NBT lists must contain one non-end tag type");
        tag->requireAdoptable(this);
        if(list.size()>=INT_MAX)throw IoError("NBT list too large");
        if(list.size()==list.capacity())list.reserve(std::max<std::size_t>(8,list.capacity()*2)); // Allocate before ownership passes.
        list.emplace_back(tag);tag->parent_=this;type=tag->getId();
    }
    Tag* get(int index){return list.at(static_cast<std::size_t>(index)).get();}
    int size(){return static_cast<int>(list.size());}
    Tag* copy() override {
        auto result=std::make_unique<TagList>(getName());result->type=type;
        for(auto& tag:list){std::unique_ptr<Tag> child(tag->copy());result->add(child.get());child.release();}
        return result.release();
    }
    bool equals(Tag* obj) override {
        if(!Tag::equals(obj))return false;
        auto* other=dynamic_cast<TagList*>(obj);
        if(!other || list.size()!=other->list.size())return false;
        // Empty lists serialize as TAG_Byte in the original writer.
        if(!list.empty() && type!=other->type)return false;
        for(std::size_t i=0;i<list.size();++i)if(!list[i]->equals(other->list[i].get()))return false;
        return true;
    }
};
template<class T> class ListTag:public TagList {
public:
    using TagList::TagList;
    void add(T* tag){TagList::add(tag);}
    T* get(int index){auto* tag=dynamic_cast<T*>(TagList::get(index));if(!tag)throw IoError("Unexpected NBT list element type");return tag;}
};
