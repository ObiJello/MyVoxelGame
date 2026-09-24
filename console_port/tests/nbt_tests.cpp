#include "NbtIo.h"
#include "EndTag.h"
#include <sstream>
#include <iostream>
#include <functional>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class E=IoError,class F> static void rejects(F f){try{f();}catch(const E&){return;}throw std::runtime_error("Expected rejection");}
static std::unique_ptr<CompoundTag> parse(const std::vector<unsigned char>& bytes){return std::unique_ptr<CompoundTag>(NbtIo::decompress(byteArray(const_cast<unsigned char*>(bytes.data()),static_cast<unsigned>(bytes.size()))));}
static std::vector<unsigned char> encode(CompoundTag& root){auto array=NbtIo::compress(&root);std::unique_ptr<unsigned char[]> owner(array.data);return {array.data,array.data+array.length};}
class CountTag:public IntTag {
public:
    static inline int live=0;
    CountTag():IntTag(L"",7){++live;}
    ~CountTag(){--live;}
};
int main(){try{
    CompoundTag empty;auto emptyBytes=encode(empty);
    require(emptyBytes==std::vector<unsigned char>({10,0,0,0}),"NBT compress must return valid bytes without capacity padding");
    CompoundTag root(L"root");unsigned char original[]={1,2,3};int numbers[]={0x01000000,INT_MIN,INT_MAX};
    root.putByteArray(L"bytes",byteArray(original,3));root.putIntArray(L"ints",intArray(numbers,3));
    root.putString(L"unicode",L"PS3 \U0001f30d");
    auto* typedList=new ListTag<CompoundTag>(L"");auto* member=new CompoundTag(L"");member->putInt(L"value",2);typedList->add(member);root.put(L"list",typedList);
    require(root.getList(L"list")->get(0)==member,"List specializations must share checked storage");
    auto wire=encode(root);auto decoded=parse(wire);require(decoded->equals(&root),"All tags must survive round trip");
    auto* decodedArray=static_cast<ByteArrayTag*>(decoded->get(L"bytes"));require(decodedArray->data.data!=original,"Decoded data must own separate memory");
    std::unique_ptr<CompoundTag> copy(static_cast<CompoundTag*>(decoded->copy()));
    decodedArray->data[0]=99;require(copy->getByteArray(L"bytes")[0]==1 && !copy->equals(decoded.get()),"Copy must deep copy arrays");
    auto transfer=decoded->takeByteArray(L"bytes");std::unique_ptr<unsigned char[]> transferred(transfer.data);decoded.reset();require(transferred[0]==99,"Taken array must outlive its tree");
    auto intTransfer=copy->takeIntArray(L"ints");std::unique_ptr<int[]> transferredInts(intTransfer.data);copy.reset();require(transferredInts[2]==INT_MAX,"Taken int array must outlive its tree");
    for(std::size_t n=0;n<wire.size();++n){std::vector<unsigned char> truncated(wire.begin(),wire.begin()+n);rejects([&]{parse(truncated);});}
    require(original[0]==1,"Borrowed builder arrays must not be freed or mutated");
    rejects([&]{parse({255});});rejects([&]{parse({12});});
    rejects([&]{parse({10,0,0,7,0,0,255,255,255,255,0});}); // negative byte-array length
    rejects([&]{parse({10,0,0,11,0,0,127,255,255,255,0});}); // oversized int array
    rejects([&]{parse({10,0,0,9,0,0,0,0,0,0,1,0});}); // nonempty End list
    rejects([&]{parse({10,0,0,9,0,0,12,0,0,0,0,0});}); // unknown list type, even when empty
    rejects([&]{parse({10,0,0,9,0,0,1,255,255,255,255,0});});
    require(!parse({0}),"Non-compound root must retain original null result");
    std::vector<unsigned char> deep;
    for(unsigned i=0;i<NbtReadScope::maxDepth+1;++i)deep.insert(deep.end(),{10,0,0});
    deep.insert(deep.end(),NbtReadScope::maxDepth+1,0);rejects([&]{parse(deep);});
    require(parse(emptyBytes)->isEmpty(),"Read budget must recover after an exception");
    // Repeated load is transactional, including when the old array was borrowed.
    ByteArrayTag array(L"a",byteArray(original,3));unsigned char shortPayload[]={0,0,0,3,9};
    {ByteArrayInputStream input(byteArray(shortPayload,5));struct Detach{ByteArrayInputStream& in;~Detach(){in.reset();}} detach{input};DataInputStream dis(&input);rejects([&]{array.load(&dis);});}
    require(array.data.data==original && original[0]==1,"Failed load must retain prior borrowed array");
    CompoundTag state;state.putInt(L"old",10);
    unsigned char badCompound[]={3,0,1,'x',0};
    {ByteArrayInputStream input(byteArray(badCompound,5));struct Detach{ByteArrayInputStream& in;~Detach(){in.reset();}} detach{input};DataInputStream dis(&input);rejects([&]{state.load(&dis);});}
    require(state.getInt(L"old")==10,"Failed compound load must retain prior tree");
    {CompoundTag counts;counts.put(L"a",new CountTag);counts.put(L"a",new CountTag);require(CountTag::live==1,"Replacement must free previous tag");
        auto* tag=counts.take(L"a");require(!counts.contains(L"a"),"Take must remove child");counts.put(L"b",tag);counts.remove(L"b");require(CountTag::live==0,"Removal must destroy child");}
    {ListTag<Tag> list;list.add(new IntTag(L"",1));std::unique_ptr<Tag> bad(new StringTag(L"",L"x"));rejects([&]{list.add(bad.get());});require(list.size()==1,"Mixed list rejected without taking ownership");
        rejects<std::out_of_range>([&]{list.get(-1);});rejects([&]{list.add(list.get(0));});
        std::unique_ptr<Tag> listCopy(list.copy());static_cast<IntTag*>(list.get(0))->data=2;require(!list.equals(listCopy.get()),"List equality must compare ordered payloads");}
    {CompoundTag a;rejects([&]{a.put(L"self",&a);});auto* child=new CompoundTag(L"");a.putCompound(L"child",child);rejects([&]{child->put(L"cycle",&a);});rejects([&]{a.put(L"alias",child);});}
    int first[]={0,0},second[]={0,0x01000000};IntArrayTag ia(L"",intArray(first,2)),ib(L"",intArray(second,2));require(!ia.equals(&ib),"Int array equality must compare every byte");
    root.putInt(L"integer",5);rejects([&]{root.getString(L"integer");});require(root.getInt(L"missing")==0,"Missing primitive fallback");
    {std::unique_ptr<CompoundTag> missing(root.getCompound(L"missing"));std::unique_ptr<TagList> list(root.getList(L"missing"));require(missing->isEmpty() && list->size()==0,"Missing container fallbacks");}
    // Duplicate names preserve the original last-value-wins semantics.
    auto duplicate=parse({10,0,0,3,0,1,'x',0,0,0,1,3,0,1,'x',0,0,0,2,0});require(duplicate->getInt(L"x")==2,"Duplicate named tags must replace prior value");
    std::wostringstream print;root.print("",print);require(print.str().find(L"TAG_Compound")!=std::wstring::npos,"Debug print must be usable on host streams");
    LongTag minimum(L"",INT64_MIN);require(minimum.toString()==L"-9223372036854775808","Long formatting must use portable width");
    std::cout<<"NBT ownership, format, malformed input, and round-trip checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
