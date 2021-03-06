#pragma once
#include "CircularBuffer.h"
#include "OverlappedIOContext.h"
#include "FastSpinlock.h"
#include "JobDispatcher.h"

class Session : public AsyncExecutable
{
public:
	Session(size_t sendBufSize, size_t recvBufSize);
	virtual ~Session() {}

	bool IsConnected() const { return !!mConnected; }

	void DisconnectRequest(DisconnectReason dr) ;

	bool PreRecv(); ///< zero byte recv
	bool PostRecv() ;

	bool PostSend(const char* data, size_t len);
	bool FlushSend() ;

	void DisconnectCompletion(DisconnectReason dr) ;
	void SendCompletion(DWORD transferred) ;
	void RecvCompletion(DWORD transferred) ;

	void AddRef();
	void ReleaseRef();

	virtual void OnRead(size_t len) {}
	virtual void OnDisconnect(DisconnectReason dr) {}
	virtual void OnRelease() {}

	void	SetSocket(SOCKET sock) { mSocket = sock; }
	SOCKET	GetSocket() const { return mSocket; }

	void EchoBack();

protected:

	SOCKET			mSocket;

	CircularBuffer	mRecvBuffer;
	CircularBuffer	mSendBuffer;
	FastSpinlock	mSendBufferLock;
	int				mSendRequestedCount;

	volatile long	mConnected;

};


