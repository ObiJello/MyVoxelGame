// Primitive stream interface; platform PlayerUID serialization is a separate pending port.
#pragma once

class DataOutput
{
public:
    virtual ~DataOutput()=default;
	virtual void write(unsigned int b) = 0;
	virtual void write(byteArray b) = 0;
	virtual void write(byteArray b, unsigned int offset, unsigned int length) = 0;
	virtual void writeByte(::byte a) = 0;
	virtual void writeDouble(double a) = 0;
	virtual void writeFloat(float a) = 0;
	virtual void writeInt(int a) = 0;
	virtual void writeLong(__int64 a) = 0;
	virtual void writeShort(short a) = 0;
	virtual void writeBoolean(bool v) = 0;
	virtual void writeChar(wchar_t v) = 0;
	virtual void writeChars(const wstring& s) = 0;
	virtual void writeUTF(const wstring& a) = 0;
};
