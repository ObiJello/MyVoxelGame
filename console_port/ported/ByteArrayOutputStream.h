#pragma once

#include "OutputStream.h"

class ByteArrayOutputStream : public OutputStream
{
// Note - when actually implementing, byteArray will need to grow as data is written
public:
    ByteArrayOutputStream(const ByteArrayOutputStream&)=delete;
    ByteArrayOutputStream& operator=(const ByteArrayOutputStream&)=delete;

	byteArray buf;  //The buffer where data is stored.

protected:
	unsigned int count; //The number of valid bytes in the buffer.
    void ensureCapacity(unsigned int extra);

public:
	ByteArrayOutputStream();
	ByteArrayOutputStream(unsigned int size);
	virtual ~ByteArrayOutputStream();

	virtual void flush() {}
	virtual void write(unsigned int b);
	virtual void write(byteArray b);
	virtual void write(byteArray b, unsigned int offset, unsigned int length);
	virtual void close();
	virtual byteArray toByteArray();

	void reset() { count = 0; }
	unsigned int size() { return count; }
	
};
