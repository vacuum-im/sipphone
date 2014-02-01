#ifndef ISIPPHONE_H
#define ISIPPHONE_H

#include <QSize>
#include <QUuid>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QMultiMap>

#define SIPPHONE_UUID "{39AD82FD-64D1-486E-AD4F-CD8FA978869B}"

struct ISipAccountConfig {
	QString userid;
	QString password;
	QString serverHost;
	quint16 serverPort;
	QString proxyHost;
	quint16 proxyPort;
};

struct ISipMedia
{
	enum Type {
		Null,
		Audio,
		Video,
		Unknown
	};
	enum Direction {
		None                 = 0x00,
		Capture              = 0x01,
		Playback             = 0x02,
		CaptureAndPlayback   = Capture|Playback
	};
};

struct ISipMediaFormat
{
	#define FORMAT_PACK(C1, C2, C3, C4) ( C4<<24 | C3<<16 | C2<<8 | C1 )
	enum FormatId {
		L16               = 0,
		PCM               = L16,
		PCMA              = FORMAT_PACK('A', 'L', 'A', 'W'),
		ALAW              = PCMA,
		PCMU              = FORMAT_PACK('u', 'L', 'A', 'W'),
		ULAW              = PCMU,
		AMR               = FORMAT_PACK(' ', 'A', 'M', 'R'),
		G729              = FORMAT_PACK('G', '7', '2', '9'),
		ILBC              = FORMAT_PACK('I', 'L', 'B', 'C'),
		RGB24             = FORMAT_PACK('R', 'G', 'B', '3'),
		RGBA              = FORMAT_PACK('R', 'G', 'B', 'A'),
		BGRA              = FORMAT_PACK('B', 'G', 'R', 'A'),
		RGB32             = RGBA,
		DIB               = FORMAT_PACK('D', 'I', 'B', ' '),
		GBRP              = FORMAT_PACK('G', 'B', 'R', 'P'),
		AYUV              = FORMAT_PACK('A', 'Y', 'U', 'V'),
		YUY2              = FORMAT_PACK('Y', 'U', 'Y', '2'),
		UYVY              = FORMAT_PACK('U', 'Y', 'V', 'Y'),
		YVYU              = FORMAT_PACK('Y', 'V', 'Y', 'U'),
		I420              = FORMAT_PACK('I', '4', '2', '0'),
		IYUV              = I420,
		YV12              = FORMAT_PACK('Y', 'V', '1', '2'),
		I422              = FORMAT_PACK('I', '4', '2', '2'),
		I420JPEG          = FORMAT_PACK('J', '4', '2', '0'),
		I422JPEG          = FORMAT_PACK('J', '4', '2', '2'),
		H261              = FORMAT_PACK('H', '2', '6', '1'),
		H263              = FORMAT_PACK('H', '2', '6', '3'),
		H263P             = FORMAT_PACK('P', '2', '6', '3'),
		H264              = FORMAT_PACK('H', '2', '6', '4'),
		MJPEG             = FORMAT_PACK('M', 'J', 'P', 'G'),
		MPEG1VIDEO        = FORMAT_PACK('M', 'P', '1', 'V'),
		MPEG2VIDEO        = FORMAT_PACK('M', 'P', '2', 'V'),
		MPEG4             = FORMAT_PACK('M', 'P', 'G', '4')
	};
	union Details 
	{
		struct Audio {
			unsigned clockRate;
			unsigned channelCount;
			unsigned frameTimeUsec;
			unsigned bitsPerSample;
			quint32 avgBitrate;
			quint32 maxBitrate;
		} aud;
		struct Video {
			int fpsNum;
			int fpsDenum;
			unsigned width;
			unsigned height;
			quint32 avgBitrate;
			quint32 maxBitrate;
		} vid;
	};
	
	ISipMediaFormat() {
		type = ISipMedia::Null;
	}
	ISipMedia::Type type;
	quint32 id;
	Details details;
};

struct ISipMediaStream
{
	enum State {
		None,
		Active,
		LocalHold,
		RemoteHold,
		Error
	};
	enum Property {
		Enabled,          // bool
		Volume,           // float 0.0-mute 1.0-default
	};
	ISipMediaStream() {
		type = ISipMedia::Null;
		dir = ISipMedia::None;
		state = None;
		index = -1;
	}
	ISipMedia::Type type;
	ISipMedia::Direction dir;
	int index;
	State state;
	ISipMediaFormat format;
};

struct ISipDevice
{
	ISipDevice() {
		type = ISipMedia::Null;
		dir = ISipMedia::None;
		index = -1;
	}
	ISipMedia::Type type;
	ISipMedia::Direction dir;
	int index;
	QString name;
	QList<ISipMediaFormat> formats;

	bool operator==(const ISipDevice &AOther) const {
		return AOther.type==type && AOther.name==name;
	}
	bool operator!=(const ISipDevice &AOther) const {
		return !operator==(AOther);
	}
	bool operator<(const ISipDevice &AOther) const {
		return AOther.type==type ? index<AOther.index : type<AOther.type;
	}
};

class ISipCall
{
public:
	enum Role {
		Caller,
		Receiver
	};
	enum State {
		Inited,
		Calling,
		Ringing,
		Connecting,
		Confirmed,
		Disconnected,
		Aborted
	};
	enum StatusCode {
		SC_Undefined                     = 0,

		SC_Trying                        = 100,
		SC_Ringing                       = 180,
		SC_CallBeingForwarded            = 181,
		SC_Queued                        = 182,
		SC_Progress                      = 183,

		SC_Ok                            = 200,
		SC_Accepted                      = 202,

		SC_MultipleChoices               = 300,
		SC_MovedPermanently              = 301,
		SC_MovedTemporarily              = 302,
		SC_UseProxy                      = 305,
		SC_AlternativeService            = 380,

		SC_BadRequest                    = 400,
		SC_Unauthorized                  = 401,
		SC_PaymentRequired               = 402,
		SC_Forbidden                     = 403,
		SC_NotFound                      = 404,
		SC_MethodNotAllowed              = 405,
		SC_NotAcceptable                 = 406,
		SC_ProxyAuthenticationRequired   = 407,
		SC_RequestTimeout                = 408,
		SC_Gone                          = 410,
		SC_RequestEntityTooLarge         = 413,
		SC_RequestUriTooLong             = 414,
		SC_UnsupportedMediaType          = 415,
		SC_UnsupportedUriScheme          = 416,
		SC_BadExtension                  = 420,
		SC_ExtensionRequired             = 421,
		SC_SesssionTimerTooSmall         = 422,
		SC_IntervalTooBrief              = 423,
		SC_TemporarilyUnavailable        = 480,
		SC_CallTsxDoesNotExist           = 481,
		SC_LoopDetected                  = 482,
		SC_TooManyHops                   = 483,
		SC_AddressIncomplete             = 484,
		SC_Ambiguous                     = 485,
		SC_BusyHere                      = 486,
		SC_RequestTerminated             = 487,
		SC_NotAcceptableHere             = 488,
		SC_BadEvent                      = 489,
		SC_RequestUpdated                = 490,
		SC_RequestPending                = 491,
		SC_Undecipherable                = 493,

		SC_InternalServerError           = 500,
		SC_NotImplemented                = 501,
		SC_BadGateway                    = 502,
		SC_ServiceUnavailable            = 503,
		SC_ServerTimeout                 = 504,
		SC_VersionNotSupported           = 505,
		SC_MessageTooLarge               = 513,
		SC_PreconditionFailure           = 580,

		SC_BusyEverywhere                = 600,
		SC_Decline                       = 603,
		SC_DoesNotExistAnywhere          = 604,
		SC_NotAcceptableAnywhere         = 606
	};
public:
	virtual QObject *instance() =0;
	// Call
	virtual QUuid accountId() const =0;
	virtual QString remoteUri() const =0;
	virtual bool isActive() const =0;
	virtual Role role() const =0;
	virtual State state() const =0;
	virtual quint32 statusCode() const =0;
	virtual QString statusText() const =0;
	virtual quint32 durationTime() const =0;
	virtual bool sendDtmf(const char *ADigits) =0;
	virtual bool startCall(bool AWithVideo = false) =0;
	virtual bool hangupCall(quint32 AStatusCode=SC_Decline, const QString &AText=QString::null) =0;
	virtual bool destroyCall(unsigned long AWaitForDisconnected = ULONG_MAX) =0;
	// Media
	virtual bool hasActiveMediaStream() const =0;
	virtual QList<ISipMediaStream> mediaStreams(ISipMedia::Type AType=ISipMedia::Null) const =0;
	virtual ISipMediaStream findMediaStream(int AMediaIndex) const =0;
	virtual QVariant mediaStreamProperty(int AMediaIndex, ISipMedia::Direction ADir, ISipMediaStream::Property AProperty) const =0;
	virtual bool setMediaStreamProperty(int AMediaIndex, ISipMedia::Direction ADir, ISipMediaStream::Property AProperty, const QVariant &AValue) =0;
	virtual QWidget *getVideoPlaybackWidget(int AMediaIndex, QWidget *AParent) =0;
protected:
	virtual void stateChanged() =0;
	virtual void statusChanged() =0;
	virtual void mediaChanged() =0;
	virtual void callDestroyed() =0;
	virtual void dtmfSent(const char *ADigits) =0;
};

class ISipCallHandler
{
public:
	virtual bool sipCallReceive(int AOrder, ISipCall *ACall) = 0;
};

class ISipPhone
{
public:
	virtual QObject *instance() =0;
	// Calls
	virtual bool isCallsAvailable() const =0;
	virtual bool isAudioCallsAvailable() const =0;
	virtual bool isVideoCallsAvailable() const =0;
	virtual QList<ISipCall *> sipCalls(bool AActiveOnly=false) const =0;
	virtual ISipCall *newCall(const QUuid &AAccountId, const QString &ARemoteUri) =0;
	// Accounts
	virtual QList<QUuid> availAccounts() const =0;
	virtual QString accountUri(const QUuid &AAccountId) const =0;
	virtual ISipAccountConfig accountConfig(const QUuid &AAccountId) const =0;
	virtual bool isAccountRegistered(const QUuid &AAccountId) const =0;
	virtual bool setAccountRegistered(const QUuid &AAccountId, bool ARegistered) =0;
	virtual bool insertAccount(const QUuid &AAccountId, const ISipAccountConfig &AConfig) =0;
	virtual bool updateAccount(const QUuid &AAccountId, const ISipAccountConfig &AConfig) =0;
	virtual void removeAccount(const QUuid &AAccountId) =0;
	// Devices
	virtual bool updateAvailDevices() =0;
	virtual ISipDevice findDevice(ISipMedia::Type AType, int AIndex) const =0;
	virtual ISipDevice findDevice(ISipMedia::Type AType, const QString &AName) const =0;
	virtual ISipDevice defaultDevice(ISipMedia::Type AType, ISipMedia::Direction ADir) const =0;
	virtual QList<ISipDevice> availDevices(ISipMedia::Type AType, ISipMedia::Direction ADir=ISipMedia::None) const =0;
	virtual QWidget *startVideoPreview(const ISipDevice &ADevice, QWidget *AParent) =0;
	virtual void stopVideoPreview(QWidget *APreview) =0;
	// Call Handlers
	virtual QMultiMap<int,ISipCallHandler *> callHandlers() const =0;
	virtual void insertCallHandler(int AOrder, ISipCallHandler *AHandler) =0;
	virtual void removeCallHandler(int AOrder, ISipCallHandler *AHandler) =0;
protected:
	virtual void callsAvailChanged(bool AAvail) =0;
	virtual void availDevicesChanged() =0;
	virtual void callCreated(ISipCall *ACall) =0;
	virtual void callDestroyed(ISipCall *ACall) =0;
	virtual void callStateChanged(ISipCall *ACall) =0;
	virtual void callStatusChanged(ISipCall *ACall) =0;
	virtual void callMediaChanged(ISipCall *ACall) =0;
	virtual void accountInserted(const QUuid &AAccountId) =0;
	virtual void accountChanged(const QUuid &AAccountId) =0;
	virtual void accountRemoved(const QUuid &AAccountId) =0;
	virtual void accountRegistrationChanged(const QUuid &AAccountId, bool ARegistered) =0;
};

Q_DECLARE_INTERFACE(ISipCall,"Vacuum.Plugin.ISipCall/1.0")
Q_DECLARE_INTERFACE(ISipCallHandler,"Vacuum.Plugin.ISipCallHandler/1.0")
Q_DECLARE_INTERFACE(ISipPhone,"Vacuum.Plugin.ISipPhone/1.0")

#endif //ISIPPHONE_H
