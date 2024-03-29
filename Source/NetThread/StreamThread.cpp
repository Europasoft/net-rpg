#include "StreamThread.h"
#include <cassert>
#include <limits.h>

StreamThread::StreamThread(const size_t& sendBufferSize, const size_t& receiveBufferSize,
                        const size_t sndMax, const size_t& recMax)
                        : sndBuffer{ sendBufferSize, sndMax }, recBuffer{ receiveBufferSize, recMax } {};
StreamThread::~StreamThread() { terminateThread(); }

void StreamThread::start(const std::string& hostName, const std::string& port_)
{
    // stream thread will handle connecting, since hostname was specified (client mode)
    if (streamConnected) { return; }
    hostname = hostName;
    port = port_;
    streamConnected = false;
    thread = std::thread([this] { this->threadMain(); }); // create thread
}
void StreamThread::start(const SOCKET& s)
{
    // this overload assumes socket is already connected (server mode)
    if (streamConnected) { return; }
    assert(s != INVALID_SOCKET && "connected socket (or hostname) required to start stream thread");
    socket.set(s);
    streamConnected = true;
    thread = std::thread([this] { this->threadMain(); }); // create thread
}

void StreamThread::threadMain()
{
    bool terminate = false;

    // resolve hostname and connect (only in client mode)
    if (!streamConnected)
    {
        Timer t;
        t.start();
        while (!t.checkTimeout(10.0))
        {
            SOCKET s;
            if (Sockets::setupStream(hostname, port, s))
            {
                socket.set(s); // set function is threadsafe
                streamConnected = true;
                break;
            }
        }
        // connection timeout
        if (!streamConnected) 
        { 
            terminate = true;
            connectionFailure = true;
        }
    }

    // thread main loop
    while (!terminate)
    {
        // send TCP stream data
        if (sndBuffer.getDataSize() > 0)
        {
            Lock s_lock, b_lock;
            SOCKET s = socket.get(s_lock); // lock socket mutex, released at end of block
            if (Sockets::sendData(s, sndBuffer.getBuffer(b_lock), sndBuffer.getDataSize()))
            { sndBuffer.setDataSize(0); } // indicate data sent (mark empty)
        }

        // receive TCP stream data
        if (recBuffer.getDataSize() == 0)
        {
            Lock s_lock, b_lock;
            SOCKET s = socket.get(s_lock); // lock socket mutex, released at end of block
            // avoid blocking if there is nothing to receive
            auto recSize = Sockets::getReceiveSize(s);
            if (recSize > 0)
            {
                Sockets::RecStat r = Sockets::receiveData(s, recBuffer.getBuffer(b_lock), recSize);
                recBuffer.setDataSize(recSize);
                if (r.e == Sockets::RecStatE::ConnectionClosed) { terminate = true; } // remote peer closed the connection
            }
        }
        
        terminate = forceTerminate || terminate;
    }
    streamConnected = false;
    // thread terminates at this point
}

bool StreamThread::queueSend(const char* data, const size_t& size, bool overwrite)
{
    assert(isStreamConnected() && "cannot send data, not connected");
    assert(size <= sndBuffer.bufMax && "send data will not fit in buffer");
    if (size == 0 || (sndBuffer.getDataSize() > 0 && !overwrite) || forceTerminate) { return false; }
    Lock bl;
    auto* bptr = sndBuffer.getBuffer(bl); // lock mutex to safely modify the buffer
    if (size > sndBuffer.getBufferSize()) { sndBuffer.realloc(size); }
    memcpy(bptr, data, size);
    sndBuffer.setDataSize(size); // indicate size of data to be sent
    return true;
}

size_t StreamThread::getReceiveBuffer(char* dstBuffer, const size_t& dstBufferSize)
{
    auto size = recBuffer.getDataSize();
    if (size <= 0) { return 0; }
    assert(dstBufferSize >= size && "destination buffer too small, data will be lost");
    Lock bl;
    memcpy(dstBuffer, recBuffer.getBuffer(bl), size);
    recBuffer.setDataSize(0); // mark as empty ("was read")
    return size;
}
