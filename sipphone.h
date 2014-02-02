#ifndef SIPPHONE_H
#define SIPPHONE_H

#include <QSet>
#include <QReadWriteLock>
#include <interfaces/ipluginmanager.h>
#include <interfaces/isipphone.h>
#include "sipcall.h"
#include "sipworker.h"

class SipPhone : 
	public QObject,
	public IPlugin,
	public ISipPhone
{
	Q_OBJECT;
	Q_INTERFACES(IPlugin ISipPhone);
public:
	SipPhone();
	~SipPhone();
	//IPlugin
	virtual QObject *instance() { return this; }
	virtual QUuid pluginUuid() const { return SIPPHONE_UUID; }
	virtual void pluginInfo(IPluginInfo *APluginInfo);
	virtual bool initConnections(IPluginManager *APluginManager, int &AInitOrder);
	virtual bool initObjects();
	virtual bool initSettings();
	virtual bool startPlugin() { return true; }
	//ISipPhone
	virtual bool isCallsAvailable() const;
	virtual bool isAudioCallsAvailable() const;
	virtual bool isVideoCallsAvailable() const;
	virtual QList<ISipCall *> sipCalls(bool AActiveOnly=false) const;
	virtual ISipCall *newCall(const QUuid &AAccountId, const QString &ARemoteUri);
	// Accounts
	virtual QList<QUuid> availAccounts() const;
	virtual QString accountUri(const QUuid &AAccountId) const;
	virtual ISipAccountConfig accountConfig(const QUuid &AAccountId) const;
	virtual bool isAccountRegistered(const QUuid &AAccountId) const;
	virtual bool setAccountRegistered(const QUuid &AAccountId, bool ARegistered);
	virtual bool insertAccount(const QUuid &AAccountId, const ISipAccountConfig &AConfig);
	virtual bool updateAccount(const QUuid &AAccountId, const ISipAccountConfig &AConfig);
	virtual void removeAccount(const QUuid &AAccountId);
	// Devices
	virtual bool updateAvailDevices();
	virtual ISipDevice findDevice(ISipMedia::Type AType, int AIndex) const;
	virtual ISipDevice findDevice(ISipMedia::Type AType, const QString &AName) const;
	virtual ISipDevice defaultDevice(ISipMedia::Type AType, ISipMedia::Direction ADir) const;
	virtual QList<ISipDevice> availDevices(ISipMedia::Type AType, ISipMedia::Direction ADir=ISipMedia::None) const;
	virtual QWidget *startVideoPreview(const ISipDevice &ADevice, QWidget *AParent);
	virtual void stopVideoPreview(QWidget *APreview);
	// Call Handlers
	virtual QMultiMap<int,ISipCallHandler *> callHandlers() const;
	virtual void insertCallHandler(int AOrder, ISipCallHandler *AHandler);
	virtual void removeCallHandler(int AOrder, ISipCallHandler *AHandler);
signals:
	void callsAvailChanged(bool AAvail);
	void availDevicesChanged();
	void callCreated(ISipCall *ACall);
	void callDestroyed(ISipCall *ACall);
	void callStateChanged(ISipCall *ACall);
	void callStatusChanged(ISipCall *ACall);
	void callMediaChanged(ISipCall *ACall);
	void accountInserted(const QUuid &AAccountId);
	void accountChanged(const QUuid &AAccountId);
	void accountRemoved(const QUuid &AAccountId);
	void accountRegistrationChanged(const QUuid &AAccountId, bool ARegistered);
protected:
	void initSipStack();
	void loadSipParams();
	void destroySipStack();
	QString resolveSipError(int ACode) const;
	bool isValidConfig(const ISipAccountConfig &AConfig) const;
	bool parseConfig(const ISipAccountConfig &ASrc, pjsua_acc_config &ADst) const;
	bool parseSipUri(const QString &AUri, QString &AAddress, quint16 &APort) const;
	QList<ISipMediaFormat> parseMediaFormats(pjmedia_format AFormats[], int ACount, int AType) const;
protected:
	void appendCall(SipCall *ACall);
	void removeCall(SipCall *ACall);
	bool isDuplicateCall(pjsua_call_id ACallIndex) const;
	SipCall *findCallByIndex(pjsua_call_id ACallIndex) const;
	QList<SipCall *> findCallsByAccount(const QUuid &AAccountId) const;
protected slots:
	void processSipEvent(SipEvent *AEvent);
protected slots:
	void onOptionsOpened();
	void onOptionsClosed();
	void onSipCallDestroyed();
	void onSipCallStateChanged();
	void onSipCallStatusChanged();
	void onSipCallMediaChanged();
	void onVideoPreviewWidgetDestroyed();
	void onSipWorkerTaskFinished(SipTask *ATask);
protected:
	static SipPhone *FInstance;
	static void pjcbOnRegState(pjsua_acc_id AAccIndex);
	static void pjcbOnNatDetect(const pj_stun_nat_detect_result *AResult);
	static void pjcbOnIncomingCall(pjsua_acc_id AAccIndex, pjsua_call_id ACallIndex, pjsip_rx_data *AData);
	static void pjcbOnCallState(pjsua_call_id ACallIndex, pjsip_event *AEvent);
	static void pjcbOnCallMediaState(pjsua_call_id ACallIndex);
	static void pjcbOnCallMediaEvent(pjsua_call_id ACallIndex, unsigned AMediaIndex, pjmedia_event *AEvent);
private:
	IPluginManager *FPluginManager;
private:
	SipWorker *FSipWorker;
	pj_thread_t *FPjThread;
	pj_thread_desc FPjThreadDesc;
private:
	QList<SipCall *> FCalls;
	mutable QReadWriteLock FLock;
private:
	bool FSipStackInited;
	QMap<QUuid, pjsua_acc_id> FAccounts;
	QMultiMap<int, ISipDevice> FAvailDevices;
	QMultiMap<int, ISipCallHandler *> FCallHandlers;
	QMultiMap<int, VideoWindow *> FVideoPreviewWidgets;
};

#endif // SIPPHONE_H
