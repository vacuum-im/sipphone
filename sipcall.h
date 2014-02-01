#ifndef SIPCALL_H
#define SIPCALL_H

#include <QMutex>
#include <QWaitCondition>
#include <interfaces/isipphone.h>
#include "sipevent.h"
#include "renderdev.h"

class SipCall : 
	public QObject,
	public ISipCall
{
	Q_OBJECT;
	Q_INTERFACES(ISipCall);
	friend class SipPhone;
public:
	SipCall(const QUuid &AAccountId, pjsua_acc_id AAccIndex, const QString &ARemoteUri, QObject *AParent);
	SipCall(const QUuid &AAccountId, pjsua_acc_id AAccIndex, pjsua_call_id ACallIndex, QObject *AParent);
	~SipCall();
	virtual QObject *instance() { return this; }
	// Call
	virtual QUuid accountId() const;
	virtual QString remoteUri() const;
	virtual bool isActive() const;
	virtual Role role() const;
	virtual State state() const;
	virtual quint32 statusCode() const;
	virtual QString statusText() const;
	virtual quint32 durationTime() const;
	virtual bool sendDtmf(const char *ADigits);
	virtual bool startCall(bool AWithVideo = false);
	virtual bool hangupCall(quint32 AStatusCode=SC_Decline, const QString &AText=QString::null);
	virtual bool destroyCall(unsigned long AWaitForDisconnected = ULONG_MAX);
	// Media
	virtual bool hasActiveMediaStream() const;
	virtual ISipMediaStream findMediaStream(int AMediaIndex) const;
	virtual QList<ISipMediaStream> mediaStreams(ISipMedia::Type AType=ISipMedia::Null) const;
	virtual QVariant mediaStreamProperty(int AMediaIndex, ISipMedia::Direction ADir, ISipMediaStream::Property AProperty) const;
	virtual bool setMediaStreamProperty(int AMediaIndex, ISipMedia::Direction ADir, ISipMediaStream::Property AProperty, const QVariant &AValue);
	virtual QWidget *getVideoPlaybackWidget(int AMediaIndex, QWidget *AParent);
signals:
	// Call
	void stateChanged();
	void statusChanged();
	void mediaChanged();
	void callDestroyed();
	void dtmfSent(const char *ADigits);
protected:
	void initialize();
	void initTonegen();
	void setState(State AState);
	void setError(pj_status_t AStatus);
	bool isErrorStatus(pjsip_status_code ACode);
	void setStatus(quint32 ACode, const QString &AText);
	void printCallDump(bool AWithMedia) const;
	void updateVideoPlaybackWidgets(const QList<int> &AMediaIndexes);
protected slots:
	void processSipEvent(SipEvent *AEvent);
protected slots:
	void onVideoPlaybackWidgetDestroyed();
protected:
	void pjcbOnCallState();
	void pjcbOnCallMediaState();
	void pjcbOnCallMediaEvent(unsigned AMediaIndex, pjmedia_event *AEvent);
	inline pjsua_call_id callIndex() const { return FCallIndex; }
	inline pjsua_acc_id accountIndex() const { return FAccIndex; }
private:
	Role FRole;
	State FState;
	QUuid FAccountId;
	QString FRemoteUri;
	quint32 FStatusCode;
	QString FStatusText;
	bool FDelayedDestroy;
	qint64 FDestroyWaitTime;
	quint32 FTotalDurationTime;
private:
	pjsua_acc_id FAccIndex;
	pjsua_call_id FCallIndex;
	mutable QMutex FDestroyLock;
	mutable QWaitCondition FEventWait;
private:
	pj_pool_t *FTonegenPool;
	pjmedia_port *FTonegenPort;
	pjsua_conf_port_id FTonegenSlot;
private:
	QMultiMap<int, VideoWindow *> FVideoPlaybackWidgets;
	QMap<int, QMap<ISipMedia::Direction, QMap<ISipMediaStream::Property,QVariant> > > FStreamProperties;
};

#endif // SIPCALL_H
