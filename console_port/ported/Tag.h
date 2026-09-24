#pragma once
#include <ostream>
#include "NbtReadBudget.h"
#include "InputOutputStream.h"
using namespace std;

 
class Tag
{
public:
    static const ::byte TAG_End = 0;
    static const ::byte TAG_Byte = 1;
    static const ::byte TAG_Short = 2;
    static const ::byte TAG_Int = 3;
    static const ::byte TAG_Long = 4;
    static const ::byte TAG_Float = 5;
    static const ::byte TAG_Double = 6;
    static const ::byte TAG_Byte_Array = 7;
    static const ::byte TAG_String = 8;
    static const ::byte TAG_List = 9;
    static const ::byte TAG_Compound = 10;
	static const ::byte TAG_Int_Array = 11;

private:
    wstring name;
    Tag* parent_=nullptr;
    friend class TagList;
    friend class CompoundTag;
    void requireAdoptable(Tag* parent){
        if(parent_)throw IoError("NBT tag already belongs to a container");
        for(auto p=parent;p;p=p->parent_)if(p==this)throw IoError("Cyclic NBT container");
    }

protected:
	Tag(const wstring &name);

public:
    virtual void write(DataOutput *dos) = 0;
    virtual void load(DataInput *dis)  = 0;
    virtual wstring toString() = 0;
    virtual ::byte getId() = 0;
    void print(wostream& out);
    virtual void print(const char *prefix, wostream& out);
    wstring getName();
    Tag *setName(const wstring& name);
    static Tag *readNamedTag(DataInput *dis);
    static void writeNamedTag(Tag *tag, DataOutput *dos);
    static Tag *newTag(::byte type, const wstring &name);
    static const wchar_t *getTagName(::byte type);
	Tag(const Tag&)=delete;
    Tag& operator=(const Tag&)=delete;
    virtual ~Tag() {}
	virtual bool equals(Tag *obj); // 4J Brought forward from 1.2
	virtual Tag *copy() = 0; // 4J Brought foward from 1.2
};
