// Primitive stream interface; platform PlayerUID serialization is a separate pending port.
#pragma once
// 4J Stu - Represents Java standard library class (although we miss out an intermediate inheritance class that we don't care about)

#include "InputStream.h"
#include "DataInput.h"

class DataInputStream : public InputStream, public DataInput
{
private:
	InputStream *stream;
    InputStream& child(){if(!stream)throw IoError("Input stream has no child");return *stream;}
    unsigned requiredByte(){int b=child().read();if(b<0)throw EndOfStream();if(b>255)throw IoError("Invalid ::byte from input stream");return static_cast<unsigned>(b);}
    std::uint64_t readBits(int bytes){std::uint64_t value=0;while(bytes--)value=(value<<8)|requiredByte();return value;}

public:
    DataInputStream(const DataInputStream&)=delete;
    DataInputStream& operator=(const DataInputStream&)=delete;
	DataInputStream(InputStream *in);
	virtual int read();
	virtual int read(byteArray b);
	virtual int read(byteArray b, unsigned int offset, unsigned int length);
	virtual void close();
	virtual bool readBoolean();
	virtual ::byte readByte();
	virtual unsigned char readUnsignedByte();
	virtual wchar_t readChar();
	virtual bool readFully(byteArray b);
	virtual bool readFully(charArray b);
	virtual double readDouble();
	virtual float readFloat();
	virtual int readInt();
	virtual __int64 readLong();
	virtual short readShort();
	virtual wstring readUTF();
	void deleteChildStream();
	virtual int readUTFChar();
	virtual __int64 skip(__int64 n);
	virtual int skipBytes(int n);
};
