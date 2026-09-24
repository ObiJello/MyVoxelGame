#include "stdafx.h"
#include "BasicTypeContainers.h"

#include "DataOutputStream.h"
#include "ModifiedUtf.h"

//Creates a new data output stream to write data to the specified underlying output stream. The counter written is set to zero.
//Parameters:
//out - the underlying output stream, to be saved for later use.
DataOutputStream::DataOutputStream( OutputStream *out ) : stream( out ), written( 0 )
{
    if(!out)throw std::invalid_argument("Output stream is null");
}

// 4J Stu - We cannot always delete the stream when we are destroyed, but we want to clear it up as there
// are occasions when we don't have a handle to the child stream elsewhere and want to delete it
void DataOutputStream::deleteChildStream()
{
    delete stream;stream=nullptr;
}

//Writes the specified ::byte (the low eight bits of the argument b) to the underlying output stream.
//If no exception is thrown, the counter written is incremented by 1.
//Implements the write method of OutputStream.
//Parameters:
//b - the ::byte to be written.
void DataOutputStream::write(unsigned int b)
{
    child().write(b);addWritten(1);
}

void DataOutputStream::flush()
{
	child().flush();
}

//Writes b.length bytes from the specified ::byte array to this output stream.
//The general contract for write(b) is that it should have exactly the same effect as the call write(b, 0, b.length).
//Parameters:
//b - the data.
void DataOutputStream::write(byteArray b)
{
	write(b, 0, b.length);
}

//Writes len bytes from the specified ::byte array starting at offset off to the underlying output stream.
//If no exception is thrown, the counter written is incremented by len.
//Parameters:
//b - the data.
//off - the start offset in the data.
//len - the number of bytes to write.
void DataOutputStream::write(byteArray b, unsigned int offset, unsigned int length)
{
    validateByteRange(b,offset,length);child().write(b,offset,length);addWritten(length);
}

//Closes this output stream and releases any system resources associated with the stream.
//The close method of FilterOutputStream calls its flush method, and then calls the close method of its underlying output stream.
void DataOutputStream::close()
{
	child().close();
}

//Writes out a ::byte to the underlying output stream as a 1-::byte value. If no exception is thrown, the counter written is incremented by 1.
//Parameters:
//v - a ::byte value to be written.
void DataOutputStream::writeByte(::byte a)
{
    write(static_cast<unsigned>(a));
}

//Converts the double argument to a long using the doubleToLongBits method in class Double,
//and then writes that long value to the underlying output stream as an 8-::byte quantity,
//high ::byte first. If no exception is thrown, the counter written is incremented by 8.
//Parameters:
//v - a double value to be written.
void DataOutputStream::writeDouble(double a)
{
    writeLong(Double::doubleToLongBits(a));
}

//Converts the float argument to an int using the floatToIntBits method in class Float,
//and then writes that int value to the underlying output stream as a 4-::byte quantity, high ::byte first.
//If no exception is thrown, the counter written is incremented by 4.
//Parameters:
//v - a float value to be written.
void DataOutputStream::writeFloat(float a)
{
    writeInt(Float::floatToIntBits(a));
}

//Writes an int to the underlying output stream as four bytes, high ::byte first. If no exception is thrown, the counter written is incremented by 4.
//Parameters:
//v - an int to be written.
void DataOutputStream::writeInt(int a)
{
    auto bits=static_cast<std::uint32_t>(a);for(int shift=24;shift>=0;shift-=8)write((bits>>shift)&255);
}

//Writes a long to the underlying output stream as eight bytes, high ::byte first.
//In no exception is thrown, the counter written is incremented by 8.
//Parameters:
//v - a long to be written.
void DataOutputStream::writeLong(__int64 a)
{
    auto bits=static_cast<std::uint64_t>(a);for(int shift=56;shift>=0;shift-=8)write(static_cast<unsigned>((bits>>shift)&255));
}

//Writes a short to the underlying output stream as two bytes, high ::byte first.
//If no exception is thrown, the counter written is incremented by 2.
//Parameters:
//v - a short to be written.
void DataOutputStream::writeShort(short a)
{
    auto bits=static_cast<std::uint16_t>(a);write(bits>>8);write(bits&255);
}

//Writes a char to the underlying output stream as a 2-::byte value, high ::byte first.
//If no exception is thrown, the counter written is incremented by 2.
//Parameters:
//v - a char value to be written.
void DataOutputStream::writeChar( wchar_t v )
{
    auto unit=static_cast<std::uint32_t>(v);if(unit>65535)throw InvalidUtf("writeChar requires one UTF-16 code unit");
    write(unit>>8);write(unit&255);
}

//Writes a string to the underlying output stream as a sequence of characters.
//Each character is written to the data output stream as if by the writeChar method.
//If no exception is thrown, the counter written is incremented by twice the length of s.
//Parameters:
//s - a String value to be written.
void DataOutputStream::writeChars(const wstring& str)
{
    for(auto unit:console::io::utf16Units(str))writeChar(static_cast<wchar_t>(unit));
}

//Writes a boolean to the underlying output stream as a 1-::byte value.
//The value true is written out as the value (::byte)1; the value false is written out as the value (::byte)0.
//If no exception is thrown, the counter written is incremented by 1.
//Parameters:
//v - a boolean value to be written.
void DataOutputStream::writeBoolean(bool b)
{
    write(b?1u:0u);
}

//Writes a string to the underlying output stream using modified UTF-8 encoding in a machine-independent manner.
//First, two bytes are written to the output stream as if by the writeShort method giving the number of bytes to follow.
//This value is the number of bytes actually written out, not the length of the string. Following the length,
//each character of the string is output, in sequence, using the modified UTF-8 encoding for the character.
//If no exception is thrown, the counter written is incremented by the total number of bytes written to the output stream.
//This will be at least two plus the length of str, and at most two plus thrice the length of str.
//Parameters:
//str - a string to be written.
void DataOutputStream::writeUTF(const wstring& str)
{
    if(str.size()>65535)throw InvalidUtf("Modified UTF string is too long");
    auto units=console::io::utf16Units(str);
    unsigned length=0;
    for(auto c:units){length+=(c>=1 && c<=127)?1:c>2047?3:2;if(length>65535)throw InvalidUtf("Modified UTF string is too long");}
    std::vector<unsigned char> bytes;bytes.reserve(length+2);bytes.push_back(length>>8);bytes.push_back(length&255);
    for(auto c:units){
        if(c>=1 && c<=127)bytes.push_back(c);
        else if(c>2047){bytes.push_back(0xe0|((c>>12)&15));bytes.push_back(0x80|((c>>6)&63));bytes.push_back(0x80|(c&63));}
        else{bytes.push_back(0xc0|((c>>6)&31));bytes.push_back(0x80|(c&63));}
    }
    write(byteArray(bytes.data(),static_cast<unsigned>(bytes.size())));
}

// 4J Added
