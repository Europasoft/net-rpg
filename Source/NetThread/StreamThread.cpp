// Copyright 2025 Simon Liimatainen, Europa Software. All rights reserved.
#include "StreamThread.h"
#include <cassert>
#include <limits.h>
#include <cstring>

StreamThread::StreamThread(size_t sendBufferSize, size_t receiveBufferSize)
    : sendBuffer{ sendBufferSize }, recvBuffer{ receiveBufferSize } {}

StreamThread::~StreamThread() 
{ 
    stop();
    thread.join();
}

void StreamThread::start(std::string_view hostname_, std::string_view port_, size_t connectTimeout_)
{
    hostname = hostname_;
    port = port_;
    connectTimeout = connectTimeout_;
    start(INVALID_SOCKET);
}

void StreamThread::start(SOCKET socket_)
{
    if (streamConnected) { return; }
    
    if (socket_ != INVALID_SOCKET)
    {
        // assumes socket is already connected (server mode)
        socket.set(socket_);
        streamConnected = true;
    }
    // start thread
    thread = std::thread([this] { this->threadMain(); }); 
}

void StreamThread::threadMain()
{
    bool terminate = false;

    // resolve hostname and connect (client mode only)
    if (!streamConnected)
    {
        Timer t;
        t.start();
        SOCKET s;
        while (!t.checkTimeout(connectTimeout))
        {
            if (Sockets::setupStream(hostname, port, s))
            {
                socket.set(s);
                streamConnected = true;
                break;
            }
        }
        if (!streamConnected) 
        { 
            // timeout
            terminate = true;
            connectionFailure = true;
        }
    }

	lastComTimer.start();

    // thread main loop
    while (!terminate)
    {
        // send
        if (sendBuffer.getDataSize() > 0)
        {
            Lock socketLock, bufferLock;
            SOCKET s = socket.get(socketLock); // lock socket mutex, released at end of block
            if (Sockets::sendData(s, sendBuffer.getBuffer(bufferLock), sendBuffer.getDataSize()))
            { 
                sendBuffer.setDataSize(0); // data sent, mark empty
            }
			lastComTimer.start();
        }

        // receive
        if (recvBuffer.getDataSize() == 0)
        {
            Lock socketLock;
            SOCKET s = socket.get(socketLock); // lock socket mutex, released at end of scope
            auto recvSize = Sockets::getReceiveSize(s);
            if (recvSize > 0)
            {
				// end the connection if the network buffer grows too large
				if (recvSize > 5e7)
				{
					terminate = true;
				}
				else
				{
					Lock bufferLock;
					auto* buffer = recvBuffer.getBuffer(bufferLock);
					if (recvBuffer.reserve(recvSize, bufferLock))
					{
						recvSize = Sockets::receiveData(s, buffer, recvSize);
						recvBuffer.setDataSize(recvSize);
						lastComTimer.start();
					}
					else
					{
						terminate = true;
					}
				}

                if (recvSize == 0) { terminate = true; } // connection closed
            }
        }
		
		if (lastComTimer.getElapsed() > 3.f)
			Sockets::threadSleep(50); // idle the thread if no communication has happened in a while
        
        terminate = forceTerminate || terminate;
    }
    streamConnected = false;
}

bool StreamThread::queueSend(std::string_view data)
{
    assert(streamConnected && data.size() <= sendBuffer.getBufMax() && "cannot send data");
    if (!streamConnected || !data.size() || sendBuffer.getDataSize() > 0 || forceTerminate) { return false; }

    Lock lock;
    sendBuffer.getBuffer(lock);
    return sendBuffer.copyFrom(data.data(), data.size(), lock);
}

void StreamThread::getReceiveBuffer(std::string& data) 
{
    if (recvBuffer.getBufferSize() <= 0) { return; }
    Lock lock;
    data = std::string(recvBuffer.getBuffer(lock), recvBuffer.getDataSize());
    recvBuffer.setDataSize(0);
}

size_t StreamThread::getReceiveBuffer(char* dstBuffer, const size_t& dstBufferSize)
{
    auto size = recvBuffer.getDataSize();
    if (size <= 0) { return 0; }
    assert(dstBufferSize >= size && "destination buffer too small, data will be lost");
    Lock bl;
    memcpy(dstBuffer, recvBuffer.getBuffer(bl), size);
    recvBuffer.setDataSize(0); // mark as empty ("was read")
    return size;
}
